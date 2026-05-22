#include <string>
#include <vector>
#include <map>
#include <mutex>

#include "archive_metadata.h"
#include "archive_backend.h"
#include "archive_reader.h"
#include "archive_common.h"
#include "debug_log.h"
#include "archive_cache.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "stb_vorbis.c"

enum MetadataAudioFormat
{
    METADATA_AUDIO_FORMAT_UNKNOWN = 0,
    METADATA_AUDIO_FORMAT_MP3,
    METADATA_AUDIO_FORMAT_WAV,
    METADATA_AUDIO_FORMAT_FLAC,
    METADATA_AUDIO_FORMAT_OGG,
    METADATA_AUDIO_FORMAT_WMA,
    METADATA_AUDIO_FORMAT_M4A,
    METADATA_AUDIO_FORMAT_AAC
};

static bool GetAudioDurationMsFromMemory(MetadataAudioFormat format, const unsigned char* data, size_t dataSize, int* outLengthMs);
static bool GetAudioDurationMsFromStream(MetadataAudioFormat format, const char* archivePath, uint32_t index, const char* filename, uint64_t uncompSize, int* outLengthMs);

struct MetadataFingerprintKey
{
    uint64_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    std::string extension;

    bool operator<(const MetadataFingerprintKey& other) const
    {
        if (size != other.size)
        {
            return size < other.size;
        }

        if (hasCrc32 != other.hasCrc32)
        {
            return hasCrc32 < other.hasCrc32;
        }

        if (crc32 != other.crc32)
        {
            return crc32 < other.crc32;
        }

        return extension < other.extension;
    }
};

struct MetadataFingerprintValue
{
    int lengthMs = -1000;
};

static bool g_metadataFingerprintCacheLoaded = false;
static bool g_metadataFingerprintCacheDirty = false;

static std::mutex g_metadataFingerprintCacheMutex;
static std::map<MetadataFingerprintKey, MetadataFingerprintValue> g_metadataFingerprintCache;


static bool MetadataGetCacheFilePath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        // should add loggin here
        return false;
    }

    outPath[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("metadata_cache.tsv", outPath, outPathSize))
    {
        DebugLogWrite("WARN", "METADATA_CACHE", "PATH_FAIL", "reason=plugin_data_file_path_failed");
        return false;
    }

    return true;
}

static void MetadataFingerprintCacheLoad()
{
    std::lock_guard<std::mutex> lock(g_metadataFingerprintCacheMutex);

    if (g_metadataFingerprintCacheLoaded)
    {
        return;
    }

    g_metadataFingerprintCacheLoaded = true;

    char path[MAX_PATH * 4];

    if (!MetadataGetCacheFilePath(path, sizeof(path)))
    {
        DebugLogWrite("WARN", "METADATA_CACHE", "LOAD_SKIP", "reason=cache_path_failed");
        return;
    }

    std::ifstream file(path);

    if (!file.is_open())
    {
        DebugLogWrite("INFO", "METADATA_CACHE", "LOAD_SKIP", "path=%s | reason=file_not_found", path);
        return;
    }

    std::string line;
    int loaded = 0;

    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string sizeText;
        std::string crcText;
        std::string extension;
        std::string lengthText;

        if (!std::getline(ss, sizeText, '\t') || !std::getline(ss, crcText, '\t') || !std::getline(ss, extension, '\t') || !std::getline(ss, lengthText, '\t'))
        {
            continue;
        }

        MetadataFingerprintKey key;
        MetadataFingerprintValue value;

        key.size = _strtoui64(sizeText.c_str(), nullptr, 10);
        key.hasCrc32 = _stricmp(crcText.c_str(), "nocrc") != 0;
        key.crc32 = key.hasCrc32 ? (uint32_t)strtoul(crcText.c_str(), nullptr, 16) : 0;
        key.extension = extension;
        value.lengthMs = atoi(lengthText.c_str());

        if (key.size > 0 && !key.extension.empty() && value.lengthMs > 0)
        {
            g_metadataFingerprintCache[key] = value;
            ++loaded;
        }
    }

    DebugLogWrite("INFO", "METADATA_CACHE", "LOAD_DONE", "path=%s | entries=%d", path, loaded);
}

static void MetadataFingerprintCacheSave()
{
    std::lock_guard<std::mutex> lock(g_metadataFingerprintCacheMutex);

    if (!g_metadataFingerprintCacheDirty)
    {
        return;
    }

    char path[MAX_PATH * 4];

    if (!MetadataGetCacheFilePath(path, sizeof(path)))
    {
        DebugLogWrite("WARN", "METADATA_CACHE", "SAVE_SKIP", "reason=cache_path_failed");
        return;
    }

    std::ofstream file(path, std::ios::trunc);

    if (!file.is_open())
    {
        DebugLogWrite("WARN", "METADATA_CACHE", "SAVE_FAIL", "path=%s | reason=open_failed", path);
        return;
    }

    int saved = 0;

    for (const auto& item : g_metadataFingerprintCache)
    {
        file << item.first.size << "\t";

        if (item.first.hasCrc32)
        {
            file << std::hex << item.first.crc32 << std::dec;
        }
        else
        {
            file << "nocrc";
        }

        file << "\t" << item.first.extension << "\t" << item.second.lengthMs << "\n";
        ++saved;
    }

    g_metadataFingerprintCacheDirty = false;
    DebugLogWrite("INFO", "METADATA_CACHE", "SAVE_DONE", "path=%s | entries=%d", path, saved);
}

static std::string MetadataGetLowerExtension(const char* filename)
{
    if (!filename || !filename[0])
    {
        return std::string();
    }

    const char* dot = strrchr(filename, '.');

    if (!dot || !dot[0])
    {
        return std::string();
    }

    std::string ext = dot;

    for (size_t i = 0; i < ext.size(); ++i)
    {
        if (ext[i] >= 'A' && ext[i] <= 'Z')
        {
            ext[i] = (char)(ext[i] - 'A' + 'a');
        }
    }

    return ext;
}

static bool MetadataBuildFingerprintKey(uint64_t size, uint32_t crc32, bool hasCrc32, const char* filename, MetadataFingerprintKey* outKey)
{
    if (!outKey || size == 0 || !filename || !filename[0])
    {
        return false;
    }

    std::string ext = MetadataGetLowerExtension(filename);

    if (ext.empty())
    {
        return false;
    }

    outKey->size = size;
    outKey->hasCrc32 = hasCrc32;
    outKey->crc32 = hasCrc32 ? crc32 : 0;
    outKey->extension = ext;
    return true;
}

static const char* MetadataFingerprintCrcText(const MetadataFingerprintKey& key, char* buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
    {
        return "";
    }

    if (!key.hasCrc32)
    {
        strcpy_s(buffer, bufferSize, "nocrc");
        return buffer;
    }

    sprintf_s(buffer, bufferSize, "%08X", (unsigned int)key.crc32);
    return buffer;
}

static bool MetadataFingerprintCacheGet(const MetadataFingerprintKey& key, int* outLengthMs)
{
    if (!outLengthMs)
    {
        // should ahve log added here
        return false;
    }

    MetadataFingerprintCacheLoad();

    std::lock_guard<std::mutex> lock(g_metadataFingerprintCacheMutex);
    auto it = g_metadataFingerprintCache.find(key);

    if (it == g_metadataFingerprintCache.end())
    {
        // should ahve log added here
        return false;
    }

    *outLengthMs = it->second.lengthMs;

    return true;
}

static void MetadataFingerprintCacheStore(const MetadataFingerprintKey& key, int lengthMs)
{
    if (lengthMs <= 0)
    {
        // should ahve log added here
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_metadataFingerprintCacheMutex);

        MetadataFingerprintValue value;
        value.lengthMs = lengthMs;

        g_metadataFingerprintCache[key] = value;
        g_metadataFingerprintCacheDirty = true;
    }

    MetadataFingerprintCacheSave();
}

void ArchiveMetadataClearFingerprintCache()
{
    std::lock_guard<std::mutex> lock(g_metadataFingerprintCacheMutex);
    g_metadataFingerprintCache.clear();
}

static bool MetadataEndsWithIgnoreCase(const char* text, const char* suffix)
{
    if (!text || !suffix) 
        return false;

    size_t textLen = strlen(text);
    size_t suffixLen = strlen(suffix);
    if (suffixLen == 0 || suffixLen > textLen) return false;
    const char* a = text + textLen - suffixLen;
    const char* b = suffix;

    while (*a && *b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

        if (ca != cb) 
            return false;

        ++a;
        ++b;
    }

    return true;
}

static MetadataAudioFormat GetMetadataAudioFormatFromPath(const char* filename)
{
    if (!filename || !filename[0])
    {
        return METADATA_AUDIO_FORMAT_UNKNOWN;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".mp3"))
    {
        return METADATA_AUDIO_FORMAT_MP3;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".wav"))
    {
        return METADATA_AUDIO_FORMAT_WAV;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".flac"))
    {
        return METADATA_AUDIO_FORMAT_FLAC;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".ogg"))
    {
        return METADATA_AUDIO_FORMAT_OGG;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".wma"))
    {
        return METADATA_AUDIO_FORMAT_WMA;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".m4a"))
    {
        return METADATA_AUDIO_FORMAT_M4A;
    }

    if (MetadataEndsWithIgnoreCase(filename, ".aac"))
    {
        return METADATA_AUDIO_FORMAT_AAC;
    }

    return METADATA_AUDIO_FORMAT_UNKNOWN;
}

static bool MetadataFormatUsesMediaFoundation(MetadataAudioFormat format)
{
    return format == METADATA_AUDIO_FORMAT_WMA || format == METADATA_AUDIO_FORMAT_M4A || format == METADATA_AUDIO_FORMAT_AAC;
}

static bool MetadataArchiveEntryIsSolidBackend(const char* archivePath, uint32_t index)
{
    if (!archivePath || !archivePath[0])
    {
        return false;
    }

    wchar_t wideArchivePath[MAX_PATH * 4];
    wideArchivePath[0] = L'\0';

    if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
    {
        DebugLogWrite("WARN", "METADATA", "SOLID_CHECK_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
        return false;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveListEntries(wideArchivePath, entries, error))
    {
        DebugLogWrite("WARN", "METADATA", "SOLID_CHECK_FAIL", "archive=%s | index=%u | error=%s", archivePath, (unsigned int)index, error.c_str());
        return false;
    }

    for (const ArchiveEntry& entry : entries)
    {
        if (entry.index == index)
        {
            return entry.isSolid;
        }
    }

    DebugLogWrite("WARN", "METADATA", "SOLID_CHECK_FAIL", "archive=%s | index=%u | reason=entry_not_found", archivePath, (unsigned int)index);
    return false;
}

static AudioFormat MetadataFormatToAudioFormat(MetadataAudioFormat format)
{
    if (format == METADATA_AUDIO_FORMAT_WMA)
    {
        return AUDIO_FORMAT_WMA;
    }

    if (format == METADATA_AUDIO_FORMAT_M4A)
    {
        return AUDIO_FORMAT_M4A;
    }

    if (format == METADATA_AUDIO_FORMAT_AAC)
    {
        return AUDIO_FORMAT_AAC;
    }

    return AUDIO_FORMAT_UNKNOWN;
}

static bool GetAudioDurationMsFromMemory(MetadataAudioFormat format, const unsigned char* data, size_t dataSize, int* outLengthMs)
{
    if (outLengthMs)
        *outLengthMs = -1000;

    if (!data || dataSize == 0 || !outLengthMs)
    {
        // should ahve log added here
        return false;
    }

    bool result = false;

    if (format == METADATA_AUDIO_FORMAT_MP3)
    {
        drmp3 mp3;

        if (drmp3_init_memory(&mp3, data, dataSize, NULL))
        {
            drmp3_uint64 totalFrames = drmp3_get_pcm_frame_count(&mp3);

            if (mp3.sampleRate > 0 && totalFrames > 0)
            {
                double seconds = (double)totalFrames / (double)mp3.sampleRate;
                *outLengthMs = (int)(seconds * 1000.0);
                result = true;
            }

            drmp3_uninit(&mp3);
        }
    }
    else if (format == METADATA_AUDIO_FORMAT_WAV)
    {
        drwav wav;

        if (drwav_init_memory(&wav, data, dataSize, NULL))
        {
            if (wav.sampleRate > 0 && wav.totalPCMFrameCount > 0)
            {
                double seconds = (double)wav.totalPCMFrameCount / (double)wav.sampleRate;
                *outLengthMs = (int)(seconds * 1000.0);
                result = true;
            }

            drwav_uninit(&wav);
        }
    }
    else if (format == METADATA_AUDIO_FORMAT_FLAC)
    {
        drflac* flac = drflac_open_memory(data, dataSize, NULL);

        if (flac)
        {
            if (flac->sampleRate > 0 && flac->totalPCMFrameCount > 0)
            {
                double seconds = (double)flac->totalPCMFrameCount / (double)flac->sampleRate;
                *outLengthMs = (int)(seconds * 1000.0);
                result = true;
            }

            drflac_close(flac);
        }
    }
    else if (format == METADATA_AUDIO_FORMAT_OGG)
    {
        int error = 0;
        stb_vorbis* ogg = stb_vorbis_open_memory(data, (int)dataSize, &error, NULL);

        if (ogg)
        {
            stb_vorbis_info info = stb_vorbis_get_info(ogg);
            unsigned int sampleRate = info.sample_rate;
            unsigned int totalSamples = stb_vorbis_stream_length_in_samples(ogg);

            if (sampleRate > 0 && totalSamples > 0)
            {
                double seconds = (double)totalSamples / (double)sampleRate;
                *outLengthMs = (int)(seconds * 1000.0);
                result = true;
            }

            stb_vorbis_close(ogg);
        }
    }

    return result;
}

static bool GetMediaFoundationDurationMsFromArchive(const char* archivePath, uint32_t index, uint64_t uncompSize, MetadataAudioFormat format, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!archivePath || !archivePath[0] || !outLengthMs || uncompSize == 0)
    {
        // should ahve log added here
        return false;
    }

    AudioFormat audioFormat = MetadataFormatToAudioFormat(format);

    if (audioFormat == AUDIO_FORMAT_UNKNOWN)
    {
        // should ahve log added here
        return false;
    }

    MfAudioDecoder* decoder = nullptr;

    if (!MfOpenArchiveDecoder(archivePath, (mz_uint)index, (mz_uint64)uncompSize, audioFormat, NULL, &decoder))
    {
        // should ahve log added here
        return false;
    }

    int lengthMs = MfGetLengthMs(decoder);
    MfCloseDecoder(decoder);

    if (lengthMs <= 0)
    {
        // should ahve log added here
        return false;
    }

    *outLengthMs = lengthMs;
    return true;
}

static bool GetMediaFoundationDurationMsFromCache(const char* cachePath, MetadataAudioFormat format, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!cachePath || !cachePath[0] || !outLengthMs)
    {
        // should ahve log added here
        return false;
    }

    AudioFormat audioFormat = MetadataFormatToAudioFormat(format);

    if (audioFormat == AUDIO_FORMAT_UNKNOWN)
    {
        // should ahve log added here
        return false;
    }

    MfAudioDecoder* decoder = nullptr;

    if (!MfOpenCacheFileDecoder(cachePath, audioFormat, NULL, &decoder))
    {
        // should ahve log added here
        return false;
    }

    int lengthMs = MfGetLengthMs(decoder);
    MfCloseDecoder(decoder);

    if (lengthMs <= 0)
    {
        // should ahve log added here
        return false;
    }

    *outLengthMs = lengthMs;

    return true;
}

static bool GetAudioDurationMsFromStream(MetadataAudioFormat format, const char* archivePath, uint32_t index, const char* filename, uint64_t uncompSize, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!archivePath || !archivePath[0] || !filename || !filename[0] || !outLengthMs || uncompSize == 0)
    {
        return false;
    }

    if (MetadataFormatUsesMediaFoundation(format))
    {
        if (GetMediaFoundationDurationMsFromArchive(archivePath, index, uncompSize, format, outLengthMs))
        {
            DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE", "archive=%s | index=%u | entry=%s | route=STREAM_MF | length_ms=%d", archivePath, (unsigned int)index, filename, *outLengthMs);
            return true;
        }

        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE_FAIL", "archive=%s | index=%u | entry=%s | route=STREAM_MF | reason=mf_duration_failed", archivePath, (unsigned int)index, filename);
        return false;
    }

    ArchiveStreamContext ctx{};
    if (!ArchiveOpenEntryStream(&ctx, archivePath, (mz_uint)index))
    {
        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE_FAIL", "archive=%s | index=%u | entry=%s | route=STREAM | reason=open_stream_failed", archivePath, (unsigned int)index, filename);
        return false;
    }

    std::vector<unsigned char> tempBuffer;
    tempBuffer.resize((size_t)uncompSize);

    size_t totalRead = 0;

    while (totalRead < tempBuffer.size())
    {
        size_t remaining = tempBuffer.size() - totalRead;
        size_t chunk = remaining > (256 * 1024) ? (256 * 1024) : remaining;
        size_t got = ArchiveReadEntryStream(&ctx, tempBuffer.data() + totalRead, chunk);

        if (got == 0)
            break;

        totalRead += got;
    }

    ArchiveCloseEntryStream(&ctx);

    if (totalRead != tempBuffer.size())
    {
        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE_FAIL", "archive=%s | index=%u | entry=%s | route=STREAM | read=%zu | expected=%llu | reason=short_read", archivePath, (unsigned int)index, filename, totalRead, (unsigned long long)uncompSize);
        return false;
    }

    bool result = GetAudioDurationMsFromMemory(format, tempBuffer.data(), tempBuffer.size(), outLengthMs);

    if (result)
        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE", "archive=%s | index=%u | entry=%s | route=STREAM | length_ms=%d", archivePath, (unsigned int)index, filename, *outLengthMs);
    else
        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE_FAIL", "archive=%s | index=%u | entry=%s | route=STREAM | reason=decoder_failed", archivePath, (unsigned int)index, filename);

    return result;
}

static bool ReadMetadataFileToMemory(const char* path, std::vector<unsigned char>& outData)
{
    outData.clear();

    if (!path || !path[0])
    {
        // should ahve log added here
        return false;
    }

    FILE* f = nullptr;

    if (fopen_s(&f, path, "rb") != 0 || !f)
    {
        // should ahve log added here
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        // should ahve log added here
        return false;
    }

    long size = ftell(f);

    if (size <= 0)
    {
        fclose(f);
        // should ahve log added here
        return false;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        // should ahve log added here
        return false;
    }

    outData.resize((size_t)size);
    size_t got = fread(outData.data(), 1, outData.size(), f);
    fclose(f);

    if (got != outData.size())
    {
        outData.clear();
        // should ahve log added here
        return false;
    }

    return true;
}



bool GetAudioDurationMsFromArchiveEntryCacheOnly(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!archivePath || !archivePath[0] || !outLengthMs || uncompSize == 0 || !filename || !filename[0])
    {
        return false;
    }

    char entryName[1024];
    uint64_t actualSize = uncompSize;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;

    entryName[0] = '\0';

    if (ArchiveGetEntryInfoEx(archivePath, index, entryName, sizeof(entryName), &actualSize, &crc32, &hasCrc32))
    {
        if (entryName[0])
        {
            filename = entryName;
        }
    }

    MetadataFingerprintKey fingerprintKey;

    if (!MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
    {
        DebugLogWrite("INFO", "METADATA_CACHE", "CACHE_ONLY_SKIP", "archive=%s | index=%u | entry=%s | size=%llu | has_crc=%d | reason=missing_or_invalid_fingerprint", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, hasCrc32 ? 1 : 0);
        return false;
    }

    int cachedLengthMs = -1000;

    if (!MetadataFingerprintCacheGet(fingerprintKey, &cachedLengthMs))
    {
        char crcText[32];
        DebugLogWrite("INFO", "METADATA_CACHE", "CACHE_ONLY_MISS", "archive=%s | index=%u | entry=%s | size=%llu | crc=%s | ext=%s", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, MetadataFingerprintCrcText(fingerprintKey, crcText, sizeof(crcText)), fingerprintKey.extension.c_str());
        return false;
    }

    *outLengthMs = cachedLengthMs;

    char crcText[32];
    DebugLogWrite("INFO", "METADATA_CACHE", "CACHE_ONLY_HIT", "archive=%s | index=%u | entry=%s | size=%llu | crc=%s | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, MetadataFingerprintCrcText(fingerprintKey, crcText, sizeof(crcText)), fingerprintKey.extension.c_str(), cachedLengthMs);
    return true;
}

bool GetAudioDurationMsFromArchiveEntry(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!archivePath || !archivePath[0] || !outLengthMs || uncompSize == 0 || !filename || !filename[0])
    {
        // should ahve log added here
        return false;
    }

    MetadataAudioFormat format = GetMetadataAudioFormatFromPath(filename);

    if (format == METADATA_AUDIO_FORMAT_UNKNOWN)
    {
        // should ahve log added here
        return false;
    }

    char entryName[1024];
    uint64_t actualSize = uncompSize;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    entryName[0] = '\0';

    if (ArchiveGetEntryInfoEx(archivePath, index, entryName, sizeof(entryName), &actualSize, &crc32, &hasCrc32))
    {
        if (entryName[0])
        {
            filename = entryName;
        }
    }

    MetadataFingerprintKey fingerprintKey;

    if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
    {
        int cachedLengthMs = -1000;

        if (MetadataFingerprintCacheGet(fingerprintKey, &cachedLengthMs))
        {
            *outLengthMs = cachedLengthMs;
            DebugLogWrite("INFO", "METADATA_CACHE", "FINGERPRINT_HIT", "archive=%s | index=%u | entry=%s | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), cachedLengthMs);
            return true;
        }

        DebugLogWrite("INFO", "METADATA_CACHE", "FINGERPRINT_MISS", "archive=%s | index=%u | entry=%s | size=%llu | crc=%08X | ext=%s", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str());
    }
    else
    {
        DebugLogWrite("INFO", "METADATA_CACHE", "FINGERPRINT_SKIP", "archive=%s | index=%u | entry=%s | size=%llu | has_crc=%d | reason=missing_or_invalid_fingerprint", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, hasCrc32 ? 1 : 0);
    }

    char existingCachePath[MAX_PATH * 4];
    existingCachePath[0] = '\0';

    if (ArchiveFindExistingCacheFile(archivePath, index, filename, existingCachePath, sizeof(existingCachePath)))    
    {
        if (MetadataFormatUsesMediaFoundation(format))
        {
            if (GetMediaFoundationDurationMsFromCache(existingCachePath, format, outLengthMs))
            {
                DebugLogWrite("INFO", "METADATA", "DURATION_OK", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE_MF | cache=%s | length_ms=%d", archivePath, (unsigned int)index, filename, existingCachePath, *outLengthMs);

                if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
                {
                    MetadataFingerprintCacheStore(fingerprintKey, *outLengthMs);
                    DebugLogWrite("INFO", "METADATA_CACHE", "STORE", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE_MF | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), *outLengthMs);
                }

                return true;
            }

            DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE_MF | cache=%s | reason=mf_duration_failed", archivePath, (unsigned int)index, filename, existingCachePath);
        }
        else
        {
            std::vector<unsigned char> cacheBuffer;

            if (ReadMetadataFileToMemory(existingCachePath, cacheBuffer))
            {
                if (GetAudioDurationMsFromMemory(format, cacheBuffer.data(), cacheBuffer.size(), outLengthMs))
                {
                    DebugLogWrite("INFO", "METADATA", "DURATION_OK", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE | cache=%s | length_ms=%d", archivePath, (unsigned int)index, filename, existingCachePath, *outLengthMs);

                    if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
                    {
                        MetadataFingerprintCacheStore(fingerprintKey, *outLengthMs);
                        DebugLogWrite("INFO", "METADATA_CACHE", "STORE", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), *outLengthMs);
                    }

                    return true;
                }

                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE | cache=%s | reason=decoder_failed", archivePath, (unsigned int)index, filename, existingCachePath);
            }
            else
            {
                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=EXISTING_CACHE | cache=%s | reason=cache_read_failed", archivePath, (unsigned int)index, filename, existingCachePath);
            }
        }
    }

    const bool metadataEntryIsSolid = MetadataArchiveEntryIsSolidBackend(archivePath, index);

    if (metadataEntryIsSolid && MetadataFormatUsesMediaFoundation(format))
    {
        DebugLogWrite("INFO", "METADATA", "DURATION_DEFER", "archive=%s | index=%u | entry=%s | route=SOLID_MF_CACHE_MISS | reason=defer_until_playback_or_background_worker", archivePath, (unsigned int)index, filename);
        return false;
    }

    if (metadataEntryIsSolid)
    {
        DebugLogWrite("INFO", "METADATA", "DURATION_ROUTE_SKIP", "archive=%s | index=%u | entry=%s | route=STREAM | reason=solid_entry_stream_not_attempted", archivePath, (unsigned int)index, filename);
    }
    else if (GetAudioDurationMsFromStream(format, archivePath, index, filename, actualSize, outLengthMs))
    {
        MetadataFingerprintKey fingerprintKey;

        if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
        {
            MetadataFingerprintCacheStore(fingerprintKey, *outLengthMs);
            DebugLogWrite("INFO", "METADATA_CACHE", "STORE", "archive=%s | index=%u | entry=%s | route=STREAM | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), *outLengthMs);
        }

        return true;
    }

    const uint64_t maxMetadataBytes = (uint64_t)MAX_METADATA_SCAN_MB * 1024ULL * 1024ULL;

    if (actualSize <= maxMetadataBytes)
    {
        wchar_t wideArchivePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
        {
            DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=BUFFER | reason=path_convert_failed", archivePath, (unsigned int)index, filename);
            return false;
        }

        std::vector<unsigned char> tempBuffer;
        std::string extractError;

        if (!ArchiveExtractEntryToMemory(wideArchivePath, index, tempBuffer, extractError))
        {
            DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=BUFFER | bytes=%llu | reason=extract_to_memory_failed | error=%s", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, extractError.c_str());
            return false;
        }

        if (tempBuffer.empty())
        {
            DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=BUFFER | reason=extract_returned_empty_buffer", archivePath, (unsigned int)index, filename);
            return false;
        }

        if (GetAudioDurationMsFromMemory(format, tempBuffer.data(), tempBuffer.size(), outLengthMs))
        {
            DebugLogWrite("INFO", "METADATA", "DURATION_OK", "archive=%s | index=%u | entry=%s | route=BUFFER | length_ms=%d", archivePath, (unsigned int)index, filename, *outLengthMs);

            if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
            {
                MetadataFingerprintCacheStore(fingerprintKey, *outLengthMs);
                DebugLogWrite("INFO", "METADATA_CACHE", "STORE", "archive=%s | index=%u | entry=%s | route=BUFFER | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), *outLengthMs);
            }

            return true;
        }

        DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=BUFFER | reason=decoder_failed", archivePath, (unsigned int)index, filename);
        return false;
    }

    DebugLogWrite("INFO", "METADATA", "DURATION_SKIP", "archive=%s | index=%u | entry=%s | bytes=%llu | max_bytes=%llu | route=CACHE | reason=too_large_for_metadata_buffer", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned long long)maxMetadataBytes);
    return false;
}

bool GetAudioDurationMsFromArchiveEntryBackground(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs)
{
    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!archivePath || !archivePath[0] || !outLengthMs || uncompSize == 0 || !filename || !filename[0])
    {
        return false;
    }

    if (GetAudioDurationMsFromArchiveEntry(archivePath, index, uncompSize, filename, outLengthMs))
    {
        return true;
    }

    MetadataAudioFormat format = GetMetadataAudioFormatFromPath(filename);

    if (!MetadataFormatUsesMediaFoundation(format))
    {
        return false;
    }

    char entryName[1024];
    uint64_t actualSize = uncompSize;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;

    entryName[0] = '\0';

    if (ArchiveGetEntryInfoEx(archivePath, index, entryName, sizeof(entryName), &actualSize, &crc32, &hasCrc32))
    {
        if (entryName[0])
        {
            filename = entryName;
        }
    }

    char cachePath[MAX_PATH * 4];
    cachePath[0] = '\0';

    if (!ArchiveExtractEntryToCacheFile(archivePath, index, filename, cachePath, sizeof(cachePath)))
    {
        DebugLogWrite("WARN", "METADATA", "BACKGROUND_CACHE_FAIL", "archive=%s | index=%u | entry=%s | route=BACKGROUND_MF_CACHE | reason=extract_to_cache_failed", archivePath, (unsigned int)index, filename);
        return false;
    }

    if (!GetMediaFoundationDurationMsFromCache(cachePath, format, outLengthMs))
    {
        DebugLogWrite("WARN", "METADATA", "BACKGROUND_DURATION_FAIL", "archive=%s | index=%u | entry=%s | route=BACKGROUND_MF_CACHE | cache=%s | reason=mf_duration_failed", archivePath, (unsigned int)index, filename, cachePath);
        return false;
    }

    MetadataFingerprintKey fingerprintKey;

    if (MetadataBuildFingerprintKey(actualSize, crc32, hasCrc32, filename, &fingerprintKey))
    {
        MetadataFingerprintCacheStore(fingerprintKey, *outLengthMs);
        DebugLogWrite("INFO", "METADATA_CACHE", "STORE", "archive=%s | index=%u | entry=%s | route=BACKGROUND_MF_CACHE | size=%llu | crc=%08X | ext=%s | length_ms=%d", archivePath, (unsigned int)index, filename, (unsigned long long)actualSize, (unsigned int)crc32, fingerprintKey.extension.c_str(), *outLengthMs);
    }

    DebugLogWrite("INFO", "METADATA", "DURATION_OK", "archive=%s | index=%u | entry=%s | route=BACKGROUND_MF_CACHE | cache=%s | length_ms=%d", archivePath, (unsigned int)index, filename, cachePath, *outLengthMs);
    return true;
}

bool GetVirtualArchiveEntryInfoCacheOnly(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs)
{
    if (outTitle && outTitleSize > 0)
    {
        outTitle[0] = '\0';
    }

    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!file || !file[0])
    {
        return false;
    }

    char archivePath[MAX_PATH * 4];
    char internalPath[1024];
    mz_uint index = 0;

    archivePath[0] = '\0';
    internalPath[0] = '\0';

    if (!ParseVirtualArchivePath(file, archivePath, sizeof(archivePath), &index, internalPath, sizeof(internalPath)))
    {
        return false;
    }

    if (outTitle && outTitleSize > 0 && internalPath[0])
    {
        CopyBaseName(outTitle, outTitleSize, internalPath);
    }

    char entryName[1024];
    uint64_t uncompSize = 0;

    entryName[0] = '\0';

    if (!ArchiveGetEntryInfo(archivePath, (uint32_t)index, entryName, sizeof(entryName), &uncompSize))
    {
        return internalPath[0] != '\0';
    }

    if (outTitle && outTitleSize > 0 && !internalPath[0])
    {
        CopyBaseName(outTitle, outTitleSize, entryName);
    }

    if (outLengthMs)
    {
        GetAudioDurationMsFromArchiveEntryCacheOnly(archivePath, (uint32_t)index, uncompSize, entryName[0] ? entryName : internalPath, outLengthMs);
    }

    return true;
}

bool GetVirtualArchiveEntryInfoBackground(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs)
{
    if (outTitle && outTitleSize > 0)
    {
        outTitle[0] = '\0';
    }

    if (outLengthMs)
    {
        *outLengthMs = -1000;
    }

    if (!file || !file[0])
    {
        return false;
    }

    char archivePath[MAX_PATH * 4];
    char internalPath[1024];
    mz_uint index = 0;

    archivePath[0] = '\0';
    internalPath[0] = '\0';

    if (!ParseVirtualArchivePath(file, archivePath, sizeof(archivePath), &index, internalPath, sizeof(internalPath)))
    {
        return false;
    }

    if (outTitle && outTitleSize > 0 && internalPath[0])
    {
        CopyBaseName(outTitle, outTitleSize, internalPath);
    }

    char entryName[1024];
    uint64_t uncompSize = 0;

    entryName[0] = '\0';

    if (!ArchiveGetEntryInfo(archivePath, (uint32_t)index, entryName, sizeof(entryName), &uncompSize))
    {
        return internalPath[0] != '\0';
    }

    if (outTitle && outTitleSize > 0 && !internalPath[0])
    {
        CopyBaseName(outTitle, outTitleSize, entryName);
    }

    if (outLengthMs)
    {
        GetAudioDurationMsFromArchiveEntryBackground(archivePath, (uint32_t)index, uncompSize, entryName[0] ? entryName : internalPath, outLengthMs);
    }

    return true;
}

bool GetVirtualArchiveEntryInfo(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs)
{
    if (outTitle && outTitleSize > 0)
        outTitle[0] = '\0';

    if (outLengthMs)
        *outLengthMs = -1000;

    if (!file || !file[0])
    {
        // should ahve log added here
        return false;
    }

    char archivePath[MAX_PATH * 4];
    char internalPath[1024];
    mz_uint index = 0;

    archivePath[0] = '\0';
    internalPath[0] = '\0';

    if (!ParseVirtualArchivePath(file, archivePath, sizeof(archivePath), &index, internalPath, sizeof(internalPath)))
    {
        // should ahve log added here
        return false;
    }

    if (outTitle && outTitleSize > 0 && internalPath[0])
        CopyBaseName(outTitle, outTitleSize, internalPath);

    char entryName[1024];
    uint64_t uncompSize = 0;

    entryName[0] = '\0';

    if (!ArchiveGetEntryInfo(archivePath, (uint32_t)index, entryName, sizeof(entryName), &uncompSize))
    {
        if (!internalPath[0])
            DebugLogWrite("WARN", "VIRTUAL", "ENTRY_INFO_FAIL", "file=%s | archive=%s | index=%u | reason=archive_entry_info_failed_no_internal_name", file, archivePath, (unsigned int)index);

        return internalPath[0] != '\0';
    }

    if (outTitle && outTitleSize > 0 && !internalPath[0])
        CopyBaseName(outTitle, outTitleSize, entryName);

    if (outLengthMs)
        GetAudioDurationMsFromArchiveEntry(archivePath, (uint32_t)index, uncompSize, entryName[0] ? entryName : internalPath, outLengthMs);

    return true;
}

bool ArchiveFormatVirtualTitle(const char* virtualPath, const char* baseTitle, char* outTitle, size_t outTitleSize)
{
    if (!outTitle || outTitleSize == 0)
    {
        // should ahve log added here
        return false;
    }

    outTitle[0] = '\0';

    if (baseTitle && baseTitle[0])
    {
        strcpy_s(outTitle, outTitleSize, baseTitle);
        return true;
    }

    if (virtualPath && virtualPath[0])
    {
        CopyBaseName(outTitle, outTitleSize, virtualPath);
        return outTitle[0] != '\0';
    }

    strcpy_s(outTitle, outTitleSize, "Archive Stream");

    return true;
}