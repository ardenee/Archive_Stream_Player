// Native BASS archive PCM decoder for x86 MusicBee.
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
#include <filesystem>
#include <fstream>
#include "archive_backend.h"

#include "bass-addon.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "native_track_manifest.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"
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

struct ArchivePcm
{
    HSTREAM handle = 0;
    BASSFILE descriptor = nullptr;
    ArchiveStreamContext stream{};
    std::vector<unsigned char> encoded;
    drmp3 decoder{};
    drwav wav{};
    drflac* flac = nullptr;
    AudioFormat format = AUDIO_FORMAT_MP3;
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

static void CloseDecoder(ArchivePcm* state)
{
    if (!state || !state->opened)
        return;
    if (state->format == AUDIO_FORMAT_WAV)
        drwav_uninit(&state->wav);
    else if (state->format == AUDIO_FORMAT_FLAC)
        drflac_close(state->flac);
    else
        drmp3_uninit(&state->decoder);
    state->opened = false;
    state->flac = nullptr;
}

const ADDON_FUNCTIONS ArchivePcm::callbacks = {
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
    const bool ok = state->format == AUDIO_FORMAT_WAV
        ? drwav_seek_to_pcm_frame(&state->wav, frame) != 0
        : state->format == AUDIO_FORMAT_FLAC
            ? drflac_seek_to_pcm_frame(state->flac, frame) != 0
            : drmp3_seek_to_pcm_frame(&state->decoder, frame) != 0;
    if (!ok)
    {
        Trace("SEEK_FAIL", "");
        errorn(BASS_ERROR_POSITION);
    }
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

    const uint64_t read = state->format == AUDIO_FORMAT_WAV
        ? drwav_read_pcm_frames_s16(&state->wav, frames, static_cast<int16_t*>(buffer))
        : state->format == AUDIO_FORMAT_FLAC
            ? drflac_read_pcm_frames_s16(state->flac, frames, static_cast<int16_t*>(buffer))
            : drmp3_read_pcm_frames_s16(&state->decoder, frames, static_cast<int16_t*>(buffer));
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
    Trace("CREATE_BEGIN", "codec=MP3_WAV_FLAC");
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
    if (!ParseVirtualArchivePath(virtualTrack.c_str(), archive, sizeof(archive), &index, member, sizeof(member)))
        return 0;
    const AudioFormat format = GetAudioFormatFromPath(member);
    if (format != AUDIO_FORMAT_MP3 && format != AUDIO_FORMAT_WAV && format != AUDIO_FORMAT_FLAC)
        return 0;

    Trace("MANIFEST_OK", format == AUDIO_FORMAT_WAV ? "codec=WAV" : format == AUDIO_FORMAT_FLAC ? "codec=FLAC" : "codec=MP3");
    auto state = std::make_unique<ArchivePcm>();
    state->format = format;
    std::string error;
    state->isStreaming = ArchiveOpenValidatedEntryStream(&state->stream, archive, index, 4096, &error);
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
        else
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
        if (state->format == AUDIO_FORMAT_WAV)
            state->opened = drwav_init_memory(&state->wav, state->encoded.data(), state->encoded.size(), nullptr) != 0;
        else if (state->format == AUDIO_FORMAT_FLAC)
        {
            state->flac = drflac_open_memory(state->encoded.data(), state->encoded.size(), nullptr);
            state->opened = state->flac != nullptr;
        }
        else
            state->opened = drmp3_init_memory(&state->decoder, state->encoded.data(), state->encoded.size(), nullptr) != 0;
    }

    const DWORD rate = state->format == AUDIO_FORMAT_WAV ? state->wav.sampleRate :
        state->format == AUDIO_FORMAT_FLAC && state->flac ? state->flac->sampleRate : state->decoder.sampleRate;
    const DWORD channels = state->format == AUDIO_FORMAT_WAV ? state->wav.channels :
        state->format == AUDIO_FORMAT_FLAC && state->flac ? state->flac->channels : state->decoder.channels;
    if (!state->opened || !rate || (channels != 1 && channels != 2))
    {
        Trace("OPEN_FAIL", "reason=decoder_invalid");
        CloseDecoder(state.get());
        if (state->isStreaming)
            ArchiveCloseEntryStream(&state->stream);
        return 0;
    }

    state->sampleRate = rate;
    state->channels = channels;
    Trace("DECODER_INIT_OK", state->format == AUDIO_FORMAT_WAV ? "codec=WAV" : state->format == AUDIO_FORMAT_FLAC ? "codec=FLAC" : "codec=MP3");
    Trace("DURATION_COUNT_BEGIN", "");
    const uint64_t frames = state->format == AUDIO_FORMAT_WAV ? state->wav.totalPCMFrameCount :
        state->format == AUDIO_FORMAT_FLAC ? state->flac->totalPCMFrameCount : drmp3_get_pcm_frame_count(&state->decoder);
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
    Trace("BASS_CREATE_STREAM_BEGIN", state->format == AUDIO_FORMAT_WAV ? "codec=WAV" : state->format == AUDIO_FORMAT_FLAC ? "codec=FLAC" : "codec=MP3");
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
    bassfunc->file.SetStream(descriptor, stream);
    state.release();
    if (BASS_GetVersion() >= 0x02041000)
        BASS_ChannelLock(stream, FALSE);
    Trace("OPEN_OK", openedFormat == AUDIO_FORMAT_WAV ? "codec=WAV route=STREAM_or_BUFFER" :
        openedFormat == AUDIO_FORMAT_FLAC ? "codec=FLAC route=STREAM_or_BUFFER" : "codec=MP3 route=STREAM_or_BUFFER");
    return stream;
}

// List-only native MusicBee integration: no separate helper executable, HTTP
// listener, PCM extraction or BASS runtime redistribution is required.
// The managed adapter calls this on its background archive-opening worker.
static int WriteNativeTracksInternal(const wchar_t* archivePath, const wchar_t* outputFolder, bool allowWav, bool allowFlac)
{
    if (!archivePath || !*archivePath || !outputFolder || !*outputFolder)
        return 0;
    try
    {
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
            // Fail closed for mixed, encrypted and missing audio. The existing
            // bridge handles unsupported layouts and their password prompts.
            const AudioFormat entryFormat = GetAudioFormatFromPath(entry.path.c_str());
            if (!entry.size || entry.isEncrypted ||
                (entryFormat != AUDIO_FORMAT_MP3 && !(allowWav && entryFormat == AUDIO_FORMAT_WAV) &&
                 !(allowFlac && entryFormat == AUDIO_FORMAT_FLAC)) ||
                entry.path.empty() || entry.path.size() >= 1024 ||
                entry.path.find('\r') != std::string::npos ||
                entry.path.find('\n') != std::string::npos ||
                entry.path.find('\0') != std::string::npos)
                return 0;

            // This native prototype has no disk-last recovery yet. Only queue
            // entries that the shared backend can actually stream; all others
            // retain the existing bridge's stream/RAM/disk-last recovery policy.
            if (ArchiveGetEntryStreamCapability(archiveUtf8.c_str(), entry.index) != ARCHIVE_STREAM_CAPABILITY_SUPPORTED)
            {
                Trace("NATIVE_ENUM_FALLBACK", "reason=non_streamable_entry");
                return 0;
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

            const std::string virtualPath = archiveUtf8 + ".archive_stream|" +
                std::to_string(entry.index) + "|" + entry.path;
            if (virtualPath.size() > 16300)
                return 0;
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
    return WriteNativeTracksInternal(archivePath, outputFolder, false, false);
}

extern "C" int __stdcall ArchiveStreamWriteNativePcmTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, false);
}

// New ABI retains older MP3-only and MP3/WAV scanners for installed adapters.
extern "C" int __stdcall ArchiveStreamWriteNativeAudioTracksW(const wchar_t* archivePath, const wchar_t* outputFolder)
{
    return WriteNativeTracksInternal(archivePath, outputFolder, true, true);
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
