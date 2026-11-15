// Native BASS archive decoder prototype, x86 MusicBee first.
// Proprietary BASS SDK headers/libs are external build-time inputs and must
// never be redistributed. No HTTP server or runtime vendor DLL is bundled.
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <algorithm>

#include "bass-addon.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "native_track_manifest.h"
#include "dr_mp3.h"
#include "archive_adapter_preferences.h"

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

struct ArchiveMp3
{
    HSTREAM handle = 0;
    BASSFILE descriptor = nullptr;
    ArchiveStreamContext stream{};
    std::vector<unsigned char> encoded;
    drmp3 decoder{};
    bool opened = false;
    bool isStreaming = false;
    bool firstRead = false;
    DWORD channels = 0;
    DWORD sampleRate = 0;
    QWORD pcmBytes = 0;

    static const ADDON_FUNCTIONS callbacks;
    static void WINAPI Free(void* ptr);
    static QWORD WINAPI Length(void* ptr, DWORD mode);
    static void WINAPI Info(void* ptr, BASS_CHANNELINFO* info);
    static BOOL WINAPI CanSeek(void* ptr, QWORD pos, DWORD mode);
    static QWORD WINAPI Seek(void* ptr, QWORD pos, DWORD mode);
    static DWORD CALLBACK ReadPcm(HSTREAM handle, void* buffer, DWORD length, void* ptr);
};

static size_t ReadEncoded(void* context, void* output, size_t length)
{
    return ArchiveReadEntryStream(static_cast<ArchiveStreamContext*>(context), output, length);
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

const ADDON_FUNCTIONS ArchiveMp3::callbacks = {
    ADDON_LOCK,
    Free,
    Length,
    nullptr,
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

void WINAPI ArchiveMp3::Free(void* ptr)
{
    auto* state = static_cast<ArchiveMp3*>(ptr);
    if (!state)
        return;
    if (state->opened)
        drmp3_uninit(&state->decoder);
    if (state->isStreaming)
        ArchiveCloseEntryStream(&state->stream);
    delete state;
    Trace("CLOSE", "");
}

QWORD WINAPI ArchiveMp3::Length(void* ptr, DWORD mode)
{
    const auto* state = static_cast<ArchiveMp3*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE)
        errorn(BASS_ERROR_NOTAVAIL);
    noerrorn(state->pcmBytes);
}

void WINAPI ArchiveMp3::Info(void* ptr, BASS_CHANNELINFO* info)
{
    if (info)
        info->ctype = kArchiveCtype;
}

BOOL WINAPI ArchiveMp3::CanSeek(void* ptr, QWORD bytePosition, DWORD mode)
{
    const auto* state = static_cast<ArchiveMp3*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE)
        error(BASS_ERROR_NOTAVAIL);
    if (!state->channels || bytePosition > state->pcmBytes ||
        bytePosition % (state->channels * sizeof(int16_t)) != 0)
        error(BASS_ERROR_POSITION);
    return TRUE;
}

QWORD WINAPI ArchiveMp3::Seek(void* ptr, QWORD bytePosition, DWORD mode)
{
    auto* state = static_cast<ArchiveMp3*>(ptr);
    if (!state || (mode & 0xFF) != BASS_POS_BYTE || !state->channels)
        errorn(BASS_ERROR_NOTAVAIL);
    const QWORD frame = bytePosition / (state->channels * sizeof(int16_t));
    if (!drmp3_seek_to_pcm_frame(&state->decoder, frame))
    {
        Trace("SEEK_FAIL", "");
        errorn(BASS_ERROR_POSITION);
    }
    Trace("SEEK_OK", "");
    noerrorn(bytePosition);
}

DWORD CALLBACK ArchiveMp3::ReadPcm(HSTREAM, void* buffer, DWORD length, void* ptr)
{
    auto* state = static_cast<ArchiveMp3*>(ptr);
    if (!state || !buffer || !state->channels)
        return BASS_STREAMPROC_END;

    const DWORD bytesPerFrame = state->channels * sizeof(int16_t);
    const DWORD frames = length / bytesPerFrame;
    if (!frames)
        return 0;

    const drmp3_uint64 read = drmp3_read_pcm_frames_s16(&state->decoder, frames, static_cast<int16_t*>(buffer));
    if (!state->firstRead && read > 0)
    {
        state->firstRead = true;
        Trace("PCM_FIRST", state->isStreaming ? "route=STREAM" : "route=BUFFER");
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

static HSTREAM WINAPI CreateArchiveStream(BASSFILE descriptor, DWORD flags)
{
    Trace("CREATE_BEGIN", "codec=MP3");
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
    if (!ArchiveNativeTrackRead(descriptorPath.c_str(), virtualTrack))
    {
        Trace("MANIFEST_READ_FAIL", "invalid_or_unreadable_ast");
        return 0;
    }
    Trace("MANIFEST_READ_OK", "");

    char archive[MAX_PATH * 4]{};
    char member[1024]{};
    mz_uint index = 0;
    if (!ParseVirtualArchivePath(virtualTrack.c_str(), archive, sizeof(archive), &index, member, sizeof(member)) ||
        GetAudioFormatFromPath(member) != AUDIO_FORMAT_MP3)
        return 0;

    Trace("MANIFEST_OK", "codec=MP3");
    auto state = std::make_unique<ArchiveMp3>();
    std::string error;
    state->isStreaming = ArchiveOpenValidatedEntryStream(&state->stream, archive, index, 4096, &error);
    Trace("STREAM_VALIDATED", state->isStreaming ? "route=STREAM" : "route=FALLBACK");
    if (state->isStreaming)
    {
        state->opened = drmp3_init(&state->decoder, ReadEncoded, SeekEncoded, TellEncoded, nullptr, &state->stream, nullptr) != 0;
        if (!state->opened)
        {
            ArchiveCloseEntryStream(&state->stream);
            state->isStreaming = false;
        }
    }

    if (!state->opened)
    {
        uint64_t entryLength = 0;
        char entryName[1024]{};
        const unsigned memoryMb = ArchiveAdapterPreferencesLoad().memoryMb;
        if (!ArchiveGetEntryInfo(archive, index, entryName, sizeof(entryName), &entryLength) ||
            !entryLength || entryLength > static_cast<uint64_t>(memoryMb) * 1024u * 1024u)
        {
            Trace("OPEN_FAIL", "reason=stream_unsupported_or_memory_limit");
            return 0; // disk cache fallback is not implemented in this prototype
        }
        if (!ArchiveExtractEntryToMemory(archive, index, state->encoded, error))
        {
            Trace("OPEN_FAIL", "reason=memory_extract_failed");
            return 0;
        }
        state->opened = drmp3_init_memory(&state->decoder, state->encoded.data(), state->encoded.size(), nullptr) != 0;
    }

    if (!state->opened || !state->decoder.sampleRate ||
        (state->decoder.channels != 1 && state->decoder.channels != 2))
    {
        Trace("OPEN_FAIL", "reason=mp3_decoder_invalid");
        if (state->opened)
            drmp3_uninit(&state->decoder);
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        return 0;
    }

    state->sampleRate = state->decoder.sampleRate;
    state->channels = state->decoder.channels;
    Trace("MP3_INIT_OK", "");
    Trace("DURATION_COUNT_BEGIN", "");
    const uint64_t frames = drmp3_get_pcm_frame_count(&state->decoder);
    Trace("DURATION_COUNT_DONE", "");
    if (!frames || frames > std::numeric_limits<QWORD>::max() / (state->channels * sizeof(int16_t)))
    {
        drmp3_uninit(&state->decoder);
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        Trace("OPEN_FAIL", "reason=invalid_length");
        return 0;
    }
    state->pcmBytes = frames * state->channels * sizeof(int16_t);
    state->descriptor = descriptor;

    DWORD streamFlags = flags & (BASS_STREAM_DECODE | BASS_SAMPLE_SOFTWARE | BASS_SAMPLE_LOOP |
                                 BASS_STREAM_AUTOFREE | BASS_SAMPLE_3D | BASS_SAMPLE_FX | 0x3f000000);
    HSTREAM stream = bassfunc->CreateStream(state->sampleRate, state->channels, streamFlags,
                                           ArchiveMp3::ReadPcm, state.get(), &ArchiveMp3::callbacks);
    if (!stream)
    {
        drmp3_uninit(&state->decoder);
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        Trace("OPEN_FAIL", "reason=bass_stream_creation");
        return 0;
    }

    state->handle = stream;
    bassfunc->file.SetStream(descriptor, stream);
    state.release();
    if (BASS_GetVersion() >= 0x02041000)
        BASS_ChannelLock(stream, FALSE);
    Trace("OPEN_OK", "codec=MP3 route=STREAM_or_BUFFER");
    return stream;
}

extern "C" const void* WINAPI BASSplugin(DWORD face)
{
    static const BASS_PLUGINFORM formats[] = {
        {kArchiveCtype, "Archive Stream native track", "*.ast"}
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
