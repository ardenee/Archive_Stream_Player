// Archive Stream Player: native VLC 3 M3U8 playlist builder.
// This process only enumerates archive metadata; VLC's native access DLL
// opens each selected entry. No HTTP server and no decoded audio files.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_adapter_logging.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#pragma comment(lib, "credui.lib")

namespace
{
bool AskPassword(const char* path, const char*, char* result, size_t capacity, bool* save)
{
    if (!result || !capacity)
        return false;
    result[0] = '\0';
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
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, nullptr, 0, nullptr, nullptr);
    const bool good = needed > 1 && static_cast<size_t>(needed) <= capacity &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, result, static_cast<int>(capacity), nullptr, nullptr) == needed;
    SecureZeroMemory(password, sizeof(password));
    if (!good)
        result[0] = '\0';
    if (good && save && prefs.askToSavePassword)
        *save = MessageBoxW(nullptr, L"Save this verified password in your configured plaintext list?",
            L"Archive Stream Player - VLC", MB_YESNO | MB_ICONQUESTION) == IDYES;
    return good;
}

std::string EncodeUri(const std::wstring& archive, const ArchiveEntry& entry)
{
    const std::string utf8 = ArchiveAdapterWideToUtf8(archive);
    if (utf8.empty() || utf8.size() > 4095)
        return {};
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "ast://";
    for (unsigned char byte : utf8)
    {
        result += hex[byte >> 4];
        result += hex[byte & 0xf];
    }
    result += "/" + std::to_string(entry.index) + "/track";
    const auto slash = entry.path.find_last_of("/\\");
    const auto dot = entry.path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
        entry.path.size() - dot < 12)
    {
        bool valid = true;
        for (size_t i = dot; i < entry.path.size(); ++i)
        {
            const char c = entry.path[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.'))
                valid = false;
        }
        if (valid)
            result += entry.path.substr(dot);
    }
    return result;
}

std::string SafeTitle(const std::wstring& archive, const ArchiveEntry& entry, const ArchiveAdapterPreferences& settings)
{
    std::wstring title = ArchiveAdapterFormatTitle(archive, ArchiveAdapterUtf8ToWide(entry.path),
        entry.index + 1, settings.titleTemplate);
    if (title.empty())
        title = ArchiveAdapterUtf8ToWide(entry.path);
    std::string utf8 = ArchiveAdapterWideToUtf8(title);
    for (char& c : utf8)
    {
        if (c == '\r' || c == '\n' || c == '\0')
            c = ' ';
    }
    return utf8;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5 || wcscmp(argv[1], L"--archive") || wcscmp(argv[3], L"--playlist"))
    {
        fwprintf(stderr, L"Usage: vlc_native_playlist.exe --archive ARCHIVE --playlist OUTPUT.m3u8\n");
        return 2;
    }
    const std::wstring archive = argv[2];
    const std::filesystem::path destination = argv[4];
    const auto prefs = ArchiveAdapterPreferencesLoad();
    ArchiveSetDataDirectoryName("VLCArchiveStream");
    char debug[8]{};
    const bool forced = GetEnvironmentVariableA("ARCHIVE_STREAM_ADAPTER_DEBUG", debug, sizeof(debug)) == 1 && debug[0] == '1';
    ArchiveAdapterLogging::Configure(L"vlc_playlist", prefs.debugLogging || forced);
    if (!prefs.passwordListPath.empty())
    {
        const std::string utf8 = ArchiveAdapterWideToUtf8(prefs.passwordListPath);
        if (!utf8.empty())
            ArchivePasswordSetListPath(utf8.c_str());
    }
    ArchivePasswordSetPromptCallback(prefs.promptPasswords ? AskPassword : nullptr);
    ArchivePasswordSetPromptAllowedForCurrentThread(prefs.promptPasswords);
    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archive.c_str(), entries, error))
    {
        fprintf(stderr, "NATIVE_VLC_LIST_FAIL: %s\n", error.c_str());
        return 3;
    }

    std::string playlist = "#EXTM3U\r\n";
    unsigned count = 0;
    for (const auto& entry : entries)
    {
        if (entry.isFolder || !entry.isSupportedAudio)
            continue;
        const std::string uri = EncodeUri(archive, entry);
        if (uri.empty())
            continue;
        playlist += "#EXTINF:-1," + SafeTitle(archive, entry, prefs) + "\r\n";
        playlist += uri + "\r\n";
        ++count;
    }
    if (!count)
    {
        fputs("NATIVE_VLC_NO_AUDIO_ENTRIES\n", stderr);
        return 4;
    }
    std::error_code ec;
    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
        return 5;
    // The playlist contains source paths, indexes and display names, never keys
    // or decoded audio. Stage it atomically to avoid partially read playlists.
    const std::filesystem::path temporary = destination.native() + L".partial";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(playlist.data(), static_cast<std::streamsize>(playlist.size()));
        if (!output.good())
        {
            std::filesystem::remove(temporary, ec);
            return 6;
        }
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return 7;
    }
    printf("NATIVE_VLC_PLAYLIST_ENTRIES=%u\n", count);
    printf("NATIVE_VLC_PLAYLIST=%s\n", ArchiveAdapterWideToUtf8(destination.wstring()).c_str());
    return 0;
}
