// Native BASS archive PCM decoder for x86 MusicBee.
// Proprietary BASS SDK headers/libs are external build-time inputs and must
// never be redistributed. No HTTP server or runtime vendor DLL is bundled.
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include "archive_backend.h"

#include "bass-addon.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "archive_passwords.h"
#include "native_track_manifest.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"
#include "mf_audio_decoder.h"
#include "archive_adapter_preferences.h"
#include "archive_track_tags.h"
#include "archive_unified_cache.h"

const BASS_FUNCTIONS* bassfunc = nullptr;
const BASS_FUNCTIONS2* bassfunc2 = nullptr;
static constexpr DWORD kArchiveCtype = 0x10A50;
static constexpr DWORD kFormatVersion = 0x02040000;

static void Trace(const char* event, const char* note)
{
    char file[MAX_PATH * 4]{};
    const DWORD n = GetEnvironmentVariableA("AST_BASS_ADDON_LOG", file, static_cast<DWORD>(sizeof(file)));
    if (!n || n >= sizeof(file) || !event)
        return;
    HANDLE handle = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    char line[320]{};
    int length = sprintf_s(line, "AST_BASS %s %s\r\n", event, note ? note : "");
    if (length > 0)
    {
        DWORD written = 0;
        WriteFile(handle, line, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(handle);
}

// MusicBee owns the native UI. Archive backends already own password
// verification, process-local reuse and verified-only keeps.txt persistence.
// No password ever appears in logs or the .ast reference file.
static thread_local bool gNativePasswordAttempted = false;
static thread_local bool gNativePasswordCancelled = false;
static std::mutex gNativePasswordSaveMutex;
static std::unordered_map<std::string, std::string> gNativePasswordSavePending;

static void MusicBeePendingPasswordSave(const char* archive, const char* password, bool consent)
{
    if (!archive || !*archive || !password || !*password || !consent)
        return;
    std::lock_guard<std::mutex> guard(gNativePasswordSaveMutex);
    auto& pending = gNativePasswordSavePending[archive];
    if (!pending.empty())
        SecureZeroMemory(pending.data(), pending.size());
    pending.assign(password);
}

static void MusicBeeForgetPendingPassword(const char* archive)
{
    if (!archive || !*archive)
        return;
    std::lock_guard<std::mutex> guard(gNativePasswordSaveMutex);
    auto entry = gNativePasswordSavePending.find(archive);
    if (entry != gNativePasswordSavePending.end())
    {
        if (!entry->second.empty())
            SecureZeroMemory(entry->second.data(), entry->second.size());
        gNativePasswordSavePending.erase(entry);
    }
}

// Call only after a real PCM decode succeeds. The shared backend verifies
// archive credentials, but encrypted-header listing ALONE is insufficient.
static void MusicBeeCommitVerifiedPassword(const char* archive)
{
    if (!archive || !*archive)
        return;
    std::string confirmed;
    {
        std::lock_guard<std::mutex> guard(gNativePasswordSaveMutex);
        auto item = gNativePasswordSavePending.find(archive);
        if (item == gNativePasswordSavePending.end())
            return;
        confirmed = std::move(item->second);
        gNativePasswordSavePending.erase(item);
    }
    if (confirmed.empty())
        return;
    std::string error;
    const bool stored = ArchivePasswordAppendIfMissing(confirmed.c_str(), error);
    Trace(stored ? "NATIVE_PASSWORD_VERIFIED_SAVED" : "NATIVE_PASSWORD_SAVE_FAILED",
        "proof=pcm_read");
    SecureZeroMemory(confirmed.data(), confirmed.size());
}

static bool MusicBeePromptPassword(const char* archive, const char* entry, char* output,
    size_t capacity, bool* save)
{
    (void)entry;
    if (!output || capacity < 2 || !save)
        return false;
    output[0] = '\0';
    *save = false;
    const auto settings = ArchiveAdapterPreferencesLoad();
    if (!settings.promptPasswords)
    {
        Trace("NATIVE_PASSWORD_DISABLED", "reason=shared_settings");
        return false;
    }
    gNativePasswordAttempted = true;
#if defined(AST_NATIVE_TEST_HOOKS)
    // Only isolated x86 test DLLs can return synthetic credentials without
    // opening UI. Production DLLs cannot read any of these environment keys.
    char testPassword[256]{};
    const DWORD n = GetEnvironmentVariableA("AST_NATIVE_TEST_PASSWORD", testPassword, sizeof(testPassword));
    if (n > 0 && n < sizeof(testPassword))
    {
        char mode[16]{};
        GetEnvironmentVariableA("AST_NATIVE_TEST_PASSWORD_MODE", mode, sizeof(mode));
        if (_stricmp(mode, "cancel") == 0)
        {
            gNativePasswordCancelled = true;
            SecureZeroMemory(testPassword, sizeof(testPassword));
            Trace("NATIVE_PASSWORD_CANCELLED", "source=test_only");
            return false;
        }
        const bool accepted = strcpy_s(output, capacity, testPassword) == 0;
        MusicBeePendingPasswordSave(archive, testPassword,
            accepted && settings.askToSavePassword && _stricmp(mode, "save") == 0);
        *save = false;
        SecureZeroMemory(testPassword, sizeof(testPassword));
        Trace(accepted ? "NATIVE_PASSWORD_ENTERED" : "NATIVE_PASSWORD_REJECTED", "source=test_only");
        return accepted;
    }
#endif
    wchar_t username[CREDUI_MAX_USERNAME_LENGTH + 1]{};
    wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
    const std::wstring label = archive ? ArchiveAdapterUtf8ToWide(archive) : L"";
    std::wstring display = label.empty() ? L"the selected archive" :
        std::filesystem::path(label).filename().wstring();
    if (display.size() > 128)
        display.resize(128);
    for (wchar_t& character : display)
    {
        if (character < 32)
            character = L' ';
    }
    const std::wstring message = L"Enter the password for " + display +
        L". Saving is optional and writes a verified password to your configured plain-text password list.";
    CREDUI_INFOW ui{};
    ui.cbSize = sizeof(ui);
    ui.pszCaptionText = L"Archive Stream Player - Password Required";
    ui.pszMessageText = message.c_str();
    BOOL shouldSave = FALSE;
    const DWORD flags = CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_GENERIC_CREDENTIALS |
        CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_SHOW_SAVE_CHECK_BOX;
    Trace("NATIVE_PASSWORD_PROMPT", "source=windows_credui");
    const DWORD response = CredUIPromptForCredentialsW(&ui, L"Archive Stream Player",
        nullptr, 0, username, static_cast<ULONG>(std::size(username)),
        password, static_cast<ULONG>(std::size(password)), &shouldSave, flags);
    bool accepted = false;
    if (response == NO_ERROR && password[0])
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            password, -1, output, static_cast<int>(capacity), nullptr, nullptr);
        accepted = bytes > 1;
        if (accepted)
            MusicBeePendingPasswordSave(archive, output,
                settings.askToSavePassword && shouldSave != FALSE);
        // The backend must not persist on header-only validation.
        *save = false;
    }
    if (!accepted)
    {
        gNativePasswordCancelled = true;
        SecureZeroMemory(output, capacity);
    }
    SecureZeroMemory(username, sizeof(username));
    SecureZeroMemory(password, sizeof(password));
    Trace(accepted ? "NATIVE_PASSWORD_ENTERED" : "NATIVE_PASSWORD_CANCELLED", "source=windows_credui");
    return accepted;
}

static void MusicBeeInitializePasswordBackend()
{
    const auto settings = ArchiveAdapterPreferencesLoad();
    std::string list = ArchiveAdapterWideToUtf8(settings.passwordListPath);
#if defined(AST_NATIVE_TEST_HOOKS)
    wchar_t testList[MAX_PATH * 4]{};
    const DWORD n = GetEnvironmentVariableW(L"AST_NATIVE_TEST_PASSWORD_LIST",
        testList, std::size(testList));
    if (n > 0 && n < std::size(testList) && std::filesystem::path(testList).is_absolute())
        list = ArchiveAdapterWideToUtf8(testList);
#endif
    if (!list.empty())
        ArchivePasswordSetListPath(list.c_str());
    ArchivePasswordSetPromptCallback(MusicBeePromptPassword);
}

struct ArchivePcm
{
    HSTREAM handle = 0;
    BASSFILE descriptor = nullptr;
    ArchiveStreamContext stream{};
    std::vector<unsigned char> encoded;
    drmp3 decoder{};
    drwav wav{};
    drflac* flac = nullptr;
    MfAudioDecoder* mf = nullptr;
    bool mfArchiveStream = false;
    bool diskCached = false;
    bool recoveryAttempted = false;
    std::string archivePathUtf8;
    mz_uint entryIndex = 0;
    uint64_t pcmFrameCursor = 0;
    std::wstring cachedEncodedPath;
    HSTREAM hostStream = 0;
    FILE* cachedEncodedFile = nullptr; // BASS file-user callback retains compressed OGG cache
    uint64_t encodedSize = 0;
    size_t memoryCursor = 0;
    AudioFormat format = AUDIO_FORMAT_MP3;
    bool opened = false;
    bool isStreaming = false;
    bool firstRead = false;
    DWORD channels = 0;
    DWORD sampleRate = 0;
    QWORD pcmBytes = 0;
    std::vector<char> displayTitleTags; // BASS TAG_* UTF-8 metadata, double-NUL
    std::vector<unsigned char> displayId3; // Verified ID3 metadata alias, BASS_TAG_ID3V2

    static const ADDON_FUNCTIONS callbacks;
    static void WINAPI Free(void* ptr);
    static QWORD WINAPI Length(void* ptr, DWORD mode);
    static const char* WINAPI GetTags(void* ptr, DWORD type);
    static void WINAPI Info(void* ptr, BASS_CHANNELINFO* info);
    static BOOL WINAPI CanSeek(void* ptr, QWORD pos, DWORD mode);
    static QWORD WINAPI Seek(void* ptr, QWORD pos, DWORD mode);
    static DWORD CALLBACK ReadPcm(HSTREAM handle, void* buffer, DWORD length, void* ptr);
};

#if defined(AST_NATIVE_TEST_HOOKS)
static thread_local bool gInjectPlaybackReadFailure = false;
#endif

static size_t ReadEncoded(void* context, void* output, size_t length)
{
    auto* stream = static_cast<ArchiveStreamContext*>(context);
#if defined(AST_NATIVE_TEST_HOOKS)
    // Fault injection exists only in a deliberately instrumented QA build.
    // Production DLLs are never compiled with AST_NATIVE_TEST_HOOKS.
    wchar_t text[32]{};
    const DWORD n = GetEnvironmentVariableW(L"AST_NATIVE_TEST_STREAM_FAIL_AFTER", text, std::size(text));
    if (gInjectPlaybackReadFailure && n > 0 && n < std::size(text))
    {
        const uint64_t cutoff = _wcstoui64(text, nullptr, 10);
        if (cutoff >= 4096 && stream->position >= cutoff)
            return 0;
    }
#endif
    return ArchiveReadEntryStream(stream, output, length);
}

static drmp3_bool32 SeekEncoded(void* context, int offset, drmp3_seek_origin origin)
{
    auto* stream = static_cast<ArchiveStreamContext*>(context);
    const int64_t current = static_cast<int64_t>(stream->position);
    const int64_t position = origin == DRMP3_SEEK_SET ? offset : current + offset;
    if (position < 0)
        return DRMP3_FALSE;
    return ArchiveSeekEntryStream(stream, static_cast<mz_uint64>(position)) ? DRMP3_TRUE : DRMP3_FALSE;
}

static drmp3_bool32 TellEncoded(void* context, drmp3_int64* position)
{
    if (!context || !position)
        return DRMP3_FALSE;
    *position = static_cast<drmp3_int64>(static_cast<ArchiveStreamContext*>(context)->position);
    return DRMP3_TRUE;
}

static drwav_bool32 WavSeekEncoded(void* context, int offset, drwav_seek_origin origin)
{
    // The archive reader has no generic SEEK_END callback. Reject it rather
    // than pretending it means SEEK_CUR: dr_wav would mismeasure the file and
    // clamp the WAV data chunk to zero.
    if (origin == DRWAV_SEEK_END)
        return DRWAV_FALSE;
    auto* stream = static_cast<ArchiveStreamContext*>(context);
    const int64_t current = static_cast<int64_t>(stream->position);
    const int64_t position = origin == DRWAV_SEEK_SET ? static_cast<int64_t>(offset) : current + offset;
    if (position < 0)
        return DRWAV_FALSE;
    return ArchiveSeekEntryStream(stream, static_cast<mz_uint64>(position)) ? DRWAV_TRUE : DRWAV_FALSE;
}

static drwav_bool32 WavTellEncoded(void* context, drwav_int64* position)
{
    if (!context || !position)
        return DRWAV_FALSE;
    *position = static_cast<drwav_int64>(static_cast<ArchiveStreamContext*>(context)->position);
    return DRWAV_TRUE;
}

static drflac_bool32 FlacSeekEncoded(void* context, int offset, drflac_seek_origin origin)
{
    if (!context || origin == DRFLAC_SEEK_END)
        return DRFLAC_FALSE;
    auto* stream = static_cast<ArchiveStreamContext*>(context);
    const int64_t base = origin == DRFLAC_SEEK_SET ? 0 : static_cast<int64_t>(stream->position);
    const int64_t position = base + static_cast<int64_t>(offset);
    if (position < 0)
        return DRFLAC_FALSE;
    return ArchiveSeekEntryStream(stream, static_cast<mz_uint64>(position)) ? DRFLAC_TRUE : DRFLAC_FALSE;
}

static drflac_bool32 FlacTellEncoded(void* context, drflac_int64* position)
{
    if (!context || !position)
        return DRFLAC_FALSE;
    *position = static_cast<drflac_int64>(static_cast<ArchiveStreamContext*>(context)->position);
    return DRFLAC_TRUE;
}

// Reuse BASS's licensed, already-installed Ogg/Vorbis decoder with the
// shared archive backend as a true seekable custom compressed-byte source.
// File I/O ownership remains with ArchivePcm for both streaming and RAM.
static void CALLBACK EncodedFileClose(void* user)
{
    auto* state = static_cast<ArchivePcm*>(user);
    if (!state)
        return;
    if (state->isStreaming)
    {
        ArchiveCloseEntryStream(&state->stream);
        state->isStreaming = false;
    }
    if (state->cachedEncodedFile)
    {
        fclose(state->cachedEncodedFile);
        state->cachedEncodedFile = nullptr;
    }
}

static QWORD CALLBACK EncodedFileLength(void* user)
{
    return static_cast<ArchivePcm*>(user)->encodedSize;
}

// Called from BASS's compressed OGG file-user callbacks. Preserve the
// existing Vorbis decoder/PCM timeline: move the encoded byte source at the
// exact requested offset rather than restarting BASS at an approximate time.
// Do not open another BASS decoder recursively from within this callback.
static bool PrepareDiskLastAudio(const wchar_t* archivePath, const ArchiveEntry& entry,
    const ArchiveAdapterPreferences& settings, std::wstring& cachePath, std::string& error);

static bool RecoverOggEncodedSource(ArchivePcm* state, uint64_t offset)
{
    if (!state || state->format != AUDIO_FORMAT_OGG || !state->isStreaming ||
        !state->firstRead || state->recoveryAttempted || state->archivePathUtf8.empty() ||
        !state->encodedSize || offset >= state->encodedSize)
        return false;

    state->recoveryAttempted = true;
    Trace("RECOVERY_START", "codec=OGG source=BASS_FILEUSER");
    ArchiveAdapterPreferences preferences = ArchiveAdapterPreferencesLoad();
#if defined(AST_NATIVE_TEST_HOOKS)
    // Only test builds can force an otherwise memory-fitting OGG through
    // the cache route. This is never part of production routing.
    wchar_t overrideText[32]{};
    const DWORD chars = GetEnvironmentVariableW(L"AST_NATIVE_TEST_MEMORY_MB", overrideText, std::size(overrideText));
    if (chars > 0 && chars < std::size(overrideText))
    {
        const unsigned candidate = static_cast<unsigned>(_wcstoui64(overrideText, nullptr, 10));
        if (candidate >= 1 && candidate <= preferences.memoryMb)
            preferences.memoryMb = candidate;
    }
#endif
    std::string error;
    if (state->encodedSize <= static_cast<uint64_t>(preferences.memoryMb) * 1024u * 1024u)
    {
        std::vector<unsigned char> recovered;
        if (ArchiveExtractEntryToMemory(state->archivePathUtf8.c_str(), state->entryIndex, recovered, error) &&
            recovered.size() == state->encodedSize)
        {
            ArchiveCloseEntryStream(&state->stream);
            state->isStreaming = false;
            state->encoded = std::move(recovered);
            state->memoryCursor = static_cast<size_t>(offset);
            state->diskCached = false;
            Trace("RECOVERY_OK", "codec=OGG route=BUFFER");
            return true;
        }
        Trace("RECOVERY_BUFFER_FAIL", "codec=OGG");
    }

    wchar_t archiveWide[MAX_PATH * 4]{};
    ArchiveEntry entry;
    std::wstring path;
    if (!CharPathToWidePath(state->archivePathUtf8.c_str(), archiveWide, std::size(archiveWide)) ||
        !ArchiveUnifiedCache::LookupEntry(archiveWide, state->entryIndex, entry, error) ||
        entry.size != state->encodedSize ||
        !PrepareDiskLastAudio(archiveWide, entry, preferences, path, error))
    {
        Trace("RECOVERY_FAIL", "codec=OGG reason=cache_unavailable");
        return false;
    }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file ||
        offset > static_cast<uint64_t>(INT64_MAX) ||
        _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) != 0)
    {
        if (file)
            fclose(file);
        Trace("RECOVERY_FAIL", "codec=OGG reason=cache_seek");
        return false;
    }
    ArchiveCloseEntryStream(&state->stream);
    state->isStreaming = false;
    state->cachedEncodedFile = file;
    state->cachedEncodedPath = path;
    state->diskCached = true;
    Trace("RECOVERY_OK", "codec=OGG route=CACHE");
    return true;
}

static DWORD CALLBACK EncodedFileRead(void* buffer, DWORD length, void* user)
{
    auto* state = static_cast<ArchivePcm*>(user);
    if (!state || !buffer)
        return 0;
    if (state->isStreaming)
    {
        DWORD read = 0;
#if defined(AST_NATIVE_TEST_HOOKS)
        wchar_t text[32]{};
        const DWORD n = GetEnvironmentVariableW(L"AST_NATIVE_TEST_STREAM_FAIL_AFTER", text, std::size(text));
        const uint64_t cutoff = n > 0 && n < std::size(text) ? _wcstoui64(text, nullptr, 10) : 0;
        const bool injected = gInjectPlaybackReadFailure && cutoff >= 4096 &&
            state->stream.position >= cutoff;
        if (injected)
            Trace("RECOVERY_TEST_FAULT", "codec=OGG");
        else
#endif
            read = static_cast<DWORD>(ArchiveReadEntryStream(&state->stream, buffer, length));
        if (read || state->stream.position >= state->encodedSize)
            return read;
        const uint64_t failedOffset = state->stream.position;
        if (!RecoverOggEncodedSource(state, failedOffset))
            return 0;
        // Re-enter exactly once now that the source is RAM or cache.
        return EncodedFileRead(buffer, length, user);
    }
    if (state->cachedEncodedFile)
        return static_cast<DWORD>(fread(buffer, 1, length, state->cachedEncodedFile));
    if (state->memoryCursor >= state->encoded.size())
        return 0;
    const size_t count = std::min(static_cast<size_t>(length), state->encoded.size() - state->memoryCursor);
    memcpy(buffer, state->encoded.data() + state->memoryCursor, count);
    state->memoryCursor += count;
    return static_cast<DWORD>(count);
}

static BOOL CALLBACK EncodedFileSeek(QWORD position, void* user)
{
    auto* state = static_cast<ArchivePcm*>(user);
    if (!state || position > state->encodedSize)
        return FALSE;
    if (state->isStreaming)
    {
        if (ArchiveSeekEntryStream(&state->stream, position))
            return TRUE;
        // A failed compressed seek must not reposition PCM inaccurately.
        return RecoverOggEncodedSource(state, position) ? TRUE : FALSE;
    }
    if (state->cachedEncodedFile)
        return _fseeki64(state->cachedEncodedFile, static_cast<int64_t>(position), SEEK_SET) == 0 ? TRUE : FALSE;
    if (position > state->encoded.size())
        return FALSE;
    state->memoryCursor = static_cast<size_t>(position);
    return TRUE;
}

static const BASS_FILEPROCS kArchiveEncodedFileProcs = {
    EncodedFileClose, EncodedFileLength, EncodedFileRead, EncodedFileSeek
};

// The native BASS file decoder may inspect registered add-ons while opening a
// virtual compressed source. Do not recursively open the same .ast entry.
static thread_local bool gOpeningNestedFile = false;
static HSTREAM OpenArchiveHostDecoder(ArchivePcm* state)
{
    gOpeningNestedFile = true;
    const HSTREAM stream = BASS_StreamCreateFileUser(STREAMFILE_NOBUFFER, BASS_STREAM_DECODE, &kArchiveEncodedFileProcs, state);
    gOpeningNestedFile = false;
    return stream;
}

static bool IsMediaFoundationFormat(AudioFormat format)
{
    return format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_M4A || format == AUDIO_FORMAT_WMA;
}

// Media Foundation estimates raw ADTS AAC duration from average compressed
// bitrate and can overreport it. Scan frame headers with the same archive
// stream reader to count exact AAC-LC PCM frames without extracting to disk.
// Only accept ADTS with a stable sample rate matching the decoder output;
// other AAC layouts retain the established compatibility path.
static bool ReadAdtsExact(ArchiveStreamContext* stream, void* dest, size_t size)
{
    auto* bytes = static_cast<unsigned char*>(dest);
    size_t done = 0;
    while (done < size)
    {
        const size_t read = ArchiveReadEntryStream(stream, bytes + done, size - done);
        if (!read)
            return false;
        done += read;
    }
    return true;
}

static bool CountAdtsPcmFrames(const char* archivePath, mz_uint index, uint64_t entrySize,
                               DWORD decodedSampleRate, uint64_t* outFrames)
{
    if (outFrames)
        *outFrames = 0;
    if (!archivePath || !outFrames || !decodedSampleRate || entrySize < 7)
        return false;

    ArchiveStreamContext stream{};
    std::string error;
    if (!ArchiveOpenValidatedEntryStream(&stream, archivePath, index, 4096, &error))
        return false;

    static constexpr unsigned rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350 };
    bool valid = true;
    uint64_t consumed = 0;
    uint64_t frames = 0;
    unsigned referenceIndex = 255;
    unsigned char discard[4096]{};
    while (consumed < entrySize)
    {
        unsigned char header[7]{};
        if (entrySize - consumed < sizeof(header) || !ReadAdtsExact(&stream, header, sizeof(header)))
        {
            valid = false;
            break;
        }
        const unsigned rateIndex = (header[2] >> 2) & 0x0F;
        const unsigned frameLength = ((header[3] & 3) << 11) | (header[4] << 3) | (header[5] >> 5);
        const unsigned minLength = (header[1] & 1) ? 7 : 9;
        if (header[0] != 0xFF || (header[1] & 0xF6) != 0xF0 ||
            rateIndex >= sizeof(rates) / sizeof(rates[0]) || rates[rateIndex] != decodedSampleRate ||
            (referenceIndex != 255 && rateIndex != referenceIndex) ||
            frameLength < minLength || frameLength > entrySize - consumed)
        {
            valid = false;
            break;
        }
        referenceIndex = rateIndex;
        if (frames > std::numeric_limits<uint64_t>::max() - 1024u * (1 + (header[6] & 3)))
        {
            valid = false;
            break;
        }
        frames += 1024u * (1 + (header[6] & 3));
        consumed += frameLength;
        size_t rest = frameLength - sizeof(header);
        while (rest)
        {
            const size_t chunk = std::min(rest, sizeof(discard));
            if (!ReadAdtsExact(&stream, discard, chunk))
            {
                valid = false;
                break;
            }
            rest -= chunk;
        }
        if (!valid)
            break;
    }
    ArchiveCloseEntryStream(&stream);
    if (!valid || !frames || consumed != entrySize)
        return false;
    *outFrames = frames;
    return true;
}

// The ADTS duration must count every compressed frame, including cache
// files from solid archives whose original entry cannot be streamed.
static bool CountAdtsMemoryFrames(const std::vector<unsigned char>& bytes,
    DWORD decodedRate, uint64_t* frames)
{
    if (!frames || !decodedRate)
        return false;
    *frames = 0;
    static constexpr unsigned rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350 };
    size_t offset = 0;
    unsigned rateIndex = 255;
    uint64_t samples = 0;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 7)
            return false;
        const auto* h = bytes.data() + offset;
        const unsigned rate = (h[2] >> 2) & 15u;
        const unsigned length = ((h[3] & 3u) << 11) | (h[4] << 3) | (h[5] >> 5);
        const unsigned minimum = (h[1] & 1u) ? 7 : 9;
        const uint64_t next = 1024ull * (1u + (h[6] & 3u));
        if (h[0] != 0xff || (h[1] & 0xf6) != 0xf0 ||
            rate >= std::size(rates) || rates[rate] != decodedRate ||
            (rateIndex != 255 && rate != rateIndex) ||
            length < minimum || length > bytes.size() - offset ||
            samples > std::numeric_limits<uint64_t>::max() - next)
            return false;
        rateIndex = rate;
        samples += next;
        offset += length;
    }
    if (!samples)
        return false;
    *frames = samples;
    return true;
}

static bool CountAdtsCachedFrames(const wchar_t* path, DWORD decodedRate, uint64_t* frames)
{
    if (frames)
        *frames = 0;
    if (!path || !frames || !decodedRate)
        return false;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"rb") || !file)
        return false;
    static constexpr unsigned rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350 };
    bool valid = _fseeki64(file, 0, SEEK_END) == 0;
    const int64_t fileBytes = valid ? _ftelli64(file) : -1;
    valid = valid && fileBytes >= 7 && _fseeki64(file, 0, SEEK_SET) == 0;
    uint64_t consumed = 0;
    uint64_t samples = 0;
    unsigned rateIndex = 255;
    while (valid && consumed < static_cast<uint64_t>(fileBytes))
    {
        unsigned char header[7]{};
        const size_t read = fread(header, 1, sizeof(header), file);
        if (read != sizeof(header))
        {
            valid = false;
            break;
        }
        const unsigned index = (header[2] >> 2) & 15u;
        const unsigned length = ((header[3] & 3u) << 11) | (header[4] << 3) | (header[5] >> 5);
        const unsigned minimum = (header[1] & 1u) ? 7 : 9;
        if (header[0] != 0xFF || (header[1] & 0xF6) != 0xF0 ||
            index >= std::size(rates) || rates[index] != decodedRate ||
            (rateIndex != 255 && index != rateIndex) || length < minimum ||
            length > static_cast<uint64_t>(fileBytes) - consumed ||
            samples > std::numeric_limits<uint64_t>::max() - 1024u * (1u + (header[6] & 3u)) ||
            _fseeki64(file, length - sizeof(header), SEEK_CUR) != 0)
        {
            valid = false;
            break;
        }
        rateIndex = index;
        consumed += length;
        samples += 1024u * (1u + (header[6] & 3u));
    }
    const int fileError = ferror(file);
    const int closed = fclose(file);
    if (!valid || fileError || closed || !samples ||
        consumed != static_cast<uint64_t>(fileBytes))
        return false;
    *frames = samples;
    return true;
}

static bool IsBassFileDecodedFormat(AudioFormat format)
{
    return format == AUDIO_FORMAT_OGG;
}

static const char* FormatNote(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_MP3: return "codec=MP3";
    case AUDIO_FORMAT_WAV: return "codec=WAV";
    case AUDIO_FORMAT_FLAC: return "codec=FLAC";
    case AUDIO_FORMAT_OGG: return "codec=OGG";
    case AUDIO_FORMAT_AAC: return "codec=AAC";
    case AUDIO_FORMAT_M4A: return "codec=M4A";
    case AUDIO_FORMAT_WMA: return "codec=WMA";
    default: return "codec=UNKNOWN";
    }
}

static const char* OpenRouteNote(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_MP3: return "codec=MP3 route=STREAM_or_BUFFER";
    case AUDIO_FORMAT_WAV: return "codec=WAV route=STREAM_or_BUFFER";
    case AUDIO_FORMAT_FLAC: return "codec=FLAC route=STREAM_or_BUFFER";
    case AUDIO_FORMAT_OGG: return "codec=OGG route=BASS_FILEUSER";
    case AUDIO_FORMAT_AAC: return "codec=AAC route=MF_ARCHIVE_STREAM";
    case AUDIO_FORMAT_M4A: return "codec=M4A route=MF_ARCHIVE_STREAM";
    case AUDIO_FORMAT_WMA: return "codec=WMA route=MF_ARCHIVE_STREAM";
    default: return "codec=UNKNOWN";
    }
}

static void CloseDecoder(ArchivePcm* state)
{
    if (!state || !state->opened)
        return;
    if (state->format == AUDIO_FORMAT_WAV)
        drwav_uninit(&state->wav);
    else if (state->format == AUDIO_FORMAT_FLAC)
        drflac_close(state->flac);
    else if (IsBassFileDecodedFormat(state->format))
        BASS_StreamFree(state->hostStream);
    else if (IsMediaFoundationFormat(state->format))
        MfCloseDecoder(state->mf);
    else
        drmp3_uninit(&state->decoder);
    state->opened = false;
    state->flac = nullptr;
    state->mf = nullptr;
    state->mfArchiveStream = false;
}

// Native midstream recovery for the three decoders with exact PCM-frame
// seeking. Recover only once, after an unexpected short read; do not turn
// ordinary EOF into a cache extraction. The codec state is single-threaded
// under the BASS add-on lock when this callback executes.
static bool RecoverNativePcmAtFrame(ArchivePcm* state, uint64_t targetFrame)
{
    if (!state || !state->opened || !state->isStreaming || state->recoveryAttempted ||
        (state->format != AUDIO_FORMAT_WAV && state->format != AUDIO_FORMAT_FLAC &&
         state->format != AUDIO_FORMAT_MP3) || state->archivePathUtf8.empty() ||
        !state->channels || !state->sampleRate || targetFrame >=
        state->pcmBytes / (state->channels * sizeof(int16_t)))
        return false;

    char remainingNote[128]{};
    sprintf_s(remainingNote, "remaining_frames=%llu",
        static_cast<unsigned long long>(state->pcmBytes / (state->channels * sizeof(int16_t)) - targetFrame));
    Trace("RECOVERY_REMAINING", remainingNote);
    // dr_mp3 can report a handful of padding frames beyond real EOF.
    // Never extract/cache for that normal decoder-tail discrepancy.
    const uint64_t missingFrames = state->pcmBytes / (state->channels * sizeof(int16_t)) - targetFrame;
    if (state->format == AUDIO_FORMAT_MP3 && missingFrames <= 2304)
        return false;
    state->recoveryAttempted = true;
    Trace("RECOVERY_START", FormatNote(state->format));
    const DWORD oldRate = state->sampleRate;
    const DWORD oldChannels = state->channels;
    const uint64_t expectedFrames = state->pcmBytes / (oldChannels * sizeof(int16_t));
    CloseDecoder(state);
    if (state->isStreaming)
    {
        ArchiveCloseEntryStream(&state->stream);
        state->isStreaming = false;
    }
    const ArchiveAdapterPreferences preferences = ArchiveAdapterPreferencesLoad();
    std::string error;
    char name[1024]{};
    uint64_t size = 0;
    if (!ArchiveGetEntryInfo(state->archivePathUtf8.c_str(), state->entryIndex,
                             name, sizeof(name), &size) || !size)
    {
        Trace("RECOVERY_FAIL", "reason=entry_info");
        return false;
    }

    auto openMemory = [&]() -> bool
    {
        if (size > static_cast<uint64_t>(preferences.memoryMb) * 1024u * 1024u ||
            !ArchiveExtractEntryToMemory(state->archivePathUtf8.c_str(), state->entryIndex, state->encoded, error) ||
            state->encoded.size() != size)
            return false;
        if (state->format == AUDIO_FORMAT_WAV)
            state->opened = drwav_init_memory(&state->wav, state->encoded.data(), state->encoded.size(), nullptr) != 0;
        else if (state->format == AUDIO_FORMAT_FLAC)
        {
            state->flac = drflac_open_memory(state->encoded.data(), state->encoded.size(), nullptr);
            state->opened = state->flac != nullptr;
        }
        else
            state->opened = drmp3_init_memory(&state->decoder, state->encoded.data(), state->encoded.size(), nullptr) != 0;
        return state->opened;
    };

    auto verifyAndSeek = [&]() -> bool
    {
        if (!state->opened)
            return false;
        const uint64_t frames = state->format == AUDIO_FORMAT_WAV ? state->wav.totalPCMFrameCount :
            state->format == AUDIO_FORMAT_FLAC ? state->flac->totalPCMFrameCount :
            drmp3_get_pcm_frame_count(&state->decoder);
        const DWORD rate = state->format == AUDIO_FORMAT_WAV ? state->wav.sampleRate :
            state->format == AUDIO_FORMAT_FLAC ? state->flac->sampleRate : state->decoder.sampleRate;
        const DWORD channels = state->format == AUDIO_FORMAT_WAV ? state->wav.channels :
            state->format == AUDIO_FORMAT_FLAC ? state->flac->channels : state->decoder.channels;
        const bool formatMatches = rate == oldRate && channels == oldChannels &&
            frames == expectedFrames;
        const bool seeked = formatMatches &&
            (state->format == AUDIO_FORMAT_WAV ? drwav_seek_to_pcm_frame(&state->wav, targetFrame) != 0 :
             state->format == AUDIO_FORMAT_FLAC ? drflac_seek_to_pcm_frame(state->flac, targetFrame) != 0 :
             drmp3_seek_to_pcm_frame(&state->decoder, targetFrame) != 0);
        if (!seeked)
        {
            CloseDecoder(state);
            return false;
        }
        return true;
    };

    if (openMemory() && verifyAndSeek())
    {
        state->diskCached = false;
        Trace("RECOVERY_OK", "route=BUFFER");
        return true;
    }
    state->encoded.clear();
    std::wstring path;
    ArchiveEntry entry;
    std::wstring cachePath;
    wchar_t archiveWide[MAX_PATH * 4]{};
    if (!CharPathToWidePath(state->archivePathUtf8.c_str(), archiveWide, std::size(archiveWide)) ||
        !ArchiveUnifiedCache::LookupEntry(archiveWide, state->entryIndex, entry, error) ||
        !PrepareDiskLastAudio(archiveWide, entry, preferences, cachePath, error))
    {
        Trace("RECOVERY_FAIL", "reason=memory_cache_unavailable");
        return false;
    }
    if (state->format == AUDIO_FORMAT_WAV)
        state->opened = drwav_init_file_w(&state->wav, cachePath.c_str(), nullptr) != 0;
    else if (state->format == AUDIO_FORMAT_FLAC)
    {
        state->flac = drflac_open_file_w(cachePath.c_str(), nullptr);
        state->opened = state->flac != nullptr;
    }
    else
        state->opened = drmp3_init_file_w(&state->decoder, cachePath.c_str(), nullptr) != 0;

    if (!verifyAndSeek())
    {
        Trace("RECOVERY_FAIL", "reason=cached_decoder_or_position");
        return false;
    }
    state->diskCached = true;
    Trace("RECOVERY_OK", "route=CACHE");
    return true;
}

const ADDON_FUNCTIONS ArchivePcm::callbacks = {
    ADDON_LOCK,
    Free,
    Length,
    GetTags,
    nullptr,
    Info,
    CanSeek,
    Seek,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

void WINAPI ArchivePcm::Free(void* ptr)
{
    auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state)
        return;
    CloseDecoder(state);
    if (state->isStreaming)
        ArchiveCloseEntryStream(&state->stream);
    delete state;
    Trace("CLOSE", "");
}

QWORD WINAPI ArchivePcm::Length(void* ptr, DWORD mode)
{
    const auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE)
        errorn(BASS_ERROR_NOTAVAIL);
    noerrorn(state->pcmBytes);
}

// Expose the same native title presets used by other adapters without writing
// tags into the archive or temporary .ast descriptor. BASS tag arrays remain
// valid until the stream is freed; changing a setting affects newly opened
// native tracks, while in-place playlist repaint is still host-dependent.
const char* WINAPI ArchivePcm::GetTags(void* ptr, DWORD type)
{
    auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state)
        return nullptr;
    if (type == BASS_TAG_ID3V2 && !state->displayId3.empty())
        return reinterpret_cast<const char*>(state->displayId3.data());
    if (state->displayTitleTags.empty())
        return nullptr;
    switch (type)
    {
    case BASS_TAG_OGG:
    case BASS_TAG_APE:
    case BASS_TAG_MP4:
    case BASS_TAG_WMA:
    case BASS_TAG_MF:
        Trace("TITLE_TAG_READ", "route=NATIVE");
        return state->displayTitleTags.data();
    default:
        return nullptr;
    }
}

void WINAPI ArchivePcm::Info(void* ptr, BASS_CHANNELINFO* info)
{
    if (info)
        info->ctype = kArchiveCtype;
}

BOOL WINAPI ArchivePcm::CanSeek(void* ptr, QWORD bytePosition, DWORD mode)
{
    const auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE)
        error(BASS_ERROR_NOTAVAIL);
    if (!state->channels || bytePosition > state->pcmBytes ||
        bytePosition % (state->channels * sizeof(int16_t)) != 0)
        error(BASS_ERROR_POSITION);
    return TRUE;
}

QWORD WINAPI ArchivePcm::Seek(void* ptr, QWORD bytePosition, DWORD mode)
{
    auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE || !state->channels)
        errorn(BASS_ERROR_NOTAVAIL);
    const QWORD frame = bytePosition / (state->channels * sizeof(int16_t));
    const uint64_t targetMs = IsMediaFoundationFormat(state->format) && state->sampleRate
        ? frame * 1000 / state->sampleRate : 0;
    if (IsMediaFoundationFormat(state->format) && targetMs > INT_MAX)
        errorn(BASS_ERROR_POSITION);
    const bool ok = state->format == AUDIO_FORMAT_WAV
        ? drwav_seek_to_pcm_frame(&state->wav, frame) != 0
        : state->format == AUDIO_FORMAT_FLAC
            ? drflac_seek_to_pcm_frame(state->flac, frame) != 0
            : IsBassFileDecodedFormat(state->format)
                ? BASS_ChannelSetPosition(state->hostStream, bytePosition, BASS_POS_BYTE) != 0
                : IsMediaFoundationFormat(state->format)
                    ? MfSeekMs(state->mf, static_cast<int>(targetMs))
                    : drmp3_seek_to_pcm_frame(&state->decoder, frame) != 0;
    if (!ok)
    {
        // Seek failure can be an encoded stream failure; use exact frame seek
        // on bounded memory/cache before reporting a positioning error.
        if (!RecoverNativePcmAtFrame(state, frame))
        {
            Trace("SEEK_FAIL", "");
            errorn(BASS_ERROR_POSITION);
        }
    }
    state->pcmFrameCursor = frame;
    Trace("SEEK_OK", "");
    noerrorn(bytePosition);
}

DWORD CALLBACK ArchivePcm::ReadPcm(HSTREAM, void* buffer, DWORD length, void* ptr)
{
    auto* state = static_cast<ArchivePcm*>(ptr);
    if (!state || !buffer || !state->channels)
        return BASS_STREAMPROC_END;

    const DWORD bytesPerFrame = state->channels * sizeof(int16_t);
    const DWORD frames = length / bytesPerFrame;
    if (!frames)
        return 0;

#if defined(AST_NATIVE_TEST_HOOKS)
    gInjectPlaybackReadFailure = state->firstRead && state->isStreaming;
#endif
    uint64_t read = 0;
    if (IsBassFileDecodedFormat(state->format))
    {
        const DWORD decoded = BASS_ChannelGetData(state->hostStream, buffer, frames * bytesPerFrame);
        read = decoded == static_cast<DWORD>(-1) ? 0 : decoded / bytesPerFrame;
    }
    else if (IsMediaFoundationFormat(state->format))
        read = MfReadPcm(state->mf, buffer, frames * bytesPerFrame) / bytesPerFrame;
    else if (state->format == AUDIO_FORMAT_WAV)
        read = drwav_read_pcm_frames_s16(&state->wav, frames, static_cast<int16_t*>(buffer));
    else if (state->format == AUDIO_FORMAT_FLAC)
        read = drflac_read_pcm_frames_s16(state->flac, frames, static_cast<int16_t*>(buffer));
    else
        read = drmp3_read_pcm_frames_s16(&state->decoder, frames, static_cast<int16_t*>(buffer));
#if defined(AST_NATIVE_TEST_HOOKS)
    gInjectPlaybackReadFailure = false;
#endif
    // A short read before known PCM EOF is an interrupted archive stream,
    // not normal track completion. Preserve the already decoded prefix, then
    // recover exactly at its next frame and fill the remaining callback.
    const uint64_t totalFrames = state->pcmBytes / bytesPerFrame;
    if (read < frames && state->isStreaming &&
        state->pcmFrameCursor + read < totalFrames &&
        RecoverNativePcmAtFrame(state, state->pcmFrameCursor + read))
    {
        const DWORD remaining = frames - static_cast<DWORD>(read);
        uint64_t continuation = 0;
        auto* destination = static_cast<unsigned char*>(buffer) + read * bytesPerFrame;
        if (state->format == AUDIO_FORMAT_WAV)
            continuation = drwav_read_pcm_frames_s16(&state->wav, remaining, reinterpret_cast<int16_t*>(destination));
        else if (state->format == AUDIO_FORMAT_FLAC)
            continuation = drflac_read_pcm_frames_s16(state->flac, remaining, reinterpret_cast<int16_t*>(destination));
        else
            continuation = drmp3_read_pcm_frames_s16(&state->decoder, remaining, reinterpret_cast<int16_t*>(destination));
        read += continuation;
        Trace(continuation ? "RECOVERY_PCM_RESUMED" : "RECOVERY_PCM_EMPTY", FormatNote(state->format));
    }
    state->pcmFrameCursor += read;
    if (!state->firstRead && read > 0)
    {
        MusicBeeCommitVerifiedPassword(state->archivePathUtf8.c_str());
        state->firstRead = true;
        Trace("PCM_FIRST", state->diskCached ? "route=CACHE" :
            (state->isStreaming || state->mfArchiveStream) ? "route=STREAM" : "route=BUFFER");
    }

    const DWORD bytes = static_cast<DWORD>(read) * bytesPerFrame;
    if (read < frames)
    {
        Trace("PCM_END", "");
        return bytes | BASS_STREAMPROC_END;
    }
    return bytes;
}

static bool ConvertDescriptorName(BASSFILE file, std::string& path)
{
    BOOL unicode = FALSE;
    const char* raw = bassfunc->file.GetFileName(file, &unicode);
    if (!raw)
        return false;
    if (!unicode)
    {
        path.assign(raw);
        return !path.empty();
    }

    const wchar_t* wide = reinterpret_cast<const wchar_t*>(raw);
    int bytes = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1 || bytes > 16384)
        return false;
    std::string converted(static_cast<size_t>(bytes), '\0');
    BOOL replaced = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, converted.data(), bytes, nullptr, &replaced) != bytes || replaced)
        return false;
    converted.resize(static_cast<size_t>(bytes) - 1);
    path.swap(converted);
    return true;
}


// Native ID3 metadata aliases are separate, tiny files. The paired .ast
// remains an unmodified text manifest. Accept ID3 aliases only if their
// complete trailing manifest byte-for-byte matches a valid paired .ast file.
// Existing same-file hard-link aliases retain their original verification.
static bool MusicBeeValidateId3Alias(const std::string& aliasPath, const std::string& manifestPath,
    std::vector<unsigned char>& id3)
{
    id3.clear();
    std::ifstream input(aliasPath, std::ios::binary | std::ios::ate);
    std::ifstream manifest(manifestPath, std::ios::binary | std::ios::ate);
    if (!input || !manifest)
        return false;

    const std::streamoff aliasLength = input.tellg();
    const std::streamoff manifestLength = manifest.tellg();
    if (aliasLength <= 50 || aliasLength > 32768 || manifestLength < 36 || manifestLength > 16384)
        return false;

    std::vector<unsigned char> alias(static_cast<size_t>(aliasLength));
    std::vector<unsigned char> original(static_cast<size_t>(manifestLength));
    input.seekg(0);
    manifest.seekg(0);
    if (!input.read(reinterpret_cast<char*>(alias.data()), static_cast<std::streamsize>(alias.size())) ||
        !manifest.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size())))
        return false;
    if (std::memcmp(alias.data(), "ID3\x03\0\0", 6) != 0)
        return false;
    for (size_t i = 6; i <= 9; ++i)
    {
        if (alias[i] & 0x80)
            return false;
    }

    const size_t tagLength = 10 + (size_t(alias[6]) << 21) + (size_t(alias[7]) << 14) +
        (size_t(alias[8]) << 7) + alias[9];
    if (tagLength <= 20 || tagLength + original.size() != alias.size() ||
        std::memcmp(alias.data() + tagLength, original.data(), original.size()) != 0)
        return false;

    ArchiveTrackTags tags{};
    if (!ArchiveTrackTagParser::Parse("mp3", alias.data(), tagLength, tags) || tags.title.empty())
        return false;
    id3.assign(alias.begin(), alias.begin() + tagLength);
    return true;
}

static HSTREAM WINAPI CreateArchiveStream(BASSFILE descriptor, DWORD flags)
{
    if (gOpeningNestedFile)
        return 0;
    Trace("CREATE_BEGIN", "codec=MP3_WAV_FLAC_OGG_AAC_M4A_WMA");
    if (!descriptor)
        return 0;

    std::string descriptorPath;
    std::string virtualTrack;
    Trace("DESCRIPTOR_NAME_BEGIN", "");
    if (!ConvertDescriptorName(descriptor, descriptorPath))
    {
        Trace("DESCRIPTOR_NAME_FAIL", "bass_file_name_unavailable");
        return 0;
    }
    Trace("DESCRIPTOR_NAME_OK", "");
    bool manifestValid = ArchiveNativeTrackRead(descriptorPath.c_str(), virtualTrack);
    std::vector<unsigned char> verifiedId3;
    // MusicBee's playlist admits ordinary audio filenames, not .ast files.
    // A .mp3 alias is a zero-copy hard link to the real .ast descriptor:
    // validate the paired .ast file with the existing strict parser before
    // allowing the BASS archive decoder to claim this file.
    if (!manifestValid && descriptorPath.size() >= 4 &&
        _stricmp(descriptorPath.c_str() + descriptorPath.size() - 4, ".mp3") == 0)
    {
        std::string manifestPath = descriptorPath.substr(0, descriptorPath.size() - 4) + ".ast";
        manifestValid = ArchiveNativeTrackRead(manifestPath.c_str(), virtualTrack);
        if (manifestValid)
        {
            HANDLE alias = CreateFileA(descriptorPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            HANDLE original = CreateFileA(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            BY_HANDLE_FILE_INFORMATION aliasInfo{}, originalInfo{};
            manifestValid = alias != INVALID_HANDLE_VALUE && original != INVALID_HANDLE_VALUE &&
                GetFileInformationByHandle(alias, &aliasInfo) && GetFileInformationByHandle(original, &originalInfo) &&
                aliasInfo.dwVolumeSerialNumber == originalInfo.dwVolumeSerialNumber &&
                aliasInfo.nFileIndexHigh == originalInfo.nFileIndexHigh &&
                aliasInfo.nFileIndexLow == originalInfo.nFileIndexLow;
            if (alias != INVALID_HANDLE_VALUE)
                CloseHandle(alias);
            if (original != INVALID_HANDLE_VALUE)
                CloseHandle(original);
            if (manifestValid)
                Trace("MANIFEST_MP3_ALIAS_VALID", "link=verified");
            else
            {
                manifestValid = MusicBeeValidateId3Alias(descriptorPath, manifestPath, verifiedId3);
                if (manifestValid)
                    Trace("MANIFEST_MP3_ID3_ALIAS_VALID", "manifest=byte_equal");
            }
        }
    }
    if (!manifestValid)
    {
        Trace("MANIFEST_READ_FAIL", "invalid_or_unreadable_ast_or_alias");
        return 0;
    }
    Trace("MANIFEST_READ_OK", "");

    char archive[MAX_PATH * 4]{};
    char member[1024]{};
    mz_uint index = 0;
    if (!ParseVirtualArchivePath(virtualTrack.c_str(), archive, sizeof(archive), &index, member, sizeof(member)))
        return 0;
    const auto title = ArchiveAdapterPreferredTitle(ArchiveAdapterUtf8ToWide(archive),
        ArchiveAdapterUtf8ToWide(member), static_cast<unsigned>(index) + 1,
        ArchiveAdapterPreferencesLoad());
    const std::string titleUtf8 = ArchiveAdapterWideToUtf8(title);
    if (titleUtf8.empty() || titleUtf8.size() > 4096 ||
        titleUtf8.find('\r') != std::string::npos || titleUtf8.find('\n') != std::string::npos ||
        titleUtf8.find('\0') != std::string::npos)
        return 0;
    const AudioFormat format = GetAudioFormatFromPath(member);
    if (format != AUDIO_FORMAT_MP3 && format != AUDIO_FORMAT_WAV && format != AUDIO_FORMAT_FLAC &&
        !IsBassFileDecodedFormat(format) && !IsMediaFoundationFormat(format))
        return 0;

    Trace("MANIFEST_OK", FormatNote(format));
    MusicBeeInitializePasswordBackend();
    auto state = std::make_unique<ArchivePcm>();
    state->displayId3 = std::move(verifiedId3);
    ArchiveTrackTags aliasTags{};
    const bool tagMode = ArchiveAdapterPreferencesLoad().useFileTags && !state->displayId3.empty() &&
        ArchiveTrackTagParser::Parse("mp3", state->displayId3.data(), state->displayId3.size(), aliasTags);
    auto appendTag = [&state](const std::string& key, const std::string& value)
    {
        if (value.empty())
            return;
        const std::string item = key + "=" + value;
        state->displayTitleTags.insert(state->displayTitleTags.end(), item.begin(), item.end());
        state->displayTitleTags.push_back('\0');
    };
    appendTag("TITLE", tagMode && !aliasTags.title.empty() ? aliasTags.title : titleUtf8);
    if (tagMode)
    {
        appendTag("ARTIST", aliasTags.artist);
        appendTag("ALBUM", aliasTags.album);
        appendTag("ALBUMARTIST", aliasTags.albumArtist);
        if (aliasTags.trackNumber)
            appendTag("TRACKNUMBER", std::to_string(aliasTags.trackNumber));
        appendTag("GENRE", aliasTags.genre);
        appendTag("DATE", aliasTags.year);
    }
    state->displayTitleTags.push_back('\0');
    state->format = format;
    state->archivePathUtf8 = archive;
    state->entryIndex = index;
    std::string error;
    if (IsBassFileDecodedFormat(format) || IsMediaFoundationFormat(format))
    {
        char entryName[1024]{};
        if (!ArchiveGetEntryInfo(archive, index, entryName, sizeof(entryName), &state->encodedSize) || !state->encodedSize)
            return 0;
    }
    // An encrypted RAR5 entry may report an apparent stream capability even
    // though the backend cannot supply a seekable, verified byte stream.
    // Use the same entry metadata that native enumeration validated.
    bool encryptedRar = false;
    if (IsRarArchivePath(archive))
    {
        wchar_t archiveWide[MAX_PATH * 4]{};
        std::vector<ArchiveEntry> entries;
        std::string metadataError;
        if (!CharPathToWidePath(archive, archiveWide, std::size(archiveWide)) ||
            !ArchiveListEntries(archiveWide, entries, metadataError))
        {
            Trace("OPEN_FAIL", "reason=rar_metadata_unavailable");
            return 0;
        }
        const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& entry)
        {
            return entry.index == index && !entry.isFolder;
        });
        if (found == entries.end())
        {
            Trace("OPEN_FAIL", "reason=rar_entry_missing");
            return 0;
        }
        encryptedRar = found->isEncrypted;
    }
    if (IsMediaFoundationFormat(format))
    {
        // Solid dependencies and encrypted RAR entries are not independent
        // seekable streams, regardless of an optimistic backend capability.
        const bool streamSupported = !encryptedRar &&
            ArchiveGetEntryStreamCapability(archive, index) ==
            ARCHIVE_STREAM_CAPABILITY_SUPPORTED;
        state->opened = streamSupported &&
            MfOpenArchiveDecoder(archive, index, state->encodedSize, format, &state->mf);
        state->mfArchiveStream = state->opened;
        Trace("STREAM_VALIDATED", state->opened ? "route=MF_ARCHIVE_STREAM" : "route=FALLBACK");
    }
    else
    {
        // Do not trust an archive stream-open result for solid RAR/7z.
        // A solid RAR may return bytes before its decoder dependency chain has
        // been restored. Follow the shared capability decision instead.
        const bool streamSupported = !encryptedRar &&
            ArchiveGetEntryStreamCapability(archive, index) ==
            ARCHIVE_STREAM_CAPABILITY_SUPPORTED;
        state->isStreaming = streamSupported &&
            ArchiveOpenValidatedEntryStream(&state->stream, archive, index, 4096, &error);
        Trace("STREAM_VALIDATED", state->isStreaming ? "route=STREAM" : "route=FALLBACK");
        if (state->isStreaming)
        {
            if (state->format == AUDIO_FORMAT_WAV)
                state->opened = drwav_init(&state->wav, ReadEncoded, WavSeekEncoded, WavTellEncoded, &state->stream, nullptr) != 0;
            else if (state->format == AUDIO_FORMAT_FLAC)
            {
                state->flac = drflac_open(ReadEncoded, FlacSeekEncoded, FlacTellEncoded, &state->stream, nullptr);
                state->opened = state->flac != nullptr;
            }
            else if (IsBassFileDecodedFormat(state->format))
            {
                state->hostStream = OpenArchiveHostDecoder(state.get());
                state->opened = state->hostStream != 0;
            }
            else
                state->opened = drmp3_init(&state->decoder, ReadEncoded, SeekEncoded, TellEncoded, nullptr, &state->stream, nullptr) != 0;
            if (!state->opened && state->isStreaming)
            {
                ArchiveCloseEntryStream(&state->stream);
                state->isStreaming = false;
            }
        }
    }

    if (!state->opened)
    {
        uint64_t entryLength = 0;
        char entryName[1024]{};
        const ArchiveAdapterPreferences settings = ArchiveAdapterPreferencesLoad();
        if (!ArchiveGetEntryInfo(archive, index, entryName, sizeof(entryName), &entryLength) || !entryLength)
        {
            Trace("OPEN_FAIL", "reason=entry_info_missing");
            return 0;
        }
        if (entryLength <= static_cast<uint64_t>(settings.memoryMb) * 1024u * 1024u)
        {
            if (ArchiveExtractEntryToMemory(archive, index, state->encoded, error))
            {
                if (state->format == AUDIO_FORMAT_WAV)
                    state->opened = drwav_init_memory(&state->wav, state->encoded.data(), state->encoded.size(), nullptr) != 0;
                else if (state->format == AUDIO_FORMAT_FLAC)
                {
                    state->flac = drflac_open_memory(state->encoded.data(), state->encoded.size(), nullptr);
                    state->opened = state->flac != nullptr;
                }
                else if (IsBassFileDecodedFormat(state->format))
                {
                    state->encodedSize = state->encoded.size();
                    state->memoryCursor = 0;
                    state->hostStream = OpenArchiveHostDecoder(state.get());
                    state->opened = state->hostStream != 0;
                }
                else if (IsMediaFoundationFormat(state->format))
                    state->opened = MfOpenMemoryDecoder(state->encoded.data(), state->encoded.size(), state->format, &state->mf);
                else
                    state->opened = drmp3_init_memory(&state->decoder, state->encoded.data(), state->encoded.size(), nullptr) != 0;
            }
            else
                Trace("MEMORY_FALLBACK_FAIL", "route=MEMORY");
        }
        if (!state->opened && (state->format == AUDIO_FORMAT_FLAC || state->format == AUDIO_FORMAT_WAV ||
                               state->format == AUDIO_FORMAT_MP3 || state->format == AUDIO_FORMAT_OGG ||
                               IsMediaFoundationFormat(state->format)))
        {
            wchar_t archiveWide[MAX_PATH * 4]{};
            ArchiveEntry cacheEntry;
            std::wstring cachePath;
            const bool ready = CharPathToWidePath(archive, archiveWide, std::size(archiveWide)) &&
                ArchiveUnifiedCache::LookupEntry(archiveWide, index, cacheEntry, error) &&
                PrepareDiskLastAudio(archiveWide, cacheEntry, settings, cachePath, error);
            if (ready)
            {
                state->encoded.clear();
                if (state->format == AUDIO_FORMAT_WAV)
                    state->opened = drwav_init_file_w(&state->wav, cachePath.c_str(), nullptr) != 0;
                else if (state->format == AUDIO_FORMAT_FLAC)
                {
                    state->flac = drflac_open_file_w(cachePath.c_str(), nullptr);
                    state->opened = state->flac != nullptr;
                }
                else if (IsMediaFoundationFormat(state->format))
                {
                    const std::string cacheUtf8 = ArchiveAdapterWideToUtf8(cachePath);
                    state->opened = !cacheUtf8.empty() &&
                        MfOpenCacheFileDecoder(cacheUtf8.c_str(), state->format, &state->mf);
                    if (state->opened)
                        state->cachedEncodedPath = cachePath;
                }
                else if (state->format == AUDIO_FORMAT_OGG)
                {
                    // The host BASS Vorbis decoder owns a seekable compressed
                    // file stream; archive/cache lifetime remains shared.
                    state->encodedSize = entryLength;
                    if (_wfopen_s(&state->cachedEncodedFile, cachePath.c_str(), L"rb") == 0 &&
                        state->cachedEncodedFile)
                    {
                        state->hostStream = OpenArchiveHostDecoder(state.get());
                        state->opened = state->hostStream != 0;
                        if (!state->opened && state->cachedEncodedFile)
                        {
                            fclose(state->cachedEncodedFile);
                            state->cachedEncodedFile = nullptr;
                        }
                    }
                }
                else
                    state->opened = drmp3_init_file_w(&state->decoder, cachePath.c_str(), nullptr) != 0;
                state->diskCached = state->opened;
                Trace(state->opened ? "CACHE_OPEN_OK" : "CACHE_OPEN_FAIL", FormatNote(state->format));
            }
        }
        if (!state->opened)
        {
            Trace("OPEN_FAIL", "reason=stream_memory_cache_unavailable");
            return 0;
        }
    }

    BASS_CHANNELINFO hostInfo{};
    if (IsBassFileDecodedFormat(state->format) && state->hostStream)
        BASS_ChannelGetInfo(state->hostStream, &hostInfo);
    const DWORD rate = state->format == AUDIO_FORMAT_WAV ? state->wav.sampleRate :
        state->format == AUDIO_FORMAT_FLAC && state->flac ? state->flac->sampleRate :
        IsMediaFoundationFormat(state->format) ? MfGetSampleRate(state->mf) :
        IsBassFileDecodedFormat(state->format) ? hostInfo.freq : state->decoder.sampleRate;
    const DWORD channels = state->format == AUDIO_FORMAT_WAV ? state->wav.channels :
        state->format == AUDIO_FORMAT_FLAC && state->flac ? state->flac->channels :
        IsMediaFoundationFormat(state->format) ? MfGetChannels(state->mf) :
        IsBassFileDecodedFormat(state->format) ? hostInfo.chans : state->decoder.channels;
    if (!state->opened || !rate || (channels != 1 && channels != 2) ||
        (IsMediaFoundationFormat(state->format) && MfGetBitsPerSample(state->mf) != 16))
    {
        Trace("OPEN_FAIL", "reason=decoder_invalid");
        CloseDecoder(state.get());
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        return 0;
    }

    state->sampleRate = rate;
    state->channels = channels;
    uint64_t exactAdtsFrames = 0;
    if (state->format == AUDIO_FORMAT_AAC &&
        !(state->diskCached ?
          CountAdtsCachedFrames(state->cachedEncodedPath.c_str(), state->sampleRate, &exactAdtsFrames) :
          (!state->encoded.empty() ?
            CountAdtsMemoryFrames(state->encoded, state->sampleRate, &exactAdtsFrames) :
            CountAdtsPcmFrames(archive, index, state->encodedSize, state->sampleRate, &exactAdtsFrames))))
    {
        Trace("OPEN_FAIL", "reason=aac_adts_frame_count_invalid");
        CloseDecoder(state.get());
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        return 0;
    }
    Trace("DECODER_INIT_OK", FormatNote(state->format));
    Trace("DURATION_COUNT_BEGIN", "");
    const uint64_t hostBytes = IsBassFileDecodedFormat(state->format) ? BASS_ChannelGetLength(state->hostStream, BASS_POS_BYTE) : 0;
    const int mfLengthMs = IsMediaFoundationFormat(state->format) ? MfGetLengthMs(state->mf) : 0;
    const uint64_t mfFrames = mfLengthMs > 0 ? static_cast<uint64_t>(mfLengthMs) * rate / 1000 : 0;
    const uint64_t frames = state->format == AUDIO_FORMAT_WAV ? state->wav.totalPCMFrameCount :
        state->format == AUDIO_FORMAT_FLAC ? state->flac->totalPCMFrameCount :
        IsBassFileDecodedFormat(state->format) ? hostBytes / (state->channels * sizeof(int16_t)) :
        state->format == AUDIO_FORMAT_AAC ? exactAdtsFrames :
        IsMediaFoundationFormat(state->format) ? mfFrames :
        drmp3_get_pcm_frame_count(&state->decoder);
    Trace("DURATION_COUNT_DONE", "");
    if (!frames || frames > std::numeric_limits<QWORD>::max() / (state->channels * sizeof(int16_t)))
    {
        CloseDecoder(state.get());
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        Trace("OPEN_FAIL", "reason=invalid_length");
        return 0;
    }
    state->pcmBytes = frames * state->channels * sizeof(int16_t);
    state->descriptor = descriptor;

    DWORD streamFlags = flags & (BASS_STREAM_DECODE | BASS_SAMPLE_SOFTWARE | BASS_SAMPLE_LOOP |
                                 BASS_STREAM_AUTOFREE | BASS_SAMPLE_3D | BASS_SAMPLE_FX | 0x3f000000);
    Trace("BASS_CREATE_STREAM_BEGIN", FormatNote(state->format));
    HSTREAM stream = bassfunc->CreateStream(state->sampleRate, state->channels, streamFlags,
                                           ArchivePcm::ReadPcm, state.get(), &ArchivePcm::callbacks);
    if (!stream)
    {
        CloseDecoder(state.get());
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        Trace("OPEN_FAIL", "reason=bass_stream_creation");
        return 0;
    }

    state->handle = stream;
    const AudioFormat openedFormat = state->format;
    const std::string openedNote = state->diskCached ?
        std::string(FormatNote(openedFormat)) + " route=CACHE" : OpenRouteNote(openedFormat);
    bassfunc->file.SetStream(descriptor, stream);
    state.release();
    if (BASS_GetVersion() >= 0x02041000)
        BASS_ChannelLock(stream, FALSE);
    Trace("OPEN_OK", openedNote.c_str());
    return stream;
}

// Solid 7z/RAR entries cannot be assumed streamable. Accept a bounded RAM
// route only after full extraction and actual PCM decode + seek verification.
// Keep other codecs/layouts on the proven compatibility route until certified.
// Reuse the same identity-checked, atomically published managed cache as
// VLC/Winamp and bridge clients. Never extract if streaming/RAM can work.
// v1.9 accepted FLAC, v1.10 added PCM16 WAV, and v1.11 adds MP3.
// Each previous scanner export retains its original accepted formats.
static bool PrepareDiskLastAudio(const wchar_t* archivePath, const ArchiveEntry& entry,
    const ArchiveAdapterPreferences& settings, std::wstring& cachePath, std::string& error)
{
    if (!archivePath || !entry.isSupportedAudio || entry.isFolder ||
        !entry.size || (GetAudioFormatFromPath(entry.path.c_str()) != AUDIO_FORMAT_FLAC &&
         GetAudioFormatFromPath(entry.path.c_str()) != AUDIO_FORMAT_WAV &&
         GetAudioFormatFromPath(entry.path.c_str()) != AUDIO_FORMAT_MP3 &&
         GetAudioFormatFromPath(entry.path.c_str()) != AUDIO_FORMAT_OGG &&
         !IsMediaFoundationFormat(GetAudioFormatFromPath(entry.path.c_str()))) ||
        settings.cacheLimitMb == 0 || entry.size > static_cast<uint64_t>(settings.cacheLimitMb) * 1024u * 1024u)
        return false;
    return ArchiveUnifiedCache::Prepare(archivePath, entry, settings, cachePath, error);
}

static bool PreflightDiskLastAudio(const wchar_t* archivePath, const ArchiveEntry& entry,
    const ArchiveAdapterPreferences& settings)
{
    if (entry.size <= static_cast<uint64_t>(settings.memoryMb) * 1024u * 1024u)
        return false;
    std::wstring cachePath;
    std::string error;
    if (!PrepareDiskLastAudio(archivePath, entry, settings, cachePath, error))
        return false;
    int16_t pcm[2048]{};
    const AudioFormat entryFormat = GetAudioFormatFromPath(entry.path.c_str());
    if (IsMediaFoundationFormat(entryFormat))
    {
        const std::string cacheUtf8 = ArchiveAdapterWideToUtf8(cachePath);
        MfAudioDecoder* mf = nullptr;
        if (cacheUtf8.empty() || !MfOpenCacheFileDecoder(cacheUtf8.c_str(), entryFormat, &mf) || !mf)
            return false;
        const int rate = MfGetSampleRate(mf);
        const int channels = MfGetChannels(mf);
        const int lengthMs = MfGetLengthMs(mf);
        bool valid = MfGetBitsPerSample(mf) == 16 && rate >= 8000 && rate <= 384000 &&
            (channels == 1 || channels == 2) && lengthMs > 1000;
        unsigned char buffer[4096]{};
        uint64_t exactAdts = 0;
        if (valid && entryFormat == AUDIO_FORMAT_AAC)
            valid = CountAdtsCachedFrames(cachePath.c_str(), rate, &exactAdts);
        if (valid)
            valid = MfReadPcm(mf, buffer, sizeof(buffer)) == sizeof(buffer);
        if (valid)
        {
            const int seekMs = entryFormat == AUDIO_FORMAT_AAC ?
                static_cast<int>(std::min<uint64_t>(exactAdts * 750u / rate, INT_MAX)) :
                static_cast<int>(static_cast<int64_t>(lengthMs) * 3 / 4);
            valid = MfSeekMs(mf, seekMs) && MfReadPcm(mf, buffer, sizeof(buffer)) == sizeof(buffer);
        }
        MfCloseDecoder(mf);
        return valid;
    }
    if (GetAudioFormatFromPath(entry.path.c_str()) == AUDIO_FORMAT_OGG)
    {
        ArchivePcm probe{};
        probe.format = AUDIO_FORMAT_OGG;
        probe.encodedSize = entry.size;
        if (_wfopen_s(&probe.cachedEncodedFile, cachePath.c_str(), L"rb") != 0 ||
            !probe.cachedEncodedFile)
            return false;
        HSTREAM decoded = OpenArchiveHostDecoder(&probe);
        if (!decoded)
        {
            if (probe.cachedEncodedFile)
                fclose(probe.cachedEncodedFile);
            return false;
        }
        BASS_CHANNELINFO info{};
        const bool metadata = BASS_ChannelGetInfo(decoded, &info) != FALSE &&
            info.freq >= 8000 && info.freq <= 384000 && (info.chans == 1 || info.chans == 2);
        const QWORD pcmLength = metadata ? BASS_ChannelGetLength(decoded, BASS_POS_BYTE) : static_cast<QWORD>(-1);
        bool valid = metadata && pcmLength != static_cast<QWORD>(-1) &&
            pcmLength > static_cast<QWORD>(info.freq) * info.chans * sizeof(int16_t);
        if (valid)
        {
            valid = BASS_ChannelGetData(decoded, pcm, sizeof(pcm)) == sizeof(pcm);
            const QWORD bytesPerFrame = info.chans * sizeof(int16_t);
            QWORD threeQuarters = (pcmLength / 4) * 3;
            threeQuarters -= threeQuarters % bytesPerFrame;
            if (valid)
                valid = BASS_ChannelSetPosition(decoded, threeQuarters, BASS_POS_BYTE) != FALSE &&
                    BASS_ChannelGetData(decoded, pcm, sizeof(pcm)) == sizeof(pcm);
        }
        BASS_StreamFree(decoded);
        if (probe.cachedEncodedFile)
        {
            fclose(probe.cachedEncodedFile);
            probe.cachedEncodedFile = nullptr;
        }
        return valid;
    }
    if (GetAudioFormatFromPath(entry.path.c_str()) == AUDIO_FORMAT_MP3)
    {
        drmp3 decoder{};
        if (!drmp3_init_file_w(&decoder, cachePath.c_str(), nullptr))
            return false;
        bool valid = decoder.sampleRate >= 8000 && decoder.sampleRate <= 384000 &&
            (decoder.channels == 1 || decoder.channels == 2);
        if (valid)
            valid = drmp3_read_pcm_frames_s16(&decoder, 1024, pcm) == 1024 &&
                drmp3_seek_to_pcm_frame(&decoder, decoder.sampleRate) &&
                drmp3_read_pcm_frames_s16(&decoder, 1024, pcm) == 1024;
        drmp3_uninit(&decoder);
        return valid;
    }
    if (GetAudioFormatFromPath(entry.path.c_str()) == AUDIO_FORMAT_WAV)
    {
        drwav wav{};
        if (!drwav_init_file_w(&wav, cachePath.c_str(), nullptr))
            return false;
        bool valid = wav.translatedFormatTag == DR_WAVE_FORMAT_PCM && wav.bitsPerSample == 16 &&
            wav.sampleRate >= 8000 && wav.sampleRate <= 384000 &&
            (wav.channels == 1 || wav.channels == 2) && wav.totalPCMFrameCount > wav.sampleRate;
        if (valid)
            valid = drwav_read_pcm_frames_s16(&wav, 1024, pcm) == 1024 &&
                drwav_seek_to_pcm_frame(&wav, wav.sampleRate) &&
                drwav_read_pcm_frames_s16(&wav, 1024, pcm) == 1024;
        drwav_uninit(&wav);
        return valid;
    }
    drflac* flac = drflac_open_file_w(cachePath.c_str(), nullptr);
    if (!flac)
        return false;
    bool valid = flac->sampleRate >= 8000 && flac->sampleRate <= 384000 &&
        (flac->channels == 1 || flac->channels == 2) &&
        (flac->bitsPerSample == 16 || flac->bitsPerSample == 24) &&
        flac->totalPCMFrameCount > flac->sampleRate;
    if (valid)
        valid = drflac_read_pcm_frames_s16(flac, 1024, pcm) == 1024 &&
            drflac_seek_to_pcm_frame(flac, flac->sampleRate) &&
            drflac_read_pcm_frames_s16(flac, 1024, pcm) == 1024;
    drflac_close(flac);
    return valid;
}

static bool PreflightSolidBufferedMp3Ogg(const char* archive, const ArchiveEntry& entry, AudioFormat format, unsigned memoryMb)
{
    if (!entry.isSolid || (format != AUDIO_FORMAT_MP3 && format != AUDIO_FORMAT_OGG) ||
        !entry.size || entry.size > static_cast<uint64_t>(memoryMb) * 1024u * 1024u)
        return false;
    std::vector<unsigned char> encoded;
    std::string error;
    if (!ArchiveExtractEntryToMemory(archive, entry.index, encoded, error) || encoded.size() != entry.size)
        return false;

    unsigned char pcm[8192]{};
    if (format == AUDIO_FORMAT_MP3)
    {
        drmp3 decoder{};
        if (!drmp3_init_memory(&decoder, encoded.data(), encoded.size(), nullptr))
            return false;
        const bool header = decoder.sampleRate >= 8000 && decoder.sampleRate <= 384000 &&
            (decoder.channels == 1 || decoder.channels == 2);
        bool valid = header && drmp3_read_pcm_frames_s16(&decoder, 1024, reinterpret_cast<drmp3_int16*>(pcm)) == 1024;
        if (valid)
            valid = drmp3_seek_to_pcm_frame(&decoder, decoder.sampleRate) &&
                drmp3_read_pcm_frames_s16(&decoder, 1024, reinterpret_cast<drmp3_int16*>(pcm)) == 1024;
        drmp3_uninit(&decoder);
        return valid;
    }

    auto check = std::make_unique<ArchivePcm>();
    check->format = AUDIO_FORMAT_OGG;
    check->encoded = std::move(encoded);
    check->encodedSize = check->encoded.size();
    check->memoryCursor = 0;
    check->hostStream = OpenArchiveHostDecoder(check.get());
    BASS_CHANNELINFO info{};
    bool valid = check->hostStream && BASS_ChannelGetInfo(check->hostStream, &info) &&
        info.freq >= 8000 && info.freq <= 384000 && (info.chans == 1 || info.chans == 2) &&
        BASS_ChannelGetLength(check->hostStream, BASS_POS_BYTE) != static_cast<QWORD>(-1);
    if (valid)
    {
        valid = BASS_ChannelGetData(check->hostStream, pcm, sizeof(pcm)) == sizeof(pcm);
        const QWORD second = static_cast<QWORD>(info.freq) * info.chans * sizeof(int16_t);
        if (valid && BASS_ChannelGetLength(check->hostStream, BASS_POS_BYTE) > second + sizeof(pcm))
            valid = BASS_ChannelSetPosition(check->hostStream, second, BASS_POS_BYTE) &&
                BASS_ChannelGetData(check->hostStream, pcm, sizeof(pcm)) == sizeof(pcm);
    }
    if (check->hostStream)
        BASS_StreamFree(check->hostStream);
    return valid;
}

// List-only native MusicBee integration: no separate helper executable, HTTP
// listener, PCM extraction or BASS runtime redistribution is required.
// The managed adapter calls this on its background archive-opening worker.
static bool PreflightNativeEncryptedMemory(const char* archive,
    const ArchiveEntry& entry, AudioFormat format, const ArchiveAdapterPreferences& preferences)
{
    if (!entry.isEncrypted || !entry.size ||
        entry.size > static_cast<uint64_t>(preferences.memoryMb) * 1024ull * 1024ull)
        return false;
    std::vector<unsigned char> encoded;
    std::string error;
    if (!ArchiveExtractEntryToMemory(archive, entry.index, encoded, error) ||
        encoded.size() != entry.size)
        return false;
    int16_t buffer[4096]{};
    if (format == AUDIO_FORMAT_WAV)
    {
        drwav wav{};
        if (!drwav_init_memory(&wav, encoded.data(), encoded.size(), nullptr))
            return false;
        const bool valid = wav.translatedFormatTag == DR_WAVE_FORMAT_PCM &&
            wav.bitsPerSample == 16 && wav.sampleRate >= 8000 && wav.channels >= 1 &&
            wav.channels <= 2 && wav.totalPCMFrameCount > wav.sampleRate &&
            drwav_read_pcm_frames_s16(&wav, 1024, buffer) == 1024 &&
            drwav_seek_to_pcm_frame(&wav, wav.sampleRate) &&
            drwav_read_pcm_frames_s16(&wav, 1024, buffer) == 1024;
        drwav_uninit(&wav);
        return valid;
    }
    if (format == AUDIO_FORMAT_FLAC)
    {
        drflac* flac = drflac_open_memory(encoded.data(), encoded.size(), nullptr);
        if (!flac)
            return false;
        const bool valid = flac->sampleRate >= 8000 && flac->channels >= 1 &&
            flac->channels <= 2 && (flac->bitsPerSample == 16 || flac->bitsPerSample == 24) &&
            flac->totalPCMFrameCount > flac->sampleRate &&
            drflac_read_pcm_frames_s16(flac, 1024, buffer) == 1024 &&
            drflac_seek_to_pcm_frame(flac, flac->sampleRate) &&
            drflac_read_pcm_frames_s16(flac, 1024, buffer) == 1024;
        drflac_close(flac);
        return valid;
    }
    if (format == AUDIO_FORMAT_MP3)
    {
        drmp3 mp3{};
        if (!drmp3_init_memory(&mp3, encoded.data(), encoded.size(), nullptr))
            return false;
        const bool valid = mp3.sampleRate >= 8000 && mp3.channels >= 1 &&
            mp3.channels <= 2 && drmp3_get_pcm_frame_count(&mp3) > mp3.sampleRate &&
            drmp3_read_pcm_frames_s16(&mp3, 1024, buffer) == 1024 &&
            drmp3_seek_to_pcm_frame(&mp3, mp3.sampleRate) &&
            drmp3_read_pcm_frames_s16(&mp3, 1024, buffer) == 1024;
        drmp3_uninit(&mp3);
        return valid;
    }
    if (format == AUDIO_FORMAT_OGG)
    {
        ArchivePcm probe{};
        probe.format = AUDIO_FORMAT_OGG;
        probe.encoded = std::move(encoded);
        probe.encodedSize = probe.encoded.size();
        const HSTREAM decoded = OpenArchiveHostDecoder(&probe);
        if (!decoded)
            return false;
        BASS_CHANNELINFO info{};
        const bool metadata = BASS_ChannelGetInfo(decoded, &info) != FALSE;
        const QWORD bytes = BASS_ChannelGetLength(decoded, BASS_POS_BYTE);
        const QWORD seek = static_cast<QWORD>(info.freq) * info.chans * sizeof(int16_t);
        const bool valid = metadata && info.freq >= 8000 &&
            (info.chans == 1 || info.chans == 2) &&
            bytes != static_cast<QWORD>(-1) && bytes > seek + sizeof(buffer) &&
            BASS_ChannelGetData(decoded, buffer, sizeof(buffer)) == sizeof(buffer) &&
            BASS_ChannelSetPosition(decoded, seek, BASS_POS_BYTE) &&
            BASS_ChannelGetData(decoded, buffer, sizeof(buffer)) == sizeof(buffer);
        BASS_StreamFree(decoded);
        return valid;
    }
    if (IsMediaFoundationFormat(format))
    {
        MfAudioDecoder* mf = nullptr;
        if (!MfOpenMemoryDecoder(encoded.data(), encoded.size(), format, &mf) || !mf)
            return false;
        const int rate = MfGetSampleRate(mf);
        const int lengthMs = MfGetLengthMs(mf);
        bool valid = MfGetBitsPerSample(mf) == 16 && rate >= 8000 &&
            (MfGetChannels(mf) == 1 || MfGetChannels(mf) == 2) && lengthMs > 1000;
        uint64_t aacFrames = 0;
        if (valid && format == AUDIO_FORMAT_AAC)
            valid = CountAdtsMemoryFrames(encoded, rate, &aacFrames);
        if (valid)
            valid = MfReadPcm(mf, buffer, sizeof(buffer)) == sizeof(buffer) &&
                MfSeekMs(mf, 1000) && MfReadPcm(mf, buffer, sizeof(buffer)) == sizeof(buffer);
        MfCloseDecoder(mf);
        return valid;
    }
    return false;
}

static int WriteNativeTracksInternal(const wchar_t* archivePath, const wchar_t* outputFolder, bool allowWav, bool allowFlac, bool allowOgg, bool allowMf, bool allowSolidBuffer, bool allowDiskPcm, bool allowDiskWav, bool allowDiskMp3, bool allowDiskOgg, bool allowDiskMf, bool allowEncrypted)
{
    if (!archivePath || !*archivePath || !outputFolder || !*outputFolder)
        return 0;
    try
    {
        if (allowEncrypted)
            MusicBeeInitializePasswordBackend();
        const std::filesystem::path archive(archivePath);
        const std::wstring ext = archive.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".zip") != 0 && _wcsicmp(ext.c_str(), L".7z") != 0 &&
            _wcsicmp(ext.c_str(), L".rar") != 0)
            return 0;

        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, archivePath, -1, nullptr, 0, nullptr, nullptr);
        if (required < 2 || required > MAX_PATH * 4)
            return 0;
        std::string archiveUtf8(static_cast<size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, archivePath, -1,
                                archiveUtf8.data(), required, nullptr, nullptr) != required)
            return 0;
        archiveUtf8.pop_back();
        if (allowEncrypted)
            ArchivePasswordClearArchiveCancelled(archiveUtf8.c_str());
        if (archiveUtf8.find('\r') != std::string::npos || archiveUtf8.find('\n') != std::string::npos)
            return 0;

        std::vector<ArchiveEntry> entries;
        std::string error;
        if (!ArchiveListEntries(archivePath, entries, error))
        {
            Trace("NATIVE_ENUM_FAIL", "reason=list_entries");
            return 0;
        }

        std::vector<const ArchiveEntry*> selected;
        for (const ArchiveEntry& entry : entries)
        {
            if (entry.isFolder || !entry.isSupportedAudio)
                continue;
            // Old scanner ABIs preserve their encrypted-entry rejection.
            // Only the new password-aware export can call native backends.
            const AudioFormat entryFormat = GetAudioFormatFromPath(entry.path.c_str());
            if (!entry.size || (entry.isEncrypted && !allowEncrypted) ||
                (entryFormat != AUDIO_FORMAT_MP3 && !(allowWav && entryFormat == AUDIO_FORMAT_WAV) &&
                 !(allowFlac && entryFormat == AUDIO_FORMAT_FLAC) && !(allowOgg && entryFormat == AUDIO_FORMAT_OGG) &&
                 !(allowMf && IsMediaFoundationFormat(entryFormat))) ||
                entry.path.empty() || entry.path.size() >= 1024 ||
                entry.path.find('\r') != std::string::npos ||
                entry.path.find('\n') != std::string::npos ||
                entry.path.find('\0') != std::string::npos)
                return 0;

            // Stream first. The v1.8 opt-in ABI additionally accepts proven,
            // unencrypted solid MP3/Vorbis via the configured bounded RAM buffer.
            // Other layouts continue to the bridge (RAM/disk-last there).
            const bool nonStreamingEncryptedRar = allowEncrypted && entry.isEncrypted &&
                _wcsicmp(ext.c_str(), L".rar") == 0;
            if (nonStreamingEncryptedRar ||
                ArchiveGetEntryStreamCapability(archiveUtf8.c_str(), entry.index) != ARCHIVE_STREAM_CAPABILITY_SUPPORTED)
            {
                const ArchiveAdapterPreferences preferences = ArchiveAdapterPreferencesLoad();
                const unsigned memoryMb = preferences.memoryMb;
                const bool buffered = (allowEncrypted && entry.isEncrypted &&
                    PreflightNativeEncryptedMemory(archiveUtf8.c_str(), entry, entryFormat, preferences)) ||
                    (allowSolidBuffer && !entry.isEncrypted &&
                     PreflightSolidBufferedMp3Ogg(archiveUtf8.c_str(), entry, entryFormat, memoryMb));
                const bool cached = !buffered && allowDiskPcm && (entryFormat == AUDIO_FORMAT_FLAC ||
                    (allowDiskWav && entryFormat == AUDIO_FORMAT_WAV) ||
                    (allowDiskMp3 && entryFormat == AUDIO_FORMAT_MP3) ||
                    (allowDiskOgg && entryFormat == AUDIO_FORMAT_OGG) ||
                    (allowDiskMf && IsMediaFoundationFormat(entryFormat))) &&
                    PreflightDiskLastAudio(archivePath, entry, preferences);
                if (!buffered && !cached)
                {
                    Trace("NATIVE_ENUM_FALLBACK", "reason=non_streamable_or_memory_cache_invalid");
                    return 0;
                }
                Trace(cached ? "NATIVE_ENUM_CACHE_OK" : "NATIVE_ENUM_BUFFER_OK", FormatNote(entryFormat));
                if (entry.isEncrypted && allowEncrypted)
                    MusicBeeCommitVerifiedPassword(archiveUtf8.c_str());
                selected.push_back(&entry);
                if (selected.size() > 10000)
                    return 0;
                continue;
            }
            std::string probeError;
            if (!ArchiveValidateEntryStreamRead(archiveUtf8.c_str(), entry.index, 4096, &probeError))
            {
                Trace("NATIVE_ENUM_FALLBACK", "reason=stream_probe_failed");
                return 0;
            }
            if (entryFormat == AUDIO_FORMAT_WAV)
            {
                ArchiveStreamContext wavStream{};
                if (!ArchiveOpenValidatedEntryStream(&wavStream, archiveUtf8.c_str(), entry.index, 4096, &probeError))
                    return 0;
                drwav wav{};
                const bool initialized = drwav_init(&wav, ReadEncoded, WavSeekEncoded, WavTellEncoded, &wavStream, nullptr) != 0;
                bool valid = false;
                if (initialized)
                {
                    // Native WAV acceptance currently covers validated PCM16.
                    // Other WAV variants retain the established host bridge
                    // until their PCM parity and GUI seek tests are certified.
                    valid = wav.translatedFormatTag == DR_WAVE_FORMAT_PCM && wav.bitsPerSample == 16 &&
                        wav.sampleRate >= 8000 && wav.sampleRate <= 384000 &&
                        (wav.channels == 1 || wav.channels == 2) && wav.totalPCMFrameCount > 0;
                    drwav_uninit(&wav);
                }
                ArchiveCloseEntryStream(&wavStream);
                if (!valid)
                {
                    Trace("NATIVE_ENUM_FALLBACK", "reason=wav_header_invalid");
                    return 0;
                }
            }

            if (entryFormat == AUDIO_FORMAT_FLAC)
            {
                ArchiveStreamContext flacStream{};
                if (!ArchiveOpenValidatedEntryStream(&flacStream, archiveUtf8.c_str(), entry.index, 4096, &probeError))
                    return 0;
                drflac* flac = drflac_open(ReadEncoded, FlacSeekEncoded, FlacTellEncoded, &flacStream, nullptr);
                bool valid = false;
                if (flac)
                {
                    valid = flac->sampleRate >= 8000 && flac->sampleRate <= 384000 &&
                        (flac->channels == 1 || flac->channels == 2) &&
                        (flac->bitsPerSample == 16 || flac->bitsPerSample == 24) &&
                        flac->totalPCMFrameCount > 0;
                    drflac_close(flac);
                }
                ArchiveCloseEntryStream(&flacStream);
                if (!valid)
                {
                    Trace("NATIVE_ENUM_FALLBACK", "reason=flac_header_invalid");
                    return 0;
                }
            }

            if (entryFormat == AUDIO_FORMAT_OGG || IsMediaFoundationFormat(entryFormat))
            {
                // Validate actual compressed audio signatures, not extensions.
                // Non-Vorbis OGG and non-MF-compatible input retain the bridge.
                ArchiveStreamContext probe{};
                if (!ArchiveOpenValidatedEntryStream(&probe, archiveUtf8.c_str(), entry.index, 4096, &probeError))
                    return 0;
                unsigned char prefix[4096]{};
                const size_t count = ArchiveReadEntryStream(&probe, prefix, sizeof(prefix));
                ArchiveCloseEntryStream(&probe);
                const unsigned char vorbisMagic[] = { 1, 'v', 'o', 'r', 'b', 'i', 's' };
                const unsigned char asfGuid[] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                    0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
                bool validHeader = false;
                if (entryFormat == AUDIO_FORMAT_OGG)
                    validHeader = count >= 36 && memcmp(prefix, "OggS", 4) == 0 &&
                        std::search(prefix, prefix + count, std::begin(vorbisMagic), std::end(vorbisMagic)) != prefix + count;
                else if (entryFormat == AUDIO_FORMAT_AAC)
                    validHeader = count >= 7 && prefix[0] == 0xFF && (prefix[1] & 0xF6) == 0xF0;
                else if (entryFormat == AUDIO_FORMAT_M4A)
                    validHeader = count >= 12 && memcmp(prefix + 4, "ftyp", 4) == 0;
                else if (entryFormat == AUDIO_FORMAT_WMA)
                    validHeader = count >= sizeof(asfGuid) && memcmp(prefix, asfGuid, sizeof(asfGuid)) == 0;
                if (!validHeader)
                {
                    Trace("NATIVE_ENUM_FALLBACK", "reason=encoded_header_invalid");
                    return 0;
                }
                // The actual shared decoder must open, read and seek the
                // compressed archive stream before any native queueing.
                if (IsMediaFoundationFormat(entryFormat))
                {
                    MfAudioDecoder* probeMf = nullptr;
                    bool valid = MfOpenArchiveDecoder(archiveUtf8.c_str(), entry.index, entry.size, entryFormat, &probeMf);
                    if (valid)
                    {
                        valid = MfGetBitsPerSample(probeMf) == 16 &&
                            MfGetSampleRate(probeMf) >= 8000 && MfGetSampleRate(probeMf) <= 384000 &&
                            (MfGetChannels(probeMf) == 1 || MfGetChannels(probeMf) == 2) &&
                            MfGetLengthMs(probeMf) > 1000;
                        unsigned char pcmProbe[4096]{};
                        if (valid)
                            valid = MfReadPcm(probeMf, pcmProbe, sizeof(pcmProbe)) == sizeof(pcmProbe);
                        if (valid)
                            valid = MfSeekMs(probeMf, 1000) &&
                                MfReadPcm(probeMf, pcmProbe, sizeof(pcmProbe)) == sizeof(pcmProbe);
                        if (valid && entryFormat == AUDIO_FORMAT_AAC)
                        {
                            uint64_t exactFrames = 0;
                            valid = CountAdtsPcmFrames(archiveUtf8.c_str(), entry.index, entry.size,
                                static_cast<DWORD>(MfGetSampleRate(probeMf)), &exactFrames);
                        }
                    }
                    if (probeMf)
                        MfCloseDecoder(probeMf);
                    if (!valid)
                    {
                        Trace("NATIVE_ENUM_FALLBACK", "reason=mf_decoder_invalid");
                        return 0;
                    }
                }
                else
                {
                    auto check = std::make_unique<ArchivePcm>();
                    check->format = entryFormat;
                    check->encodedSize = entry.size;
                    check->isStreaming = ArchiveOpenValidatedEntryStream(&check->stream, archiveUtf8.c_str(), entry.index, 4096, &probeError);
                    if (!check->isStreaming)
                        return 0;
                    check->hostStream = OpenArchiveHostDecoder(check.get());
                    BASS_CHANNELINFO info{};
                    bool valid = check->hostStream && BASS_ChannelGetInfo(check->hostStream, &info) &&
                        info.freq >= 8000 && info.freq <= 384000 && (info.chans == 1 || info.chans == 2) &&
                        BASS_ChannelGetLength(check->hostStream, BASS_POS_BYTE) != static_cast<QWORD>(-1);
                    if (valid)
                    {
                        // Verify PCM output and a real seek, not merely metadata.
                        unsigned char pcmProbe[4096]{};
                        valid = BASS_ChannelGetData(check->hostStream, pcmProbe, sizeof(pcmProbe)) == sizeof(pcmProbe);
                        const QWORD seekTarget = static_cast<QWORD>(info.freq) * info.chans * sizeof(int16_t);
                        if (valid && BASS_ChannelGetLength(check->hostStream, BASS_POS_BYTE) > seekTarget + sizeof(pcmProbe))
                            valid = BASS_ChannelSetPosition(check->hostStream, seekTarget, BASS_POS_BYTE) &&
                                BASS_ChannelGetData(check->hostStream, pcmProbe, sizeof(pcmProbe)) == sizeof(pcmProbe);
                    }
                    if (check->hostStream)
                        BASS_StreamFree(check->hostStream);
                    if (check->isStreaming)
                        ArchiveCloseEntryStream(&check->stream);
                    if (!valid)
                    {
                        Trace("NATIVE_ENUM_FALLBACK", "reason=host_decoder_invalid");
                        return 0;
                    }
                }
            }

            const std::string virtualPath = archiveUtf8 + ".archive_stream|" +
                std::to_string(entry.index) + "|" + entry.path;
            if (virtualPath.size() > 16300)
                return 0;
            if (entry.isEncrypted && allowEncrypted)
                MusicBeeCommitVerifiedPassword(archiveUtf8.c_str());
            selected.push_back(&entry);
            if (selected.size() > 10000)
                return 0;
        }
        if (selected.empty())
            return 0;

        std::error_code ec;
        std::filesystem::create_directories(outputFolder, ec);
        if (ec)
            return 0;
        for (const ArchiveEntry* entry : selected)
        {
            wchar_t name[32]{};
            if (swprintf_s(name, L"entry-%08u.ast", static_cast<unsigned>(entry->index)) <= 0)
                return 0;
            const std::filesystem::path manifest = std::filesystem::path(outputFolder) / name;
            const std::string contents = "ARCHIVE_STREAM_NATIVE_TRACK_V1\n" + archiveUtf8 +
                ".archive_stream|" + std::to_string(entry->index) + "|" + entry->path + "\n";
            if (contents.size() > 16384)
                return 0;
            std::ofstream file(manifest, std::ios::binary | std::ios::trunc);
            if (!file)
                return 0;
            file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (!file.good())
                return 0;
        }
        Trace("NATIVE_ENUM_OK", "route=IN_PROCESS");
        return static_cast<int>(selected.size());
    }
    catch (...)
    {
        Trace("NATIVE_ENUM_FAIL", "reason=exception");
        return 0;
    }
}

// The original ABI stays MP3-only for installed v1.2 clients. The new export
// is used by v1.3+ for mixed MP3/WAV albums, without changing old clients.
extern "C" int __stdcall ArchiveStreamWriteNativeMp3TracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, false, false, false, false, false, false, false, false, false, false, false);
}

extern "C" int __stdcall ArchiveStreamWriteNativePcmTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, false, false, false, false, false, false, false, false, false, false);
}

// New ABI retains older MP3-only and MP3/WAV scanners for installed adapters.
extern "C" int __stdcall ArchiveStreamWriteNativeAudioTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, false, false, false, false, false, false, false, false, false);
}

// v1.5 accepts actual Vorbis OGG files; older ABI selectors remain unchanged.
extern "C" int __stdcall ArchiveStreamWriteNativeFourCodecsW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, false, false, false, false, false, false, false, false);
}

// v1.6 extends the native BASS byte-stream path to Media Foundation codecs,
// without changing the older four in-process scanner ABIs.
extern "C" int __stdcall ArchiveStreamWriteNativeSevenCodecsW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, false, false, false, false, false, false, false);
}

// v1.8 adds tested unencrypted solid 7z/RAR MP3 and Vorbis through bounded
// memory while preserving all earlier scanner ABI contracts.
extern "C" int __stdcall ArchiveStreamWriteNativeBufferedTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, false, false, false, false, false, false);
}

// v1.9 opt-in: retain direct stream and bounded memory; only validated
// over-budget solid FLAC may fall back to the shared managed disk cache.
extern "C" int __stdcall ArchiveStreamWriteNativeDiskLastTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, true, false, false, false, false, false);
}

// v1.10 opt-in adds PCM16 WAV to the shared cache fallback while retaining
// the v1.9 FLAC-only API for older clients.
extern "C" int __stdcall ArchiveStreamWriteNativeDiskPcmTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, true, true, false, false, false, false);
}

// v1.11 opt-in adds oversized unencrypted MP3 to the shared encoded-file
// cache route; older exports preserve their original codec contracts.
extern "C" int __stdcall ArchiveStreamWriteNativeDiskMp3TracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, true, true, true, false, false, false);
}

// v1.12 adds oversized Vorbis OGG disk-last decoding using the installed
// BASS decoder, without changing any of the older public scanner ABIs.
extern "C" int __stdcall ArchiveStreamWriteNativeDiskOggTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, true, true, true, true, false, false);
}

// v1.13 opt-in: AAC/M4A/WMA also use shared disk-last cache when the
// archive is nonstreamable and exceeds the configured bounded RAM buffer.
extern "C" int __stdcall ArchiveStreamWriteNativeDiskMfTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true, true, true, true, true, true, true, true, true, false);
}

// v1.17 opt-in: native 7z/RAR encrypted audio; old scanner exports keep their
// previous unencrypted-only rules. -2=cancelled, -3=rejected credential.
extern "C" int __stdcall ArchiveStreamWriteNativeEncryptedTracksW(
    const wchar_t* archivePath, const wchar_t* outputFolder)
{
    gNativePasswordAttempted = false;
    gNativePasswordCancelled = false;
    const int count = WriteNativeTracksInternal(archivePath, outputFolder,
        true, true, true, true, true, true, true, true, true, true, true);
    if (count > 0)
        return count;
    if (count <= 0 && archivePath)
    {
        const auto archiveUtf8 = ArchiveAdapterWideToUtf8(archivePath);
        MusicBeeForgetPendingPassword(archiveUtf8.c_str());
    }
    if (gNativePasswordCancelled)
        return -2;
    if (gNativePasswordAttempted)
        return -3;
    return count;
}

// Shared title-preset renderer for host adapters. Using the same
// ArchiveAdapterFormatTitle implementation avoids duplicating six presets
// in the managed MusicBee plug-in. This NEVER writes archive/member bytes.
extern "C" int __stdcall ArchiveStreamFormatNativeTitleW(const wchar_t* virtualTrack, wchar_t* output, DWORD capacity)
{
    if (!virtualTrack || !*virtualTrack || !output || capacity < 2 || capacity > 4096)
        return 0;
    output[0] = L'\0';
    try
    {
        const std::string encoded = ArchiveAdapterWideToUtf8(virtualTrack);
        if (encoded.empty() || encoded.size() > 16384)
            return 0;
        char archive[MAX_PATH * 4]{};
        char member[1024]{};
        mz_uint index = 0;
        if (!ParseVirtualArchivePath(encoded.c_str(), archive, sizeof(archive), &index, member, sizeof(member)))
            return 0;
        const std::wstring title = ArchiveAdapterPreferredTitle(ArchiveAdapterUtf8ToWide(archive),
            ArchiveAdapterUtf8ToWide(member), static_cast<unsigned>(index) + 1,
            ArchiveAdapterPreferencesLoad());
        if (title.empty() || title.size() >= capacity)
            return 0;
        std::copy_n(title.c_str(), title.size() + 1, output);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

// A bounded, read-only metadata probe for MusicBee's background title update.
// Do not use for solid/encrypted entries: those must never be decompressed just
// to populate playlist metadata. Percent-encoded UTF-8 TSV avoids delimiter
// injection, leaves archive bytes intact and never generates content CRCs.
extern "C" int __stdcall ArchiveStreamReadNativeFileTagsW(const wchar_t* virtualTrack, wchar_t* output, DWORD capacity)
{
    if (!virtualTrack || !*virtualTrack || !output || capacity < 2 || capacity > 16384)
        return 0;
    output[0] = L'\0';
    try
    {
        const std::string virtualUtf8 = ArchiveAdapterWideToUtf8(virtualTrack);
        if (virtualUtf8.empty() || virtualUtf8.size() > 16384)
            return 0;
        char archive[MAX_PATH * 4]{};
        char member[1024]{};
        mz_uint index = 0;
        if (!ParseVirtualArchivePath(virtualUtf8.c_str(), archive, sizeof(archive), &index, member, sizeof(member)))
            return 0;
        const std::string ext = std::filesystem::path(member).extension().string();
        if (ext != ".mp3" && ext != ".flac" && ext != ".ogg" && ext != ".opus")
            return 0;
        if (ArchiveGetEntryStreamCapability(archive, index) != ARCHIVE_STREAM_CAPABILITY_SUPPORTED)
            return 0;

        ArchiveStreamContext stream{};
        std::string error;
        if (!ArchiveOpenValidatedEntryStream(&stream, archive, index, 4096, &error))
            return 0;
        constexpr size_t maxTagPrefix = 256 * 1024;
        std::vector<unsigned char> prefix(maxTagPrefix);
        size_t bytes = 0;
        while (bytes < prefix.size())
        {
            const size_t count = ArchiveReadEntryStream(&stream, prefix.data() + bytes, prefix.size() - bytes);
            if (!count)
                break;
            bytes += count;
            // Try parsing after each read; most ID3 and Vorbis tags are small.
            ArchiveTrackTags early{};
            if (bytes >= 4096 && ArchiveTrackTagParser::Parse(ext.substr(1), prefix.data(), bytes, early) && early.Any())
                break;
        }
        ArchiveCloseEntryStream(&stream);
        ArchiveTrackTags tags{};
        if (!ArchiveTrackTagParser::Parse(ext.substr(1), prefix.data(), bytes, tags) || !tags.Any())
            return 0;
        const std::string encoded = ArchiveTrackTagParser::Escape(tags.title) + "\t" +
            ArchiveTrackTagParser::Escape(tags.artist) + "\t" +
            ArchiveTrackTagParser::Escape(tags.album) + "\t" +
            ArchiveTrackTagParser::Escape(tags.albumArtist) + "\t" +
            std::to_string(tags.trackNumber) + "\t" +
            ArchiveTrackTagParser::Escape(tags.genre) + "\t" +
            ArchiveTrackTagParser::Escape(tags.year);
        const std::wstring wide = ArchiveAdapterUtf8ToWide(encoded);
        if (wide.empty() || wide.size() >= capacity)
            return 0;
        std::copy_n(wide.c_str(), wide.size() + 1, output);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

extern "C" int __stdcall ArchiveStreamSupportsMusicBeeTagAlias() { return 2; }

extern "C" const void* WINAPI BASSplugin(DWORD face)
{
    static const BASS_PLUGINFORM formats[] = {
        // MusicBee stores persistent archive descriptors behind .mp3 hard-link
        // aliases. The decoder strictly verifies the linked .ast descriptor;
        // ordinary MP3 files are rejected here and remain with MusicBee/BASS.
        {kArchiveCtype, "Archive Stream native track", "*.ast;*.mp3"}
    };
    static const BASS_PLUGININFO info = {kFormatVersion, 1, formats};
    switch (face)
    {
    case BASSPLUGIN_INFO:
        return &info;
    case BASSPLUGIN_CREATE:
        return reinterpret_cast<const void*>(CreateArchiveStream);
    default:
        return nullptr;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (HIWORD(BASS_GetVersion()) != BASSVERSION || !GetBassFunc())
            return FALSE;
        GetBassFunc2();
    }
    return TRUE;
}
