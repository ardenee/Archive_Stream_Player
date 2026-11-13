#include <windows.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>

#include "wa_ipc.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "archive_metadata.h"
#include "debug_log.h"

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

struct PlaylistItem {
    std::string path;
    std::string title;
    int lengthMs = -1000;
    bool useMeta = false;
};

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
    std::string filename = StripExtensionFromFileName(entryName);
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

static bool BuildReplacementItemsForArchive(HWND hwndWinamp, const char* archivePath, std::vector<PlaylistItem>& outItems, std::string& outFirstVirtualPath)
{
    outItems.clear();
    outFirstVirtualPath.clear();

    if (!hwndWinamp || !archivePath || !archivePath[0])
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

    if (!EnqueueFilePlainToWinamp(hwndWinamp, item.path.c_str()))
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

    DebugLogWrite("INFO", "GEN", "ENQUEUE_ITEM", "path=%s | title=%s | scanned_length_ms=%d | enqueue_mode=plain", item.path.c_str(), item.title.c_str(), item.lengthMs);
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


static bool RebuildPlaylistWithExpandedZip(HWND hwndWinamp, int targetIndex, bool startPlayback)
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

    if (!BuildReplacementItemsForArchive(hwndWinamp, archivePath, replacementItems, firstVirtualPath))
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
    SendMessage(hwndWinamp, WM_WA_IPC, firstInsertedIndex, IPC_SETPLAYLISTPOS);
    DebugLogWrite("INFO", "GEN", "EXPAND_REBUILD_DONE", "target_index=%d | first_inserted_index=%d | replacement_count=%zu | start_playback=%d | first_virtual=%s", targetIndex, firstInsertedIndex, replacementItems.size(), startPlayback ? 1 : 0, firstVirtualPath.c_str());

    if (startPlayback)
    {
        if (!QueuePlayExpandedIndex(hwndWinamp, firstInsertedIndex, firstVirtualPath.c_str())) DebugLogWrite("ERROR", "GEN", "EXPAND_PLAY_FAIL", "target_index=%d | reason=queue_play_failed", targetIndex);
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

bool ExpandZipPathInPlaylistAtIndex(HWND hwndWinamp, const char* zipPath, int preferredIndex, bool startPlayback)
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

    return RebuildPlaylistWithExpandedZip(hwndWinamp, targetIndex, startPlayback);
}

bool ExpandZipPathInPlaylist(HWND hwndWinamp, const char* zipPath, bool startPlayback)
{
    return ExpandZipPathInPlaylistAtIndex(hwndWinamp, zipPath, -1, startPlayback);
}