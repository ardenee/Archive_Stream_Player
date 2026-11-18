#include <windows.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <filesystem>

#include "archive_backend.h"
#include "archive_stream_owner.h"
#include "archive_passwords.h"
#include "../shared/archive_track_tags.h"
#include "../shared/archive_unified_cache.h"
#include "playlist_expand.h"

#include "wa_ipc.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "archive_metadata.h"
#include "debug_log.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_winamp_text.h"

//#include "playlist_expand.h"


static const char* VIRTUAL_SUFFIX = ".archive_stream";

#ifndef IPC_PE_DELETEINDEX
#define IPC_PE_DELETEINDEX 104
#endif

#ifndef IPC_PE_SWAPINDEX
#define IPC_PE_SWAPINDEX 105
#endif

static HWND GetPlaylistEditorWindow(HWND hwndWinamp)
{
    if (!hwndWinamp)
        return NULL;

    return (HWND)SendMessage(hwndWinamp, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND);
}

static bool DeletePlaylistIndex(HWND hwndWinamp, int index)
{
    HWND hwndPE = GetPlaylistEditorWindow(hwndWinamp);

    if (!hwndPE || index < 0)
    {
        DebugLogWrite("ERROR", "GEN", "DELETE_INDEX_FAIL", "index=%d | reason=invalid_playlist_editor_or_index", index);
        return false;
    }

    SendMessage(hwndPE, WM_USER, IPC_PE_DELETEINDEX, index);
    DebugLogWrite("INFO", "GEN", "DELETE_INDEX", "index=%d", index);
    return true;
}

static bool SwapPlaylistIndexes(HWND hwndWinamp, int indexA, int indexB)
{
    HWND hwndPE = GetPlaylistEditorWindow(hwndWinamp);

    if (!hwndPE || indexA < 0 || indexB < 0)
    {
        DebugLogWrite("ERROR", "GEN", "SWAP_INDEX_FAIL", "index_a=%d | index_b=%d | reason=invalid_playlist_editor_or_index", indexA, indexB);
        return false;
    }

    LPARAM swapParam = (LPARAM)(indexA + (indexB << 16));
    SendMessage(hwndPE, WM_USER, IPC_PE_SWAPINDEX, swapParam);
    return true;
}

static bool MovePlaylistItem(HWND hwndWinamp, int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
        return true;

    if (fromIndex < 0 || toIndex < 0)
        return false;

    if (fromIndex > toIndex)
    {
        for (int i = fromIndex; i > toIndex; --i)
        {
            if (!SwapPlaylistIndexes(hwndWinamp, i, i - 1))
                return false;
        }

        return true;
    }

    for (int i = fromIndex; i < toIndex; ++i)
    {
        if (!SwapPlaylistIndexes(hwndWinamp, i, i + 1))
            return false;
    }

    return true;
}

using PlaylistItem = ArchivePreparedPlaylistItem;

/*
struct RebuildPreview
{
    int targetIndex = -1;
    std::vector<PlaylistItem> rebuilt;
    std::string firstVirtualPath;
};
*/

static int GetTargetPlaylistIndex(HWND hwndWinamp)
{
    int selected = (int)SendMessage(hwndWinamp, WM_WA_IPC, -1, IPC_PLAYLIST_GET_NEXT_SELECTED);

    if (selected >= 0)
        return selected;

    return (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTPOS);
}

static bool GetPlaylistItem(HWND hwndWinamp, int index, char* outPath, size_t outPathSize)
{
    if (!hwndWinamp || !outPath || outPathSize == 0)
    {
        DebugLogWrite("WARN", "GEN", "PLAYLIST_ITEM_FAIL", "index=%d | reason=invalid_input", index);
        return false;
    }

    outPath[0] = '\0';

    char* item = (char*)SendMessage(hwndWinamp, WM_WA_IPC, index, IPC_GETPLAYLISTFILE);

    if (!item || !item[0])
    {
        DebugLogWrite("WARN", "GEN", "PLAYLIST_ITEM_FAIL", "index=%d | reason=empty_or_missing_item", index);
        return false;
    }

    strcpy_s(outPath, outPathSize, item);
    return true;
}

static std::string GetPlaylistTitle(HWND hwndWinamp, int index)
{
    if (!hwndWinamp || index < 0)
        return std::string();

    char* title = (char*)SendMessage(hwndWinamp, WM_WA_IPC, index, IPC_GETPLAYLISTTITLE);

    if (!title || !title[0])
        return std::string();

    return std::string(title);
}

static std::string GetFileNameOnly(const char* path)
{
    if (!path || !path[0])
        return std::string();

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* base = slash1 > slash2 ? slash1 : slash2;

    if (base)
        base++;
    else
        base = path;

    return std::string(base);
}

static std::string StripExtensionFromFileName(std::string value)
{
    if (value.empty())
        return value;

    size_t slash1 = value.find_last_of('\\');
    size_t slash2 = value.find_last_of('/');
    size_t slash = std::string::npos;

    if (slash1 != std::string::npos && slash2 != std::string::npos)
        slash = slash1 > slash2 ? slash1 : slash2;
    else if (slash1 != std::string::npos)
        slash = slash1;
    else
        slash = slash2;

    if (slash != std::string::npos)
        value = value.substr(slash + 1);

    size_t dot = value.find_last_of('.');

    if (dot != std::string::npos && dot > 0)
        value = value.substr(0, dot);

    return value;
}

static std::string GetCleanArchiveNameFromVirtualPath(const char* virtualPath)
{
    if (!virtualPath || !virtualPath[0]) return std::string();

    std::string value = virtualPath;
    size_t marker = value.find(".archive_stream|");
    if (marker != std::string::npos) value = value.substr(0, marker);

    size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos) value = value.substr(slash + 1);

    size_t dot = value.find_last_of('.');
    if (dot != std::string::npos && dot > 0) value = value.substr(0, dot);

    return value;
}

// -----------------------------------------------------------------------------
// BuildArchiveStreamIniPath
//
// Builds the shared Archive Stream config path.
//
// The input plugin saves display settings via ArchiveBuildPluginDataFilePath().
// The general plugin must read the same file or expanded playlist titles will
// keep using stale/default display-template values.
// -----------------------------------------------------------------------------
static void BuildArchiveStreamIniPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        return;
    }

    outPath[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("in_archive_stream.ini", outPath, outPathSize))
    {
        DebugLogWrite("WARN", "GEN", "DISPLAY_CONFIG_SKIP", "reason=plugin_data_file_path_failed");
        return;
    }
}

static void ReplaceAllTokens(std::string& text, const char* token, const std::string& value)
{
    if (!token || !token[0]) return;

    size_t pos = 0;
    size_t tokenLen = strlen(token);

    while ((pos = text.find(token, pos)) != std::string::npos)
    {
        text.replace(pos, tokenLen, value);
        pos += value.length();
    }
}
// -----------------------------------------------------------------------------
// DecodeDisplayConfigText
//
// Decodes display config text saved by the input plugin.
//
// The input plugin writes display prefix/postfix/template through EncodeConfigText()
// so spaces become %20 and literal percent signs become %25. The general plugin
// must decode these values before building playlist titles, otherwise prefix
// values such as "<archive> : " appear as "<archive>%20:%20" in Winamp.
// -----------------------------------------------------------------------------
static std::string DecodeDisplayConfigText(const char* input)
{
    std::string output;

    if (!input)
    {
        return output;
    }

    for (size_t i = 0; input[i]; ++i)
    {
        if (input[i] == '%' && input[i + 1] == '2' && input[i + 2] == '0')
        {
            output.push_back(' ');
            i += 2;
        }
        else if (input[i] == '%' && input[i + 1] == '2' && input[i + 2] == '5')
        {
            output.push_back('%');
            i += 2;
        }
        else
        {
            output.push_back(input[i]);
        }
    }

    return output;
}

// -----------------------------------------------------------------------------
// LoadDisplayConfigValue
//
// Loads and decodes one display config value from the shared input-plugin INI.
//
// Values are stored encoded by config_ui.cpp so spaces survive Win32 INI reads.
// This function returns the decoded display value for title building.
// -----------------------------------------------------------------------------
static std::string LoadDisplayConfigValue(const char* key, const char* fallback)
{
    char iniPath[MAX_PATH * 4];
    char encodedValue[256];

    iniPath[0] = '\0';
    encodedValue[0] = '\0';

    BuildArchiveStreamIniPath(iniPath, sizeof(iniPath));

    if (!iniPath[0])
    {
        return fallback ? fallback : "";
    }

    GetPrivateProfileStringA("ArchiveStream", key, fallback ? fallback : "", encodedValue, sizeof(encodedValue), iniPath);

    std::string decodedValue = DecodeDisplayConfigText(encodedValue);

    if (_stricmp(key, "DisplayTemplate") == 0 || _stricmp(key, "DisplayPrefix") == 0 || _stricmp(key, "DisplayPostfix") == 0)
    {
        DebugLogWrite("INFO", "GEN", "DISPLAY_CONFIG_LOAD", "path=%s | key=%s | encoded=%s | decoded=%s", iniPath, key, encodedValue, decodedValue.c_str());
    }

    return decodedValue;
}

static std::string BuildExpandedArchiveTitle(const char* virtualPath, const char* entryName, size_t displayIndex)
{
    if (ArchiveAdapterPreferencesConfigured() && virtualPath && entryName)
    {
        char archivePath[MAX_PATH * 4]{};
        char internal[1024]{};
        mz_uint index = 0;
        if (ParseVirtualArchivePath(virtualPath, archivePath, sizeof(archivePath), &index, internal, sizeof(internal)))
        {
            const auto common = ArchiveAdapterPreferencesLoad();
            const std::wstring title = ArchiveAdapterFormatTitle(ArchiveAdapterUtf8ToWide(archivePath),
                ArchiveAdapterUtf8ToWide(entryName), static_cast<unsigned>(displayIndex ? displayIndex : index + 1), common.titleTemplate);
            // Winamp's legacy playlist enqueue/title IPC is ANSI, while the
            // shared title formatter operates on Unicode. Convert at the host
            // boundary instead of displaying UTF-8 bytes as ANSI characters.
            const int length = WideCharToMultiByte(CP_ACP, 0, title.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (length > 1)
            {
                std::string converted(static_cast<size_t>(length), '\0');
                if (WideCharToMultiByte(CP_ACP, 0, title.c_str(), -1, converted.data(), length, nullptr, nullptr) == length)
                {
                    converted.resize(static_cast<size_t>(length - 1));
                    return converted;
                }
            }
        }
    }

    std::string filename = StripExtensionFromFileName(entryName);
    std::string ansiFilename;
    if (ArchiveWinampUtf8ToAnsi(filename, ansiFilename))
        filename = std::move(ansiFilename);
    std::string archive = GetCleanArchiveNameFromVirtualPath(virtualPath);

    if (filename.empty() && virtualPath && virtualPath[0]) 
        filename = StripExtensionFromFileName(virtualPath);

    if (filename.empty()) 
        filename = "archive track";

    if (archive.empty()) 
        archive = "archive";

    std::string prefix = LoadDisplayConfigValue("DisplayPrefix", "");
    std::string postfix = LoadDisplayConfigValue("DisplayPostfix", "");
    std::string displayTemplate = LoadDisplayConfigValue("DisplayTemplate", "");

    if (displayTemplate.empty())
    {
        displayTemplate = "<filename>";
    }

    std::string fullTemplate;
    fullTemplate.reserve(prefix.length() + displayTemplate.length() + postfix.length() + 1);
    fullTemplate += prefix;
    fullTemplate += displayTemplate;
    fullTemplate += postfix;

    bool hasFilenameToken = fullTemplate.find("<filename>") != std::string::npos;
    bool hasNumberToken = fullTemplate.find("<#>") != std::string::npos;
    bool hasArchiveToken = fullTemplate.find("<archive>") != std::string::npos;

    if (!hasFilenameToken && !hasNumberToken && !hasArchiveToken)
    {
        fullTemplate += "<filename>";
    }

    char numberBuffer[32];
    sprintf_s(numberBuffer, sizeof(numberBuffer), "%zu", displayIndex);

    ReplaceAllTokens(fullTemplate, "<archive>", archive);
    ReplaceAllTokens(fullTemplate, "<filename>", filename);
    ReplaceAllTokens(fullTemplate, "<#>", numberBuffer);

    return fullTemplate;
}



static bool HasExtension(const char* path, const char* ext)
{
    if (!path || !ext)
        return false;

    const char* dot = strrchr(path, '.');

    if (!dot)
        return false;

    return _stricmp(dot, ext) == 0;
}

static std::string BuildVirtualArchivePath(const char* archivePath, uint32_t index, const char* internalPath)
{
    std::string result;
    result.reserve(4096);

    result += archivePath ? archivePath : "";
    result += VIRTUAL_SUFFIX;
    result += "|";
    result += std::to_string(index);
    result += "|";

    if (internalPath && internalPath[0])
        result += internalPath;

    return result;
}

static bool BuildReplacementItemsForArchive(const char* archivePath, std::vector<PlaylistItem>& outItems, std::string& outFirstVirtualPath)
{
    outItems.clear();
    outFirstVirtualPath.clear();

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "GEN", "BUILD_REPLACEMENTS_FAIL", "archive=%s | reason=invalid_input", archivePath ? archivePath : "");
        return false;
    }

    std::vector<ArchiveAudioEntry> entries;

    if (!ArchiveListAudioEntries(archivePath, entries))
    {
        DebugLogWrite("ERROR", "GEN", "BUILD_REPLACEMENTS_FAIL", "archive=%s | reason=ArchiveListAudioEntries_failed", archivePath);
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "GEN", "BUILD_REPLACEMENTS_FAIL", "archive=%s | reason=no_audio_entries", archivePath);
        return false;
    }

    outItems.reserve(entries.size());

    // Playlist expansion is intentionally metadata-free: the selected track
    // can start immediately, then a low-priority worker updates all entries.

    for (size_t i = 0; i < entries.size(); ++i)
    {
        PlaylistItem item;
        item.path = BuildVirtualArchivePath(archivePath, entries[i].index, entries[i].name.c_str());
        item.title = BuildExpandedArchiveTitle(item.path.c_str(), entries[i].name.c_str(), i + 1);
        item.lengthMs = -1000;
        item.useMeta = false;

        bool skipExpansionMetadataProbe = HasExtension(archivePath, ".rar");

        if (!skipExpansionMetadataProbe)
        {
            char virtualTitle[1024];
            int virtualLengthMs = -1000;

            virtualTitle[0] = '\0';

            if (GetVirtualArchiveEntryInfoCacheOnly(item.path.c_str(), virtualTitle, sizeof(virtualTitle), &virtualLengthMs))
            {
                if (virtualLengthMs > 0)
                {
                    item.lengthMs = virtualLengthMs;
                }
            }
        }

        if (outFirstVirtualPath.empty()) outFirstVirtualPath = item.path;

        DebugLogWrite("INFO", "GEN", "VIRTUAL_METADATA_READY", "archive=%s | index=%u | title=%s | length_ms=%d | metadata_probe=%s", archivePath, (unsigned int)entries[i].index, item.title.c_str(), item.lengthMs, skipExpansionMetadataProbe ? "skipped_rar" : "cache_only");
        outItems.push_back(item);
    }

    return !outItems.empty();
}

// WACUP's playlist title is an immutable pre-play snapshot. Do all archive
// work on the worker thread, in shared routing order: stream, RAM, cache.
// Each source is read at most once for its tag prefix. A missing/unreadable
// tag retains the archive entry filename as the final playlist title.
bool PrepareArchivePlaylistForWacup(const char* archivePath, std::vector<ArchivePreparedPlaylistItem>& outItems, volatile LONG* generation, LONG expectedGeneration)
{
    std::string firstVirtualPath;
    if (!BuildReplacementItemsForArchive(archivePath, outItems, firstVirtualPath))
        return false;

    const std::wstring wideArchive = ArchiveAdapterUtf8ToWide(archivePath);
    std::vector<ArchiveEntry> entries;
    std::string listError;
    if (wideArchive.empty() || !ArchiveListEntries(wideArchive.c_str(), entries, listError))
    {
        DebugLogWrite("WARN", "GEN", "PREFLIGHT_LIST_FAIL", "archive=%s | error=%s", archivePath, listError.c_str());
        return true;
    }
    const ArchiveAdapterPreferences settings = ArchiveAdapterPreferencesLoad();
    constexpr size_t kTagPrefix = 256 * 1024;
    constexpr uint64_t kMb = 1024ull * 1024ull;
    unsigned taggedCount = 0;
    unsigned durationCount = 0;

    for (PlaylistItem& item : outItems)
    {
        if (generation && InterlockedCompareExchange(generation, 0, 0) != expectedGeneration)
            return false;

        char parsedArchive[MAX_PATH * 4]{};
        char internalName[4096]{};
        mz_uint index = 0;
        if (!ParseVirtualArchivePath(item.path.c_str(), parsedArchive, sizeof(parsedArchive), &index, internalName, sizeof(internalName)))
            continue;

        const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& candidate)
        {
            return candidate.index == index && candidate.isSupportedAudio && !candidate.isFolder;
        });
        if (found == entries.end() || found->size == 0)
            continue;
        const ArchiveEntry& entry = *found;

        const auto dot = entry.path.find_last_of('.');
        if (dot == std::string::npos)
            continue;
        std::string extension = entry.path.substr(dot + 1);
        for (char& ch : extension)
            ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));

        std::vector<unsigned char> prefix;
        std::vector<unsigned char> decoded;
        std::wstring preparedCache;
        bool usedStream = false;
        bool usedMemory = false;

        if (!wideArchive.empty() && ArchiveEntryCanStream(archivePath, entry.index))
        {
            ArchiveStreamOwner stream;
            std::string openError;
            if (stream.Open(wideArchive.c_str(), entry.index, openError))
            {
                const size_t limit = static_cast<size_t>((std::min)(entry.size, static_cast<uint64_t>(kTagPrefix)));
                prefix.resize(limit);
                size_t count = 0;
                while (count < prefix.size())
                {
                    const ArchiveStreamReadResult read = stream.ReadDetailed(prefix.data() + count, prefix.size() - count);
                    count += read.bytesRead;
                    if (read.errorKind != ArchiveStreamErrorKind::None || read.bytesRead == 0)
                        break;
                }
                stream.Close();
                if (count == limit && count != 0)
                    usedStream = true;
                else
                    prefix.clear();
            }
        }

        if (!usedStream && entry.size <= static_cast<uint64_t>(settings.memoryMb) * kMb)
        {
            std::string extractError;
            if (ArchiveExtractEntryToMemory(archivePath, entry.index, decoded, extractError) && decoded.size() == entry.size)
            {
                const size_t prefixSize = (std::min)(decoded.size(), kTagPrefix);
                prefix.assign(decoded.begin(), decoded.begin() + prefixSize);
                usedMemory = true;
            }
            else
                decoded.clear();
        }

        if (!usedStream && !usedMemory && !wideArchive.empty() && settings.cacheLimitMb)
        {
            std::string cacheError;
            if (ArchiveUnifiedCache::Prepare(wideArchive, entry, settings, preparedCache, cacheError))
            {
                std::ifstream cached(std::filesystem::path(preparedCache), std::ios::binary);
                if (cached)
                {
                    prefix.resize(static_cast<size_t>((std::min)(entry.size, static_cast<uint64_t>(kTagPrefix))));
                    cached.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
                    prefix.resize(static_cast<size_t>(cached.gcount()));
                }
            }
            else
                DebugLogWrite("WARN", "GEN", "PREFLIGHT_CACHE_UNAVAILABLE", "archive=%s | index=%u | error=%s", archivePath, (unsigned)entry.index, cacheError.c_str());
        }

        ArchiveTrackTags tags;
        if (!prefix.empty() && ArchiveTrackTagParser::Parse(extension, prefix.data(), prefix.size(), tags) && !tags.title.empty())
        {
            const std::string formatted = tags.artist.empty() ? tags.title : tags.artist + " - " + tags.title;
            std::string convertedTitle;
            if (ArchiveWinampUtf8ToAnsi(formatted, convertedTitle, true) && !convertedTitle.empty())
            {
                item.title = convertedTitle;
                ArchiveWinampUtf8ToAnsi(tags.title, item.tagTitle, true);
                ArchiveWinampUtf8ToAnsi(tags.artist, item.tagArtist, true);
                ArchiveWinampUtf8ToAnsi(tags.album, item.tagAlbum, true);
                ArchiveWinampUtf8ToAnsi(tags.albumArtist, item.tagAlbumArtist, true);
                ArchiveWinampUtf8ToAnsi(tags.genre, item.tagGenre, true);
                ArchiveWinampUtf8ToAnsi(tags.year, item.tagYear, true);
                item.tagTrackNumber = tags.trackNumber;
                ++taggedCount;
            }
        }

        int durationMs = -1000;
        bool durationKnown = GetAudioDurationMsFromArchiveEntryCacheOnly(archivePath, entry.index, entry.size, entry.path.c_str(), &durationMs);
        if (!durationKnown && usedMemory)
            durationKnown = GetAudioDurationMsFromPreparedMemory(decoded.data(), decoded.size(), entry, &durationMs);
        if (!durationKnown && !preparedCache.empty())
            durationKnown = GetAudioDurationMsFromPreparedCacheFile(preparedCache.c_str(), entry, &durationMs);
        if (!durationKnown && usedStream)
            durationKnown = GetAudioDurationMsFromArchiveEntryBackground(archivePath, entry.index, entry.size, entry.path.c_str(), &durationMs);
        if (durationKnown && durationMs > 0)
        {
            item.lengthMs = durationMs;
            ++durationCount;
        }

        item.useMeta = true;
        DebugLogWrite("INFO", "GEN", "PREFLIGHT_ENTRY_READY", "archive=%s | index=%u | title=%s | duration_ms=%d | route=%s", archivePath, (unsigned)entry.index, item.title.c_str(), item.lengthMs, usedStream ? "STREAM" : usedMemory ? "MEMORY" : !preparedCache.empty() ? "CACHE" : "FILENAME");
        SwitchToThread();
    }

    DebugLogWrite("INFO", "GEN", "PREFLIGHT_COMPLETE", "archive=%s | entries=%zu | tagged=%u | durations=%u", archivePath, outItems.size(), taggedCount, durationCount);
    return true;
}

/*    
static bool BuildExpandedPlaylistPreview(HWND hwndWinamp, int targetIndex, RebuildPreview& outPreview)
{
    outPreview.targetIndex = targetIndex;
    outPreview.rebuilt.clear();
    outPreview.firstVirtualPath.clear();

    std::vector<PlaylistItem> original;

    if (!ReadCurrentPlaylist(hwndWinamp, original))
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_PREVIEW_FAIL", "target_index=%d | reason=read_playlist_failed", targetIndex);
        return false;
    }

    if (targetIndex < 0 || targetIndex >= (int)original.size())
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_PREVIEW_FAIL", "target_index=%d | playlist_count=%zu | reason=target_index_out_of_range", targetIndex, original.size());
        return false;
    }

    const std::string& targetPath = original[targetIndex].path;

    if (!IsSupportedArchivePath(targetPath.c_str()))
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_PREVIEW_FAIL", "target_index=%d | path=%s | reason=unsupported_archive", targetIndex, targetPath.c_str());
        return false;
    }

    std::vector<ArchiveAudioEntry> entries;

    if (!ArchiveListAudioEntries(targetPath.c_str(), entries))
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_SCAN_FAIL", "archive=%s | reason=ArchiveListAudioEntries_failed", targetPath.c_str());
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_NO_AUDIO", "archive=%s", targetPath.c_str());
        return false;
    }

    for (int i = 0; i < (int)original.size(); ++i)
    {
        if (i != targetIndex)
        {
            outPreview.rebuilt.push_back(original[i]);
            continue;
        }

        for (size_t t = 0; t < entries.size(); ++t)
        {
            PlaylistItem virtualItem;
            virtualItem.path = BuildVirtualArchivePath(targetPath.c_str(), entries[t].index, entries[t].name.c_str());
            virtualItem.title = BuildExpandedArchiveTitle(virtualItem.path.c_str(), entries[t].name.c_str());
            char virtualTitle[1024];
            int virtualLengthMs = -1000;
            virtualTitle[0] = '\0';

            if (GetVirtualArchiveEntryInfo(virtualItem.path.c_str(), virtualTitle, sizeof(virtualTitle), &virtualLengthMs))
            {
                if (virtualTitle[0])
                    virtualItem.title = virtualTitle;

                if (virtualLengthMs > 0)
                    virtualItem.lengthMs = virtualLengthMs;
                else
                    virtualItem.lengthMs = -1000;
            }
            else
            {
                virtualItem.lengthMs = -1000;
            }

            virtualItem.useMeta = true;
            DebugLogWrite("INFO", "GEN", "VIRTUAL_METADATA_READY", "archive=%s | index=%u | title=%s | length_ms=%d", targetPath.c_str(), (unsigned int)entries[t].index, virtualItem.title.c_str(), virtualItem.lengthMs);

            if (outPreview.firstVirtualPath.empty())
                outPreview.firstVirtualPath = virtualItem.path;

            outPreview.rebuilt.push_back(virtualItem);
        }
    }

    return true;
}
*/

static bool BuildArchiveReplacementItems(HWND hwndWinamp, int targetIndex, std::vector<PlaylistItem>& replacementItems, std::string& firstVirtualPath)
{
    replacementItems.clear();
    firstVirtualPath.clear();

    if (!hwndWinamp || targetIndex < 0)
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_REPLACEMENT_FAIL", "target_index=%d | reason=invalid_input", targetIndex);
        return false;
    }

    char targetPath[4096];
    targetPath[0] = '\0';

    if (!GetPlaylistItem(hwndWinamp, targetIndex, targetPath, sizeof(targetPath)))
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_REPLACEMENT_FAIL", "target_index=%d | reason=target_item_read_failed", targetIndex);
        return false;
    }

    if (!IsSupportedArchivePath(targetPath))
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_REPLACEMENT_FAIL", "target_index=%d | path=%s | reason=unsupported_archive", targetIndex, targetPath);
        return false;
    }

    std::vector<ArchiveAudioEntry> entries;

    if (!ArchiveListAudioEntries(targetPath, entries))
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_SCAN_FAIL", "archive=%s | reason=ArchiveListAudioEntries_failed", targetPath);
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_NO_AUDIO", "archive=%s", targetPath);
        return false;
    }

    replacementItems.reserve(entries.size());

    for (size_t t = 0; t < entries.size(); ++t)
    {
        PlaylistItem virtualItem;
        virtualItem.path = BuildVirtualArchivePath(targetPath, entries[t].index, entries[t].name.c_str());
        virtualItem.title = BuildExpandedArchiveTitle(virtualItem.path.c_str(), entries[t].name.c_str(), t + 1);
        virtualItem.lengthMs = -1000;
        virtualItem.useMeta = true;

        char virtualTitle[1024];
        int virtualLengthMs = -1000;
        virtualTitle[0] = '\0';

        if (GetVirtualArchiveEntryInfoCacheOnly(virtualItem.path.c_str(), virtualTitle, sizeof(virtualTitle), &virtualLengthMs))
        {
            if (virtualLengthMs > 0)
            {
                virtualItem.lengthMs = virtualLengthMs;
            }
        }

        DebugLogWrite("INFO", "GEN", "VIRTUAL_METADATA_READY", "archive=%s | index=%u | title=%s | length_ms=%d", targetPath, (unsigned int)entries[t].index, virtualItem.title.c_str(), virtualItem.lengthMs);

        if (firstVirtualPath.empty())
            firstVirtualPath = virtualItem.path;

        replacementItems.push_back(virtualItem);
    }

    return !replacementItems.empty();
}

static bool EnqueueFilePlainToWinamp(HWND hwndWinamp, const char* path)
{
    if (!hwndWinamp || !path || !path[0])
        return false;

    COPYDATASTRUCT cds;
    memset(&cds, 0, sizeof(cds));
    cds.dwData = IPC_ENQUEUEFILE;
    cds.lpData = (void*)path;
    cds.cbData = (DWORD)strlen(path) + 1;
    // WACUP returns zero from WM_COPYDATA even when it enqueues the file.
    // Verify the observable playlist change, rather than relying on lresult.
    const int countBefore = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    const LRESULT result = SendMessage(hwndWinamp, WM_COPYDATA, 0, (LPARAM)&cds);
    const int countAfter = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    const bool appended = countBefore >= 0 && countAfter == countBefore + 1;

    DebugLogWrite(appended ? "INFO" : "ERROR", "GEN", appended ? "ENQUEUE_PLAIN" : "ENQUEUE_PLAIN_FAIL", "path=%s | lresult=%lld | before=%d | after=%d", path, (long long)result, countBefore, countAfter);
    return appended;
}

static void RefreshPlaylistCacheForPath(HWND hwndWinamp, const char* path)
{
    if (!hwndWinamp || !path || !path[0])
        return;

    wchar_t widePath[4096];
    widePath[0] = L'\0';
    int converted = MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, (int)(sizeof(widePath) / sizeof(widePath[0])));

    if (converted <= 0 || widePath[0] == L'\0') {
        DebugLogWrite("WARN", "GEN", "PLCACHE_REFRESH_SKIP", "path=%s | reason=path_wide_conversion_failed | winerr=%lu", path, GetLastError());
        return;
    }

    SendMessage(hwndWinamp, WM_WA_IPC, (WPARAM)widePath, IPC_REFRESHPLCACHE);
    DebugLogWrite("INFO", "GEN", "PLCACHE_REFRESH_PATH", "path=%s", path);
}

static bool IsVirtualArchivePlaylistPath(const char* path)
{
    return path && strstr(path, ".archive_stream|") != NULL;
}

static bool EnqueueFileToWinamp(HWND hwndWinamp, const PlaylistItem& item)
{
    if (!hwndWinamp || item.path.empty())
    {
        DebugLogWrite("ERROR", "GEN", "ENQUEUE_FAIL", "path=%s | reason=invalid_input", item.path.c_str());
        return false;
    }

    // WACUP's normal WM_COPYDATA enqueue uses its filename as the initial
    // playlist title, then replaces it after metadata refresh or playback.
    // The documented Winamp metadata-enqueue form can provide the already
    // verified ID3 title immediately. Keep Winamp's existing plain enqueue
    // route unchanged until its host-specific behavior is separately tested.
    bool metadataEnqueued = false;
    char hostExe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, hostExe, sizeof(hostExe));
    const char* baseName = strrchr(hostExe, '\\');
    baseName = baseName ? baseName + 1 : hostExe;
    if (_stricmp(baseName, "wacup.exe") == 0 && !item.title.empty())
    {
        const int before = static_cast<int>(SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH));
        enqueueFileWithMetaStruct metadata{};
        metadata.filename = item.path.c_str();
        metadata.title = item.title.c_str();
        metadata.length = item.lengthMs > 0 ? (item.lengthMs + 500) / 1000 : -1;
        const LRESULT result = SendMessage(hwndWinamp, WM_WA_IPC, reinterpret_cast<WPARAM>(&metadata), IPC_ENQUEUEFILE);
        const int after = static_cast<int>(SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH));
        if (before >= 0 && after == before + 1)
        {
            DebugLogWrite("INFO", "GEN", "ENQUEUE_WITH_METADATA", "path=%s | title=%s | length_seconds=%d | result=%lld", item.path.c_str(), item.title.c_str(), metadata.length, static_cast<long long>(result));
            metadataEnqueued = true;
        }
        else
        {
            DebugLogWrite("WARN", "GEN", "ENQUEUE_WITH_METADATA_FALLBACK", "path=%s | before=%d | after=%d | result=%lld", item.path.c_str(), before, after, static_cast<long long>(result));
        }
    }

    if (!metadataEnqueued && !EnqueueFilePlainToWinamp(hwndWinamp, item.path.c_str()))
    {
        DebugLogWrite("ERROR", "GEN", "ENQUEUE_FAIL", "path=%s | reason=plain_enqueue_failed", item.path.c_str());
        return false;
    }

    bool isVirtualArchivePath = IsVirtualArchivePlaylistPath(item.path.c_str());
    bool isVirtualRarPath = isVirtualArchivePath && strstr(item.path.c_str(), ".rar.archive_stream|") != NULL;

    if (isVirtualArchivePath && !isVirtualRarPath)
    {
        RefreshPlaylistCacheForPath(hwndWinamp, item.path.c_str());
    }
    else if (isVirtualRarPath)
    {
        DebugLogWrite("INFO", "GEN", "PLCACHE_REFRESH_SKIP", "path=%s | reason=rar_virtual_entry_deferred_metadata", item.path.c_str());
    }

    const int insertedIndex = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH) - 1;
    const std::string observedTitle = GetPlaylistTitle(hwndWinamp, insertedIndex);
    DebugLogWrite("INFO", "GEN", "ENQUEUE_ITEM", "path=%s | requested_title=%s | observed_title=%s | index=%d | scanned_length_ms=%d | enqueue_mode=%s", item.path.c_str(), item.title.c_str(), observedTitle.c_str(), insertedIndex, item.lengthMs, metadataEnqueued ? "metadata" : "plain");
    return true;
}

static bool ReplacePlaylistItemWithVirtualEntries(HWND hwndWinamp, int targetIndex, const std::vector<PlaylistItem>& replacementItems)
{
    if (!hwndWinamp || targetIndex < 0 || replacementItems.empty())
    {
        DebugLogWrite("ERROR", "GEN", "LOCAL_REPLACE_FAIL", "target_index=%d | replacement_count=%zu | reason=invalid_input", targetIndex, replacementItems.size());
        return false;
    }

    int lengthBefore = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);

    if (targetIndex >= lengthBefore)
    {
        DebugLogWrite("ERROR", "GEN", "LOCAL_REPLACE_FAIL", "target_index=%d | playlist_length=%d | reason=target_out_of_range", targetIndex, lengthBefore);
        return false;
    }

    if (!DeletePlaylistIndex(hwndWinamp, targetIndex))
    {
        DebugLogWrite("ERROR", "GEN", "LOCAL_REPLACE_FAIL", "target_index=%d | reason=delete_failed", targetIndex);
        return false;
    }

    int lengthAfterDelete = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);

    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        if (!EnqueueFileToWinamp(hwndWinamp, replacementItems[i]))
        {
            DebugLogWrite("ERROR", "GEN", "LOCAL_REPLACE_FAIL", "target_index=%d | replacement_index=%zu | reason=enqueue_failed", targetIndex, i);
            return false;
        }
    }

    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        int appendedIndex = lengthAfterDelete + (int)i;
        int desiredIndex = targetIndex + (int)i;

        if (!MovePlaylistItem(hwndWinamp, appendedIndex, desiredIndex))
        {
            DebugLogWrite("ERROR", "GEN", "LOCAL_REPLACE_FAIL", "from_index=%d | to_index=%d | reason=move_failed", appendedIndex, desiredIndex);
            return false;
        }
    }

    DebugLogWrite("INFO", "GEN", "LOCAL_REPLACE_DONE", "target_index=%d | replacement_count=%zu", targetIndex, replacementItems.size());
    return true;
}

static void RefreshInsertedVirtualEntries(HWND hwndWinamp, int firstIndex, const std::vector<PlaylistItem>& replacementItems)
{
    if (!hwndWinamp || firstIndex < 0 || replacementItems.empty()) return;

    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        if (!IsVirtualArchivePlaylistPath(replacementItems[i].path.c_str())) continue;

        RefreshPlaylistCacheForPath(hwndWinamp, replacementItems[i].path.c_str());
        DebugLogWrite("INFO", "GEN", "REFRESH_INSERTED_VIRTUAL", "playlist_index=%d | path=%s", firstIndex + (int)i, replacementItems[i].path.c_str());
    }
}

static bool QueuePlayExpandedIndex(HWND hwndWinamp, int playlistIndex, const char* virtualPath)
{
    if (!hwndWinamp || playlistIndex < 0)
    {
        DebugLogWrite("ERROR", "GEN", "PLAY_QUEUE_FAIL", "index=%d | reason=invalid_input", playlistIndex);
        return false;
    }

    DebugLogWrite("INFO", "GEN", "PLAY_QUEUE_REQUEST", "index=%d | path=%s", playlistIndex, virtualPath ? virtualPath : "");

    /*
        Do not use IPC_PLAYFILE here.

        IPC_PLAYFILE caused Winamp 5.9.2 to crash after being given the
        generated virtual .archive_stream path. The safer handoff is:
            expansion complete -> set playlist position -> post Winamp play command

        The actual play command is handled by Winamp from its playlist state,
        not by passing it a temporary C string pointer.
    */

    /*
        Do not call IPC_REFRESHPLCACHE with 0 here.

        Passing 0 flushes the whole playlist title cache, which makes Winamp
        re-query titles for everything. The expanded virtual entries already
        exist in the playlist by this point, so just set the playlist position.
    */
    SendMessage(hwndWinamp, WM_WA_IPC, playlistIndex, IPC_SETPLAYLISTPOS);

    DebugLogWrite("INFO", "GEN", "PLAY_QUEUE_POSITION_SET", "index=%d", playlistIndex);

    return true;
}


static bool RebuildPlaylistWithExpandedZip(HWND hwndWinamp, int targetIndex, bool startPlayback, const std::vector<PlaylistItem>* preparedItems = nullptr)
{
    if (!hwndWinamp)
    {
        DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | reason=missing_winamp_window", targetIndex);
        return false;
    }

    int playlistLength = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);

    if (targetIndex < 0 || targetIndex >= playlistLength)
    {
        DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | playlist_length=%d | reason=target_out_of_range", targetIndex, playlistLength);
        return false;
    }

    char archivePath[4096];
    archivePath[0] = '\0';

    if (!GetPlaylistItem(hwndWinamp, targetIndex, archivePath, sizeof(archivePath)))
    {
        DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | reason=failed_to_read_target_item", targetIndex);
        return false;
    }

    if (!IsSupportedArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "GEN", "REBUILD_FAIL", "target_index=%d | path=%s | reason=unsupported_archive", targetIndex, archivePath);
        return false;
    }

    std::vector<PlaylistItem> replacementItems;
    std::string firstVirtualPath;

    if (preparedItems)
    {
        replacementItems = *preparedItems;
        if (replacementItems.empty())
            return false;

        const std::string expectedPrefix = std::string(archivePath) + ".archive_stream|";
        for (const auto& item : replacementItems)
        {
            if (_strnicmp(item.path.c_str(), expectedPrefix.c_str(), expectedPrefix.size()) != 0)
            {
                DebugLogWrite("WARN", "GEN", "PREFLIGHT_STALE", "archive=%s | path=%s | reason=archive_mismatch", archivePath, item.path.c_str());
                return false;
            }
        }
        firstVirtualPath = replacementItems.front().path;

        // WACUP may re-query a title while playing or stopping. Freeze the
        // final title/duration in its input plugin BEFORE enqueueing anything.
        using PrimeMetadataEx = int (__cdecl *)(const char*, const char*, const char*, const char*, const char*, const char*, const char*, const char*, unsigned, int);
        const HMODULE inputModule = GetModuleHandleA("in_archive_stream.dll");
        const auto prime = inputModule
            ? reinterpret_cast<PrimeMetadataEx>(GetProcAddress(inputModule, "ArchiveStreamPrimePlaylistMetadataEx"))
            : nullptr;
        if (!prime)
        {
            DebugLogWrite("ERROR", "GEN", "PREFLIGHT_COMMIT_FAIL", "archive=%s | reason=input_plugin_tag_prime_export_missing", archivePath);
            return false;
        }
        for (const auto& item : replacementItems)
        {
            if (!prime(item.path.c_str(), item.title.c_str(), item.tagTitle.c_str(), item.tagArtist.c_str(), item.tagAlbum.c_str(), item.tagAlbumArtist.c_str(), item.tagGenre.c_str(), item.tagYear.c_str(), item.tagTrackNumber, item.lengthMs))
            {
                DebugLogWrite("ERROR", "GEN", "PREFLIGHT_COMMIT_FAIL", "archive=%s | path=%s | reason=tag_prime_rejected", archivePath, item.path.c_str());
                return false;
            }
        }
    }
    else if (!BuildReplacementItemsForArchive(archivePath, replacementItems, firstVirtualPath))
    {
        DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | archive=%s | reason=build_replacements_failed", targetIndex, archivePath);
        return false;
    }

    // Append every replacement *before* removing the original archive.
    // This preserves the user's playlist if the host rejects a virtual path.
    const int lengthBefore = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    const auto removeAppendedItems = [&]()
    {
        const int lengthNow = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
        for (int index = lengthNow - 1; index >= lengthBefore; --index)
            DeletePlaylistIndex(hwndWinamp, index);
    };

    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        if (!EnqueueFileToWinamp(hwndWinamp, replacementItems[i]))
        {
            removeAppendedItems();
            DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | replacement_index=%zu | reason=enqueue_failed_original_preserved", targetIndex, i);
            return false;
        }
    }

    if (!DeletePlaylistIndex(hwndWinamp, targetIndex))
    {
        removeAppendedItems();
        DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "target_index=%d | archive=%s | reason=delete_target_failed_original_preserved", targetIndex, archivePath);
        return false;
    }

    const int lengthAfterDelete = lengthBefore - 1;

    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        int appendedIndex = lengthAfterDelete + (int)i;
        int desiredIndex = targetIndex + (int)i;

        if (!MovePlaylistItem(hwndWinamp, appendedIndex, desiredIndex))
        {
            DebugLogWrite("ERROR", "GEN", "REBUILD_FAIL", "from_index=%d | to_index=%d | reason=move_failed", appendedIndex, desiredIndex);
            return false;
        }
    }

    int firstInsertedIndex = targetIndex;
    for (size_t i = 0; i < replacementItems.size(); ++i)
    {
        const std::string observed = GetPlaylistTitle(hwndWinamp, targetIndex + (int)i);
        if (observed != replacementItems[i].title)
            DebugLogWrite("WARN", "GEN", "TITLE_MISMATCH_AFTER_MOVE", "index=%d | requested=%s | actual=%s", targetIndex + (int)i, replacementItems[i].title.c_str(), observed.c_str());
    }
    SendMessage(hwndWinamp, WM_WA_IPC, firstInsertedIndex, IPC_SETPLAYLISTPOS);
    DebugLogWrite("INFO", "GEN", "EXPAND_REBUILD_DONE", "target_index=%d | first_inserted_index=%d | replacement_count=%zu | start_playback=%d | first_virtual=%s", targetIndex, firstInsertedIndex, replacementItems.size(), startPlayback ? 1 : 0, firstVirtualPath.c_str());

    if (startPlayback)
    {
        if (!QueuePlayExpandedIndex(hwndWinamp, firstInsertedIndex, firstVirtualPath.c_str())) DebugLogWrite("ERROR", "GEN", "EXPAND_PLAY_FAIL", "target_index=%d | reason=queue_play_failed", targetIndex);
    }

    // Prepared WACUP entries already contain their final titles/durations.
    // Never queue a second post-play update that changes their playlist rows.
    if (preparedItems)
    {
        DebugLogWrite("INFO", "GEN", "PREFLIGHT_COMMITTED", "archive=%s | entries=%zu | final_before_playback=1", archivePath, replacementItems.size());
        return true;
    }

    // Winamp's existing route uses low-priority post-expansion metadata work.
    using ScheduleMetadata = int (__cdecl *)(const char*);
    const HMODULE inputModule = GetModuleHandleA("in_archive_stream.dll");
    const auto schedule = inputModule
        ? reinterpret_cast<ScheduleMetadata>(GetProcAddress(inputModule, "ArchiveStreamSchedulePlaylistMetadata"))
        : nullptr;
    if (schedule)
    {
        for (const PlaylistItem& item : replacementItems)
            schedule(item.path.c_str());
        DebugLogWrite("INFO", "GEN", "BACKGROUND_METADATA_SCHEDULED", "archive=%s | entries=%zu", archivePath, replacementItems.size());
    }
    else
    {
        DebugLogWrite("WARN", "GEN", "BACKGROUND_METADATA_UNAVAILABLE", "archive=%s | reason=input_plugin_export_missing", archivePath);
    }

    return true;
}

static int FindPlaylistIndexByPath(HWND hwndWinamp, const char* wantedPath)
{
    if (!hwndWinamp || !wantedPath || !wantedPath[0])
    {
        DebugLogWrite("WARN", "GEN", "FIND_PLAYLIST_PATH_FAIL", "path=%s | reason=invalid_input", wantedPath ? wantedPath : "");
        return -1;
    }

    int playlistLength = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);

    if (playlistLength <= 0)
    {
        DebugLogWrite("WARN", "GEN", "FIND_PLAYLIST_PATH_FAIL", "path=%s | playlist_length=%d | reason=empty_playlist", wantedPath, playlistLength);
        return -1;
    }

    for (int i = 0; i < playlistLength; ++i)
    {
        char path[4096];

        if (!GetPlaylistItem(hwndWinamp, i, path, sizeof(path)))
            continue;

        if (_stricmp(path, wantedPath) == 0)
            return i;
    }

    DebugLogWrite("WARN", "GEN", "FIND_PLAYLIST_PATH_FAIL", "path=%s | playlist_length=%d | reason=not_found", wantedPath, playlistLength);
    return -1;
}

bool ExpandSelectedOrCurrentZip(HWND hwndWinamp)
{
    int targetIndex = GetTargetPlaylistIndex(hwndWinamp);

    if (targetIndex < 0)
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_SELECTED_FAIL", "target_index=%d | reason=no_selected_or_current_item", targetIndex);
        return false;
    }

    return RebuildPlaylistWithExpandedZip(hwndWinamp, targetIndex, false);
}

static bool ExpandArchiveAtIndexInternal(HWND hwndWinamp, const char* zipPath, int preferredIndex, bool startPlayback, const std::vector<PlaylistItem>* preparedItems)
{
    if (!hwndWinamp || !zipPath || !zipPath[0])
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_PATH_FAIL", "archive=%s | index=%d | reason=invalid_input", zipPath ? zipPath : "", preferredIndex);
        return false;
    }

    int targetIndex = -1;

    if (preferredIndex >= 0)
    {
        char path[4096];

        if (GetPlaylistItem(hwndWinamp, preferredIndex, path, sizeof(path)) && _stricmp(path, zipPath) == 0)
        {
            targetIndex = preferredIndex;
        }
        else
        {
            DebugLogWrite("WARN", "GEN", "EXPAND_INDEX_MISMATCH", "archive=%s | preferred_index=%d | reason=item_at_index_did_not_match", zipPath, preferredIndex);
        }
    }

    if (targetIndex < 0)
        targetIndex = FindPlaylistIndexByPath(hwndWinamp, zipPath);

    DebugLogWrite("INFO", "GEN", "EXPAND_TARGET_SELECTED", "archive=%s | preferred_index=%d | target_index=%d", zipPath, preferredIndex, targetIndex);

    if (targetIndex < 0)
    {
        DebugLogWrite("WARN", "GEN", "EXPAND_PATH_FAIL", "archive=%s | preferred_index=%d | reason=playlist_item_not_found", zipPath, preferredIndex);
        return false;
    }

    return RebuildPlaylistWithExpandedZip(hwndWinamp, targetIndex, startPlayback, preparedItems);
}

bool ExpandZipPathInPlaylistAtIndex(HWND hwndWinamp, const char* zipPath, int preferredIndex, bool startPlayback)
{
    return ExpandArchiveAtIndexInternal(hwndWinamp, zipPath, preferredIndex, startPlayback, nullptr);
}

bool ExpandPreparedArchiveAtIndex(HWND hwndWinamp, const char* archivePath, int preferredIndex, const std::vector<ArchivePreparedPlaylistItem>& preparedItems, bool startPlayback)
{
    return ExpandArchiveAtIndexInternal(hwndWinamp, archivePath, preferredIndex, startPlayback, &preparedItems);
}

bool ExpandZipPathInPlaylist(HWND hwndWinamp, const char* zipPath, bool startPlayback)
{
    return ExpandZipPathInPlaylistAtIndex(hwndWinamp, zipPath, -1, startPlayback);
}