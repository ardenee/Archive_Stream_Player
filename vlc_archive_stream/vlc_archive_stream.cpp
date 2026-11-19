// Archive Stream Player - VLC 3 native seekable "ast://" access module.
// Original code MIT; VLC SDK headers and all third-party libraries keep their licences.
// Never link or load a separate archive runtime DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
struct pollfd;
extern "C" int poll(struct pollfd*, unsigned, int);
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define VLC_MODULE_COPYRIGHT "Copyright (c) 2026 Archive Stream Player"
#define VLC_MODULE_LICENSE "MIT original code; retained third-party licenses"
#define __PLUGIN__ 1
#define MODULE_NAME archive_stream
#define MODULE_STRING "archive_stream"
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_access.h>
#include <vlc_url.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "archive_format.h"
#include "archive_passwords.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_adapter_tag_probe.h"
#include "../shared/archive_adapter_logging.h"
#include "../shared/archive_unified_cache.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <wincred.h>
#pragma comment(lib, "credui.lib")

namespace
{
enum class Route { None, Stream, Memory, Cache };
struct State
{
    std::wstring archive;
    ArchiveEntry entry;
    ArchiveAdapterPreferences prefs;
    ArchiveBackendStream stream{};
    std::vector<unsigned char> memory;
    HANDLE cache = INVALID_HANDLE_VALUE;
    uint64_t position = 0;
    Route route = Route::None;
};

bool DecodeLocation(std::string_view loc, std::wstring& archive, uint32_t& index)
{
    // ast://<UTF-8 path in ASCII hex>/<unsigned index>/track.wav
    // Hex encoding avoids VLC MRL delimiters, unicode normalization and
    // ambiguous path/entry separators. Only a pre-existing local archive opens.
    const auto slash = loc.find('/');
    if (slash == std::string_view::npos || !slash || (slash & 1) || slash > 8190)
        return false;
    auto digit = [](char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string utf8;
    for (size_t i = 0; i < slash; i += 2)
    {
        const int a = digit(loc[i]), b = digit(loc[i + 1]);
        if (a < 0 || b < 0 || (a == 0 && b == 0))
            return false;
        utf8.push_back(static_cast<char>(a * 16 + b));
    }
    auto suffix = loc.substr(slash + 1);
    auto end = suffix.find('/');
    auto digits = suffix.substr(0, end);
    // "list" is a native M3U8 virtual member. VLC will fetch the playlist
    // and all referenced entries in the same process, preserving verified
    // archive-session credentials without a temporary password file.
    if (digits == "list")
        index = UINT32_MAX;
    else
    {
        if (digits.empty() || digits.size() > 10)
            return false;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size())
            return false;
    }
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars < 3 || chars > 4095)
        return false;
    archive.resize(chars);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), archive.data(), chars) != chars)
        return false;
    if (!(archive[1] == L':' && (archive[2] == L'\\' || archive[2] == L'/')) &&
        !(archive[0] == L'\\' && archive[1] == L'\\'))
        return false;
    return DetectArchiveFormatFromPath(archive.c_str()) != ArchiveFormat::Unknown;
}

bool AskPassword(const char* path, const char*, char* output, size_t capacity, bool* save)
{
    if (!output || !capacity)
        return false;
    output[0] = '\0';
    if (save)
        *save = false;
    const auto prefs = ArchiveAdapterPreferencesLoad();
    if (!prefs.promptPasswords)
        return false;
    CREDUI_INFOW ui{};
    ui.cbSize = sizeof(ui);
    ui.pszCaptionText = L"Archive Stream Player - VLC Password";
    ui.pszMessageText = L"Enter the archive password. Windows will not store it.";
    wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1]{};
    wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
    BOOL persist = FALSE;
    const std::wstring target = ArchiveAdapterUtf8ToWide(path ? path : "");
    const DWORD rc = CredUIPromptForCredentialsW(&ui, target.empty() ? L"Archive Stream" : target.c_str(),
        nullptr, 0, user, CREDUI_MAX_USERNAME_LENGTH + 1, password, CREDUI_MAX_PASSWORD_LENGTH + 1,
        &persist, CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST);
    if (rc != NO_ERROR)
    {
        SecureZeroMemory(password, sizeof(password));
        return false;
    }
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, nullptr, 0, nullptr, nullptr);
    const bool good = count > 1 && static_cast<size_t>(count) <= capacity &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, output, static_cast<int>(capacity), nullptr, nullptr) == count;
    SecureZeroMemory(password, sizeof(password));
    if (!good)
        output[0] = '\0';
    if (good && save && prefs.askToSavePassword)
        *save = MessageBoxW(nullptr, L"If decoding verifies this password, save it to the configured plaintext password list?",
            L"Archive Stream Player - VLC", MB_YESNO | MB_ICONQUESTION) == IDYES;
    return good;
}

void Release(State& s)
{
    if (s.route == Route::Stream)
        ArchiveCloseEntryStream(s.stream);
    if (s.cache != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s.cache);
        s.cache = INVALID_HANDLE_VALUE;
    }
    s.memory.clear();
    s.route = Route::None;
}

bool MemoryOrCache(State& s)
{
    Release(s);
    std::string error;
    if (s.entry.size <= static_cast<uint64_t>(s.prefs.memoryMb) * 1048576ULL &&
        ArchiveExtractEntryToMemory(s.archive.c_str(), s.entry.index, s.memory, error) &&
        s.memory.size() == s.entry.size)
    {
        s.route = Route::Memory;
        DebugLogWrite("INFO", "VLC", "ROUTE_MEMORY", "index=%u | size=%llu", s.entry.index, static_cast<unsigned long long>(s.entry.size));
        return true;
    }
    s.memory.clear();
    std::wstring cached;
    if (!ArchiveUnifiedCache::Prepare(s.archive, s.entry, s.prefs, cached, error))
    {
        DebugLogWrite("WARN", "VLC", "ROUTE_FAIL", "index=%u | reason=%s", s.entry.index, error.c_str());
        return false;
    }
    s.cache = CreateFileW(cached.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (s.cache == INVALID_HANDLE_VALUE)
        return false;
    s.route = Route::Cache;
    DebugLogWrite("INFO", "VLC", "ROUTE_CACHE", "index=%u | size=%llu", s.entry.index, static_cast<unsigned long long>(s.entry.size));
    return true;
}

bool SeekState(State& s, uint64_t position)
{
    if (position > s.entry.size)
        return false;
    if (s.route == Route::Stream)
    {
        ArchivePasswordSetPromptAllowedForCurrentThread(s.prefs.promptPasswords);
        if (!ArchiveSeekEntryStream(s.stream, position) && !MemoryOrCache(s))
            return false;
    }
    if (s.route == Route::Cache)
    {
        LARGE_INTEGER absolute{};
        absolute.QuadPart = static_cast<LONGLONG>(position);
        if (!SetFilePointerEx(s.cache, absolute, nullptr, FILE_BEGIN))
            return false;
    }
    s.position = position;
    return true;
}

ssize_t Read(stream_t* access, void* out, size_t requested)
{
    auto* s = static_cast<State*>(access->p_sys);
    if (!s || !out || !requested || s->position >= s->entry.size)
        return 0;
    const size_t needed = static_cast<size_t>((std::min)(static_cast<uint64_t>(requested), s->entry.size - s->position));
    if (s->route == Route::Stream)
    {
        ArchivePasswordSetPromptAllowedForCurrentThread(s->prefs.promptPasswords);
        const auto got = ArchiveReadEntryStreamDetailed(s->stream, out, needed);
        if (got.bytesRead)
        {
            s->position += got.bytesRead;
            return static_cast<ssize_t>(got.bytesRead);
        }
        if (!MemoryOrCache(*s) || !SeekState(*s, s->position))
            return 0;
    }
    if (s->route == Route::Memory)
    {
        memcpy(out, s->memory.data() + s->position, needed);
        s->position += needed;
        return static_cast<ssize_t>(needed);
    }
    if (s->route == Route::Cache)
    {
        DWORD got = 0;
        if (!ReadFile(s->cache, out, static_cast<DWORD>((std::min)(needed, static_cast<size_t>(0x400000))), &got, nullptr))
            return 0;
        s->position += got;
        return static_cast<ssize_t>(got);
    }
    return 0;
}

int Seek(stream_t* access, uint64_t position)
{
    auto* s = static_cast<State*>(access->p_sys);
    if (!s || !SeekState(*s, position))
        return VLC_EGENERIC;
    DebugLogWrite("INFO", "VLC", "ACCESS_SEEK", "index=%u | offset=%llu | route=%u", s->entry.index, static_cast<unsigned long long>(position), static_cast<unsigned>(s->route));
    return VLC_SUCCESS;
}

int Control(stream_t* access, int query, va_list args)
{
    auto* s = static_cast<State*>(access->p_sys);
    if (!s)
        return VLC_EGENERIC;
    switch (query)
    {
    case STREAM_CAN_SEEK:
    case STREAM_CAN_FASTSEEK:
    case STREAM_CAN_PAUSE:
    case STREAM_CAN_CONTROL_PACE:
        *va_arg(args, bool*) = query != STREAM_CAN_FASTSEEK;
        return VLC_SUCCESS;
    case STREAM_GET_SIZE:
        *va_arg(args, uint64_t*) = s->entry.size;
        return VLC_SUCCESS;
    case STREAM_GET_PTS_DELAY:
        *va_arg(args, int64_t*) = DEFAULT_PTS_DELAY;
        return VLC_SUCCESS;
    case STREAM_SET_PAUSE_STATE:
        return VLC_SUCCESS;
    default:
        return VLC_EGENERIC;
    }
}

int Open(vlc_object_t* object)
{
    auto* access = reinterpret_cast<stream_t*>(object);
    if (!access || !access->psz_location)
        return VLC_EGENERIC;
    std::wstring archive;
    uint32_t index = 0;
    if (!DecodeLocation(access->psz_location, archive, index))
        return VLC_EGENERIC;
    try
    {
        auto s = std::make_unique<State>();
        s->archive = archive;
        s->prefs = ArchiveAdapterPreferencesLoad();
        // Native launcher v1.2 may supply a password-list path for its VLC
        // child process only. The CLI never changes HKCU/shared preferences.
        // Ignore the environment unless a complete absolute path is given;
        // an invalid override MUST fail closed, not fall back to the user's
        // real password list and persist a password in the wrong location.
        const DWORD overrideChars = GetEnvironmentVariableW(L"ARCHIVE_STREAM_VLC_PASSWORD_LIST", nullptr, 0);
        if (overrideChars)
        {
            if (overrideChars > 4096)
                return VLC_EGENERIC;
            std::wstring overridePath(static_cast<size_t>(overrideChars), L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"ARCHIVE_STREAM_VLC_PASSWORD_LIST", overridePath.data(), overrideChars);
            if (!copied || copied >= overrideChars)
                return VLC_EGENERIC;
            overridePath.resize(copied);
            const bool drivePath = overridePath.size() >= 3 &&
                ((overridePath[0] >= L'A' && overridePath[0] <= L'Z') || (overridePath[0] >= L'a' && overridePath[0] <= L'z')) &&
                overridePath[1] == L':' && (overridePath[2] == L'\\' || overridePath[2] == L'/');
            const bool networkPath = overridePath.size() >= 5 && overridePath[0] == L'\\' && overridePath[1] == L'\\';
            if (!drivePath && !networkPath)
                return VLC_EGENERIC;
            s->prefs.passwordListPath = std::move(overridePath);
        }
        // v1.3: scoped native memory/cache/title overrides from the launcher.
        // Validate rather than clamp or inherit an invalid environment value.
        // This does not modify any persistent shared setting.
        const auto numberOverride = [](const wchar_t* name, unsigned minimum, unsigned maximum, unsigned& result)
        {
            wchar_t chars[32]{};
            const DWORD read = GetEnvironmentVariableW(name, chars, static_cast<DWORD>(std::size(chars)));
            if (!read)
                return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
            if (read >= std::size(chars))
                return false;
            unsigned value = 0;
            for (DWORD i = 0; i < read; ++i)
            {
                if (chars[i] < L'0' || chars[i] > L'9')
                    return false;
                const unsigned digit = static_cast<unsigned>(chars[i] - L'0');
                if (value > (maximum - (std::min)(digit, maximum)) / 10)
                    return false;
                value = value * 10 + digit;
                if (value > maximum)
                    return false;
            }
            if (value < minimum)
                return false;
            result = value;
            return true;
        };
        if (!numberOverride(L"ARCHIVE_STREAM_VLC_MEMORY_MB", 1, 512, s->prefs.memoryMb) ||
            !numberOverride(L"ARCHIVE_STREAM_VLC_CACHE_LIMIT_MB", 0, 20480, s->prefs.cacheLimitMb))
            return VLC_EGENERIC;
        const DWORD titleChars = GetEnvironmentVariableW(L"ARCHIVE_STREAM_VLC_TITLE_TEMPLATE", nullptr, 0);
        if (titleChars)
        {
            if (titleChars > 4096)
                return VLC_EGENERIC;
            std::wstring format(static_cast<size_t>(titleChars), L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"ARCHIVE_STREAM_VLC_TITLE_TEMPLATE", format.data(), titleChars);
            if (!copied || copied >= titleChars)
                return VLC_EGENERIC;
            format.resize(copied);
            if (format != L"<filename>" && format != L"<archivetype>: <filename>" &&
                format != L"<#>. <filename>" && format != L"<filename> [<filetype>]" &&
                format != L"<archivetype>: <#> - <filename>" &&
                format != L"[<archive>] <#> - <filename>")
                return VLC_EGENERIC;
            s->prefs.titleTemplate = std::move(format);
        }
        ArchiveSetDataDirectoryName("VLCArchiveStream");
        char debug[8]{};
        const bool forced = GetEnvironmentVariableA("ARCHIVE_STREAM_ADAPTER_DEBUG", debug, sizeof(debug)) == 1 && debug[0] == '1';
        ArchiveAdapterLogging::Configure(L"vlc", s->prefs.debugLogging || forced);
        DebugLogWrite("INFO", "VLC", "NATIVE_PREFS", "memoryMb=%u | cacheLimitMb=%u | titleTemplate=%s",
            s->prefs.memoryMb, s->prefs.cacheLimitMb, ArchiveAdapterWideToUtf8(s->prefs.titleTemplate).c_str());
        ArchivePasswordSetPromptCallback(s->prefs.promptPasswords ? AskPassword : nullptr);
        ArchivePasswordSetPromptAllowedForCurrentThread(s->prefs.promptPasswords);
        if (!s->prefs.passwordListPath.empty())
        {
            const auto path = ArchiveAdapterWideToUtf8(s->prefs.passwordListPath);
            if (!path.empty())
                ArchivePasswordSetListPath(path.c_str());
        }
        std::vector<ArchiveEntry> entries;
        std::string error;
        if (!ArchiveListEntries(archive.c_str(), entries, error))
            return VLC_EGENERIC;
        if (index == UINT32_MAX)
        {
            // VLC opens this as an ordinary playlist, then invokes this same
            // module for every track in the current process and session.
            const std::string_view location(access->psz_location);
            const auto split = location.find('/');
            const std::string prefix = "ast://" + std::string(location.substr(0, split));
            std::string playlist = "#EXTM3U\r\n";
            unsigned selected = 0;
            for (const auto& entry : entries)
            {
                if (entry.isFolder || !entry.isSupportedAudio || entry.size == 0)
                    continue;
                const std::string filename = entry.path;
                ArchiveTrackTags tags{};
                const bool tagged = s->prefs.useFileTags && ArchiveAdapterProbeEntryTags(archive, entry, tags);
                auto title = ArchiveAdapterWideToUtf8(ArchiveAdapterPreferredTitle(archive,
                    ArchiveAdapterUtf8ToWide(filename), entry.index + 1, s->prefs,
                    tagged ? ArchiveAdapterUtf8ToWide(tags.title) : std::wstring()));
                if (title.empty())
                    title = filename;
                for (char& ch : title)
                {
                    if (ch == '\r' || ch == '\n' || ch == '\0')
                        ch = ' ';
                }
                std::string ext;
                const auto dot = filename.find_last_of('.');
                if (dot != std::string::npos && filename.size() - dot <= 10)
                {
                    bool valid = true;
                    for (size_t i = dot; i < filename.size(); ++i)
                    {
                        const char ch = filename[i];
                        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                              (ch >= '0' && ch <= '9') || ch == '.'))
                            valid = false;
                    }
                    if (valid)
                        ext = filename.substr(dot);
                }
                DebugLogWrite("INFO", "VLC", "NATIVE_PLAYLIST_TITLE", "index=%u | title=%s", entry.index, title.c_str());
                playlist += "#EXTINF:-1," + title + "\r\n";
                playlist += prefix + "/" + std::to_string(entry.index) + "/track" + ext + "\r\n";
                ++selected;
            }
            if (!selected || playlist.size() > 16 * 1024 * 1024)
                return VLC_EGENERIC;
            s->entry.index = UINT32_MAX;
            s->entry.size = playlist.size();
            s->memory.assign(playlist.begin(), playlist.end());
            s->route = Route::Memory;
            access->pf_read = Read;
            access->pf_block = nullptr;
            access->pf_seek = Seek;
            access->pf_control = Control;
            access->p_sys = s.release();
            DebugLogWrite("INFO", "VLC", "NATIVE_PLAYLIST_OPEN", "entries=%u | bytes=%llu", selected, static_cast<unsigned long long>(playlist.size()));
            return VLC_SUCCESS;
        }
        const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& e)
        {
            return e.index == index && !e.isFolder && e.isSupportedAudio && e.size != 0;
        });
        if (found == entries.end())
            return VLC_EGENERIC;
        s->entry = *found;
        // libarchive can enumerate encrypted RAR5 but cannot read its encrypted
        // data stream. Avoid a dead-end open and a second credential prompt:
        // use the supported bounded-RAM UnRAR decode path immediately.
        const bool encryptedRar = s->entry.isEncrypted && DetectArchiveFormatFromPath(archive.c_str()) == ArchiveFormat::Rar;
        if (!s->entry.isSolid && !encryptedRar && ArchiveOpenEntryStream(archive.c_str(), index, s->stream, error))
        {
            s->route = Route::Stream;
            DebugLogWrite("INFO", "VLC", "ROUTE_STREAM", "index=%u | bytes=%llu", index, static_cast<unsigned long long>(s->entry.size));
        }
        else if (!MemoryOrCache(*s))
            return VLC_EGENERIC;
        access->pf_read = Read;
        access->pf_block = nullptr;
        access->pf_seek = Seek;
        access->pf_control = Control;
        access->p_sys = s.release();
        DebugLogWrite("INFO", "VLC", "ACCESS_OPEN_OK", "index=%u", index);
        return VLC_SUCCESS;
    }
    catch (...)
    {
        return VLC_EGENERIC;
    }
}

void Close(vlc_object_t* object)
{
    auto* access = reinterpret_cast<stream_t*>(object);
    auto* s = static_cast<State*>(access->p_sys);
    if (s)
    {
        Release(*s);
        DebugLogWrite("INFO", "VLC", "ACCESS_CLOSE", "index=%u", s->entry.index);
        delete s;
        access->p_sys = nullptr;
    }
}
// Claim only local supported archive files from VLC's ordinary File/Open/Drop path.
// Non-archive input returns VLC_EGENERIC so VLC's built-in file access
// continues to handle MP3s, videos, directories and all unrelated files.
// VLC performs this redirect in-process: no PowerShell, HTTP or temp playlist.
int OpenLocalArchive(vlc_object_t* object)
{
    auto* access = reinterpret_cast<stream_t*>(object);
    if (!access || !access->psz_filepath || !access->psz_name ||
        _stricmp(access->psz_name, "file") != 0)
        return VLC_EGENERIC;
    const std::wstring archive = ArchiveAdapterUtf8ToWide(access->psz_filepath);
    if (archive.size() < 4 || archive.size() > 4095 ||
        DetectArchiveFormatFromPath(archive.c_str()) == ArchiveFormat::Unknown)
        return VLC_EGENERIC;
    const DWORD attributes = GetFileAttributesW(archive.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return VLC_EGENERIC;

    const std::string utf8 = ArchiveAdapterWideToUtf8(archive);
    if (utf8.empty() || utf8.size() > 4095)
        return VLC_EGENERIC;
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string url = "ast://";
    url.reserve(utf8.size() * 2 + 22);
    for (unsigned char byte : utf8)
    {
        url += hex[byte >> 4];
        url += hex[byte & 15];
    }
    url += "/list/album.m3u8";
    // VLC 3 for Windows is built against MinGW/MSVCRT; the host frees
    // redirected URLs with its own CRT. Allocate through VLC, never _strdup
    // from our MSVC/UCRT DLL, to avoid cross-CRT heap corruption.
    const HMODULE vlcCore = GetModuleHandleW(L"libvlccore.dll");
    using VlcDuplicateUri = char* (__cdecl*)(const char*);
    const auto duplicateUri = vlcCore ? reinterpret_cast<VlcDuplicateUri>(GetProcAddress(vlcCore, "vlc_uri_decode_duplicate")) : nullptr;
    if (!duplicateUri)
        return VLC_EGENERIC;
    char* redirect = duplicateUri(url.c_str());
    if (!redirect)
        return VLC_ENOMEM;
    access->psz_url = redirect;
    DebugLogWrite("INFO", "VLC", "LOCAL_ARCHIVE_REDIRECT", "archive=%s | native=1 | http=0", utf8.c_str());
    return VLC_ACCESS_REDIRECT;
}

void CloseLocalArchive(vlc_object_t*) {}

} // namespace

vlc_module_begin()
    set_shortname("Archive Stream Player")
    set_description("Archive Stream native ZIP/7z/RAR seekable access")
    set_capability("access", 210)
    add_shortcut("ast")
    set_callbacks(Open, Close)

    // Higher priority than VLC's built-in file access (50), but only accept
    // actual local archives; all other file types fall back to VLC unchanged.
    add_submodule()
    set_shortname("Archive Stream local archives")
    set_description("Open ZIP/7z/RAR directly through VLC Open File and drag-and-drop")
    set_capability("access", 60)
    add_shortcut("file")
    set_callbacks(OpenLocalArchive, CloseLocalArchive)
vlc_module_end()
