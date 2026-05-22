#pragma once

#define NOMINMAX

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "archive_types.h"

/*
    Compatibility integer aliases.

    Older backend/public APIs still use mz_uint, mz_uint32, and mz_uint64 in
    function signatures and structs. ZIP no longer uses miniz internally, but
    keeping these aliases avoids a large unrelated API rename while removing the
    miniz dependency from ZIP handling.
*/
using mz_uint = uint32_t;
using mz_uint32 = uint32_t;
using mz_uint64 = uint64_t;

#ifndef WM_WA_MPEG_EOF
#define WM_WA_MPEG_EOF (WM_USER + 2)
#endif

////--------------------
// Dup file contents
//--------------------
static constexpr uint64_t ARCHIVE_MAX_EXTRACT_TO_MEMORY_BYTES = 512ull * 1024ull * 1024ull;
bool ArchiveIsSupportedAudioPath(const std::string& path);
bool ArchiveIsValidUtf8(const char* text);
std::string ArchiveCp437ToUtf8(const char* text);
std::string ArchiveZipNameToUtf8BestEffort(const char* zipName);
////--------------------

static const char* VIRTUAL_PREFIX = "archive_stream:";
static const char* ARCHIVE_STREAM_HIDDEN_CLASS = "ArchiveStreamExpanderHiddenWindow";
static const ULONG_PTR ARCHIVE_STREAM_COPYDATA_EXPAND_ZIP = 0x41535A50; // "ASZP"
static const size_t MIN_BUFFER_SIZE_MB = 10;
static const size_t DEFAULT_BUFFER_SIZE_MB = 50;
static const size_t MAX_BUFFER_SIZE_MB = 200;
static const size_t MIN_CACHE_SIZE_MB = 0;        // 0 = no size cleanup
static const size_t DEFAULT_CACHE_SIZE_MB = 2048; // 2 GiB
static const size_t MAX_CACHE_SIZE_MB = 20480;    // 20 GiB
static const unsigned int MIN_CACHE_AGE_DAYS = 1;
static const unsigned int DEFAULT_CACHE_AGE_DAYS = 30;
static const unsigned int MAX_CACHE_AGE_DAYS = 99;
static const size_t MAX_METADATA_SCAN_MB = 250;

enum ArchivePlaybackMode
{
    ARCHIVE_PLAYBACK_MEMORY = 0,
    ARCHIVE_PLAYBACK_STREAM = 1,
    ARCHIVE_PLAYBACK_CACHE_FILE = 2
};

enum ArchiveStreamCapability
{
    ARCHIVE_STREAM_CAPABILITY_NONE = 0,
    ARCHIVE_STREAM_CAPABILITY_SUPPORTED = 1,
    ARCHIVE_STREAM_CAPABILITY_SOLID_7Z = 2,
    ARCHIVE_STREAM_CAPABILITY_7Z_READER_NOT_IMPLEMENTED = 3
};

enum AudioFormat
{
    AUDIO_FORMAT_UNKNOWN = 0,
    AUDIO_FORMAT_MP3,
    AUDIO_FORMAT_WAV,
    AUDIO_FORMAT_FLAC,
    AUDIO_FORMAT_OGG,
    AUDIO_FORMAT_M4A,
    AUDIO_FORMAT_AAC,
    AUDIO_FORMAT_WMA
};

struct ArchiveAudioEntry
{
    mz_uint index = 0;
    mz_uint64 uncompSize = 0;
    std::string name;
};

struct ArchiveStreamContext
{
    // Source identity, needed for ZIP stream restart/seek.
    char archivePath[MAX_PATH * 4] = { 0 };
    mz_uint index = 0;

    /*
    Libarchive ZIP stream path.

    ZIP streaming is fully libarchive-backed. The opaque stream handle is owned
    by archive_zip_libarchive.cpp and is closed by ArchiveCloseEntryStream().
    */
    void* libarchiveZipStream = nullptr;

    bool libarchiveZipOpen = false;

    // Backend streaming path, currently intended for 7z streaming.
    ArchiveBackendStream backendStream;
    bool backendOpen = false;

    mz_uint64 position = 0;
};

bool ArchiveGetPluginDataRoot(char* outPath, size_t outPathSize);
bool ArchiveBuildPluginDataFilePath(const char* fileName, char* outPath, size_t outPathSize);
bool ArchiveBuildPluginDataSubdirPath(const char* subdirName, char* outPath, size_t outPathSize);

static inline AudioFormat GetAudioFormatFromPath(const char* path)
{
    if (!path)
        return AUDIO_FORMAT_UNKNOWN;

    const char* ext = strrchr(path, '.');

    if (!ext)
        return AUDIO_FORMAT_UNKNOWN;

    if (_stricmp(ext, ".mp3") == 0)
        return AUDIO_FORMAT_MP3;

    if (_stricmp(ext, ".wav") == 0)
        return AUDIO_FORMAT_WAV;

    if (_stricmp(ext, ".flac") == 0)
        return AUDIO_FORMAT_FLAC;

    if (_stricmp(ext, ".ogg") == 0)
        return AUDIO_FORMAT_OGG;

    if (_stricmp(ext, ".m4a") == 0)
        return AUDIO_FORMAT_M4A;

    if (_stricmp(ext, ".aac") == 0)
        return AUDIO_FORMAT_AAC;

    if (_stricmp(ext, ".wma") == 0)
        return AUDIO_FORMAT_WMA;

    return AUDIO_FORMAT_UNKNOWN;
}

static inline bool IsPlayableAudioExtension(const char* ext)
{
    if (!ext)
        return false;

    return _stricmp(ext, ".mp3") == 0 ||
        _stricmp(ext, ".wav") == 0 ||
        _stricmp(ext, ".flac") == 0 ||
        _stricmp(ext, ".ogg") == 0 ||
        _stricmp(ext, ".m4a") == 0 ||
        _stricmp(ext, ".aac") == 0 ||
        _stricmp(ext, ".wma") == 0;
}

static inline bool IsVirtualArchivePath(const char* file)
{
    if (!file)
        return false;

    if (strncmp(file, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) == 0)
        return true;

    return strstr(file, ".archive_stream|") != NULL;
}

static inline void CopyBaseName(char* out, size_t outSize, const char* path)
{
    if (!out || outSize == 0)
        return;

    out[0] = '\0';

    if (!path || !path[0])
    {
        strcpy_s(out, outSize, "Archive Stream");
        return;
    }

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* base = slash1 > slash2 ? slash1 : slash2;

    if (base)
        base++;
    else
        base = path;

    strcpy_s(out, outSize, base);

    char* ext = strrchr(out, '.');

    if (ext)
        *ext = '\0';
}

bool ParseVirtualArchivePath(const char* file, char* outZipPath, size_t outZipPathSize, mz_uint* outIndex, char* outInternalPath, size_t outInternalPathSize);