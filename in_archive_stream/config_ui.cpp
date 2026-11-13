#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "plugin_state.h"
#include "config_ui.h"
#include "archive_cache.h"
#include "archive_rar_reader.h"
#include "debug_log.h"
#include "archive_passwords.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_unified_cache.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef WM_WA_IPC
#define WM_WA_IPC WM_USER
#endif

#ifndef IPC_ENQUEUEFILE
#define IPC_ENQUEUEFILE 100
#endif

#ifndef IPC_GETLISTLENGTH
#define IPC_GETLISTLENGTH 124
#endif

#ifndef IPC_GETPLAYLISTFILE
#define IPC_GETPLAYLISTFILE 211
#endif

#ifndef IPC_REFRESHPLCACHE
#define IPC_REFRESHPLCACHE 247
#endif

#ifndef IPC_GETWND
#define IPC_GETWND 260
#endif

#ifndef IPC_GETWND_PE
#define IPC_GETWND_PE 1
#endif

#ifndef IPC_PE_DELETEINDEX
#define IPC_PE_DELETEINDEX 104
#endif

#ifndef IPC_PE_SWAPINDEX
#define IPC_PE_SWAPINDEX 105
#endif

#ifndef IPC_GETLISTLENGTH
#define IPC_GETLISTLENGTH 124
#endif

#ifndef IPC_GETPLAYLISTFILE
#define IPC_GETPLAYLISTFILE 211
#endif

#ifndef IPC_REFRESHPLCACHE
#define IPC_REFRESHPLCACHE 247
#endif

#ifndef IPC_GETWND
#define IPC_GETWND 260
#endif

#ifndef IPC_GETWND_PE
#define IPC_GETWND_PE 1
#endif

#define IDC_BUFFER_EDIT 1001
#define IDC_SAVE_BUTTON 1002
#define IDC_CANCEL_BUTTON 1003
#define IDC_CACHE_EDIT 1004
#define IDC_CLEAR_CACHE_BUTTON 1005
#define IDC_VIEW_CACHE_BUTTON 1006
#define IDC_CACHE_SIZE_CLEANUP_CHECK 1007
#define IDC_CACHE_AGE_CLEANUP_CHECK  1008
#define IDC_CACHE_AGE_EDIT           1009
#define IDC_DISPLAY_PREFIX_EDIT      1010
#define IDC_DISPLAY_POSTFIX_EDIT     1011
#define IDC_DISPLAY_TEMPLATE_EDIT    1012
#define IDC_DISPLAY_PRESET_COMBO     1013
#define IDC_PASSWORD_LIST_EDIT       1014
#define IDC_PASSWORD_LIST_BROWSE     1015
#define IDC_DEBUG_LOGGING_CHECK      1016
#define IDC_DEBUG_LOG_PATH_EDIT      1017
#define IDC_OPEN_LOG_FOLDER_BUTTON   1018

#define IDC_ABOUT_TEXT          9101
#define IDC_ABOUT_ARDEN_BUTTON  9102
#define IDC_ABOUT_CLOSE_BUTTON  9103

static HWND g_cacheSizeCleanupCheck = NULL;
static HWND g_cacheAgeCleanupCheck = NULL;
static HWND g_cacheAgeEdit = NULL;
static HWND g_displayPrefixEdit = NULL;
static HWND g_displayPostfixEdit = NULL;
static HWND g_displayTemplateEdit = NULL;
static HWND g_displayPresetCombo = NULL;
static HWND g_passwordListEdit = NULL;
static HWND g_debugLoggingCheck = NULL;
static HWND g_debugLogPathEdit = NULL;

static size_t ClampBufferMb(size_t mb)
{
    if (mb < MIN_BUFFER_SIZE_MB)
        return MIN_BUFFER_SIZE_MB;

    if (mb > MAX_BUFFER_SIZE_MB)
        return MAX_BUFFER_SIZE_MB;

    return mb;
}

static size_t ClampCacheMb(size_t mb)
{
    if (mb < MIN_CACHE_SIZE_MB)
        return MIN_CACHE_SIZE_MB;

    if (mb > MAX_CACHE_SIZE_MB)
        return MAX_CACHE_SIZE_MB;

    return mb;
}

static unsigned int ClampCacheAgeDays(unsigned int days)
{
    if (days < MIN_CACHE_AGE_DAYS)
        return MIN_CACHE_AGE_DAYS;

    if (days > MAX_CACHE_AGE_DAYS)
        return MAX_CACHE_AGE_DAYS;

    return days;
}

static void BuildIniPath()
{
    g_iniPath[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("in_archive_stream.ini", g_iniPath, sizeof(g_iniPath)))
    {
        DebugLogWrite("ERROR", "CONFIG", "INI_PATH_FAIL", "reason=plugin_data_file_path_failed");
        return;
    }
}

static void EncodeConfigText(const char* input, char* output, size_t outputSize)
{
    if (!output || outputSize == 0) 
        return;

    output[0] = '\0';

    if (!input) 
        return;

    size_t outPos = 0;

    for (size_t i = 0; input[i] && outPos + 1 < outputSize; ++i)
    {
        if (input[i] == ' ' && outPos + 3 < outputSize)
        {
            output[outPos++] = '%';
            output[outPos++] = '2';
            output[outPos++] = '0';
        }
        else if (input[i] == '%' && outPos + 3 < outputSize)
        {
            output[outPos++] = '%';
            output[outPos++] = '2';
            output[outPos++] = '5';
        }
        else
        {
            output[outPos++] = input[i];
        }
    }

    output[outPos] = '\0';
}

static void DecodeConfigText(const char* input, char* output, size_t outputSize)
{
    if (!output || outputSize == 0) 
        return;

    output[0] = '\0';

    if (!input) 
        return;

    size_t outPos = 0;

    for (size_t i = 0; input[i] && outPos + 1 < outputSize; ++i)
    {
        if (input[i] == '%' && input[i + 1] == '2' && input[i + 2] == '0')
        {
            output[outPos++] = ' ';
            i += 2;
        }
        else if (input[i] == '%' && input[i + 1] == '2' && input[i + 2] == '5')
        {
            output[outPos++] = '%';
            i += 2;
        }
        else
        {
            output[outPos++] = input[i];
        }
    }

    output[outPos] = '\0';
}

static void BuildDefaultPasswordListPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        return;
    }

    outPath[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("keeps.txt", outPath, outPathSize))
    {
        DebugLogWrite("ERROR", "CONFIG", "PASSWORD_PATH_FAIL", "reason=plugin_data_file_path_failed");
        return;
    }
}

static void BuildDefaultDebugLogPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        return;
    }

    outPath[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("in_archive_stream.log", outPath, outPathSize))
    {
        DebugLogWrite("ERROR", "CONFIG", "DEBUG_LOG_PATH_FAIL", "reason=plugin_data_file_path_failed");
        return;
    }
}

static bool OpenFolderForPath(HWND hwnd, const char* filePath)
{
    if (!filePath || !filePath[0])
        return false;

    char folder[MAX_PATH * 4];
    strcpy_s(folder, sizeof(folder), filePath);

    char* slash = strrchr(folder, '\\');

    if (slash)
        *slash = '\0';

    if (!folder[0])
        return false;

    HINSTANCE result = ShellExecuteA(hwnd, "open", folder, NULL, NULL, SW_SHOWNORMAL);

    return (INT_PTR)result > 32;
}

// -----------------------------------------------------------------------------
// IsVirtualArchivePlaylistPath
//
// Returns true when a playlist item is one of our expanded virtual archive
// entries.
//
// Display-template changes only need to touch these entries. Plain files and
// unexpanded archive rows are left alone.
// -----------------------------------------------------------------------------
static bool IsVirtualArchivePlaylistPath(const char* path)
{
    return path && strstr(path, ".archive_stream|") != NULL;
}

// -----------------------------------------------------------------------------
// GetPlaylistEditorWindow
//
// Returns Winamp's playlist editor window.
//
// The playlist editor window is needed for index-level playlist operations such
// as delete and swap. These are the same operations the general plugin already
// uses when expanding an archive in-place.
// -----------------------------------------------------------------------------
static HWND GetPlaylistEditorWindow(HWND hwndWinamp)
{
    if (!hwndWinamp)
    {
        return NULL;
    }

    return (HWND)SendMessage(hwndWinamp, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND);
}

// -----------------------------------------------------------------------------
// DeletePlaylistIndex
//
// Deletes one playlist row by index through the playlist editor.
//
// This is used during display-template refresh to remove a stale virtual row
// before re-enqueuing the same virtual path. Re-enqueueing forces Winamp to call
// GetFileInfo() again and rebuild the visible title.
// -----------------------------------------------------------------------------
static bool DeletePlaylistIndex(HWND hwndWinamp, int index)
{
    HWND hwndPE = GetPlaylistEditorWindow(hwndWinamp);

    if (!hwndPE || index < 0)
    {
        DebugLogWrite("WARN", "UI", "DISPLAY_REFRESH_DELETE_SKIP", "index=%d | reason=invalid_playlist_editor_or_index", index);
        return false;
    }

    SendMessage(hwndPE, WM_USER, IPC_PE_DELETEINDEX, index);
    return true;
}

// -----------------------------------------------------------------------------
// SwapPlaylistIndexes
//
// Swaps two playlist rows through the playlist editor.
//
// Newly enqueued items are appended at the end of the playlist. This helper
// moves each re-enqueued virtual path back to its original position.
// -----------------------------------------------------------------------------
static bool SwapPlaylistIndexes(HWND hwndWinamp, int indexA, int indexB)
{
    HWND hwndPE = GetPlaylistEditorWindow(hwndWinamp);

    if (!hwndPE || indexA < 0 || indexB < 0)
    {
        DebugLogWrite("WARN", "UI", "DISPLAY_REFRESH_SWAP_SKIP", "index_a=%d | index_b=%d | reason=invalid_playlist_editor_or_index", indexA, indexB);
        return false;
    }

    LPARAM swapParam = (LPARAM)(indexA + (indexB << 16));
    SendMessage(hwndPE, WM_USER, IPC_PE_SWAPINDEX, swapParam);

    return true;
}

// -----------------------------------------------------------------------------
// MovePlaylistItem
//
// Moves one playlist row to another index by repeated adjacent swaps.
//
// This avoids rebuilding the entire playlist and preserves all non-archive rows.
// -----------------------------------------------------------------------------
static bool MovePlaylistItem(HWND hwndWinamp, int fromIndex, int toIndex)
{
    if (fromIndex == toIndex)
    {
        return true;
    }

    if (fromIndex < 0 || toIndex < 0)
    {
        return false;
    }

    if (fromIndex > toIndex)
    {
        for (int i = fromIndex; i > toIndex; --i)
        {
            if (!SwapPlaylistIndexes(hwndWinamp, i, i - 1))
            {
                return false;
            }
        }

        return true;
    }

    for (int i = fromIndex; i < toIndex; ++i)
    {
        if (!SwapPlaylistIndexes(hwndWinamp, i, i + 1))
        {
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// EnqueueFilePlainToWinamp
//
// Appends a file/path to Winamp's playlist.
//
// For virtual archive paths, this causes Winamp to query the input plugin again,
// which rebuilds the visible playlist title using the current display template.
// -----------------------------------------------------------------------------
static bool EnqueueFilePlainToWinamp(HWND hwndWinamp, const char* path)
{
    if (!hwndWinamp || !path || !path[0])
    {
        return false;
    }

    COPYDATASTRUCT cds;
    memset(&cds, 0, sizeof(cds));

    cds.dwData = IPC_ENQUEUEFILE;
    cds.lpData = (void*)path;
    cds.cbData = (DWORD)strlen(path) + 1;

    LRESULT result = SendMessage(hwndWinamp, WM_COPYDATA, 0, (LPARAM)&cds);

    if (!result)
    {
        DebugLogWrite("WARN", "UI", "DISPLAY_REFRESH_ENQUEUE_FAIL", "path=%s", path);
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// RefreshPlaylistCacheForPath
//
// Refreshes Winamp's cached title for one specific virtual archive path.
//
// This is still called after re-enqueueing, but it is no longer relied on as the
// only refresh method because existing playlist rows did not visibly update from
// IPC_REFRESHPLCACHE alone.
// -----------------------------------------------------------------------------
static void RefreshPlaylistCacheForPath(HWND hwndWinamp, const char* path)
{
    if (!hwndWinamp || !path || !path[0])
    {
        return;
    }

    wchar_t widePath[4096];
    widePath[0] = L'\0';

    int converted = MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, (int)(sizeof(widePath) / sizeof(widePath[0])));

    if (converted <= 0 || widePath[0] == L'\0')
    {
        DebugLogWrite("WARN", "UI", "DISPLAY_REFRESH_PATH_SKIP", "path=%s | reason=path_wide_conversion_failed | winerr=%lu", path, GetLastError());
        return;
    }

    SendMessage(hwndWinamp, WM_WA_IPC, (WPARAM)widePath, IPC_REFRESHPLCACHE);
}

// -----------------------------------------------------------------------------
// RefreshPlaylistTitlesAfterDisplayConfigChange
//
// Do not rewrite Winamp playlist rows from the input-plugin config UI.
//
// The general plugin is responsible for creating playlist entries during archive
// expansion. Existing entries do not need to be deleted/re-enqueued from here.
// New expansions will use the updated display template once the general plugin
// reads the same config file as the input plugin.
// -----------------------------------------------------------------------------
static void RefreshPlaylistTitlesAfterDisplayConfigChange()
{
    return;
}

void LoadConfig()
{
    BuildIniPath();
    UINT bufferMb = GetPrivateProfileIntA("ArchiveStream", "BufferSizeMB", (UINT)DEFAULT_BUFFER_SIZE_MB, g_iniPath);
    UINT cacheMb = GetPrivateProfileIntA("ArchiveStream", "CacheSizeMB", (UINT)DEFAULT_CACHE_SIZE_MB, g_iniPath);
    UINT cacheSizeCleanupEnabled = GetPrivateProfileIntA("ArchiveStream", "CacheSizeCleanupEnabled", 1, g_iniPath);
    UINT cacheAgeCleanupEnabled = GetPrivateProfileIntA("ArchiveStream", "CacheAgeCleanupEnabled", 1, g_iniPath);
    UINT cacheMaxAgeDays = GetPrivateProfileIntA( "ArchiveStream", "CacheMaxAgeDays", DEFAULT_CACHE_AGE_DAYS,g_iniPath);

    char encodedPrefix[64];
    char encodedPostfix[64];
    char encodedTemplate[256];
    GetPrivateProfileStringA("ArchiveStream", "DisplayPrefix", "", encodedPrefix, sizeof(encodedPrefix), g_iniPath);
    GetPrivateProfileStringA("ArchiveStream", "DisplayPostfix", "", encodedPostfix, sizeof(encodedPostfix), g_iniPath);
    GetPrivateProfileStringA("ArchiveStream", "DisplayTemplate", "", encodedTemplate, sizeof(encodedTemplate), g_iniPath);
    DecodeConfigText(encodedPrefix, g_displayPrefix, sizeof(g_displayPrefix));
    DecodeConfigText(encodedPostfix, g_displayPostfix, sizeof(g_displayPostfix));
    DecodeConfigText(encodedTemplate, g_displayTemplate, sizeof(g_displayTemplate));
    /*
        Prefix/postfix are limited to 20 visible characters.

        The backing buffers are 32 bytes so the terminating null and any future small
        expansion do not risk truncating at the buffer edge.
    */
    g_displayPrefix[20] = '\0';
    g_displayPostfix[20] = '\0';

    if (!g_displayTemplate[0])
        strcpy_s(g_displayTemplate, sizeof(g_displayTemplate), "<filename>");

    GetPrivateProfileStringA( "ArchiveStream", "PasswordListPath","", g_rarPasswordListPath, sizeof(g_rarPasswordListPath), g_iniPath);

    if (!g_rarPasswordListPath[0])
    {
        BuildDefaultPasswordListPath(
            g_rarPasswordListPath,
            sizeof(g_rarPasswordListPath)
        );
    }

    UINT debugLoggingEnabled = GetPrivateProfileIntA("ArchiveStream","DebugLoggingEnabled", 0, g_iniPath);
    GetPrivateProfileStringA( "ArchiveStream", "DebugLogPath", "",  g_debugLogPath, sizeof(g_debugLogPath),g_iniPath);

    if (!g_debugLogPath[0])
    {
        BuildDefaultDebugLogPath(g_debugLogPath, sizeof(g_debugLogPath) );
    }

    g_debugLoggingEnabled = debugLoggingEnabled ? true : false;
    g_configuredBufferSizeMb = ClampBufferMb((size_t)bufferMb);
    g_audioBufferSize = g_configuredBufferSizeMb * 1024 * 1024;
    g_configuredCacheSizeMb = ClampCacheMb((size_t)cacheMb);
    g_cacheSizeCleanupEnabled = cacheSizeCleanupEnabled ? true : false;
    g_cacheAgeCleanupEnabled = cacheAgeCleanupEnabled ? true : false;
    g_cacheMaxAgeDays = ClampCacheAgeDays((unsigned int)cacheMaxAgeDays);

    if (ArchiveAdapterPreferencesConfigured())
    {
        const auto shared = ArchiveAdapterPreferencesLoad();
        g_configuredBufferSizeMb = ClampBufferMb(shared.memoryMb);
        g_audioBufferSize = g_configuredBufferSizeMb * 1024 * 1024;
        g_configuredCacheSizeMb = ClampCacheMb(shared.cacheLimitMb);
        g_cacheMaxAgeDays = ClampCacheAgeDays(shared.cacheAgeDays);

        const std::string passwordUtf8 = ArchiveAdapterWideToUtf8(shared.passwordListPath);
        if (!passwordUtf8.empty() && passwordUtf8.size() < sizeof(g_rarPasswordListPath))
            strcpy_s(g_rarPasswordListPath, sizeof(g_rarPasswordListPath), passwordUtf8.c_str());

        const std::string titleUtf8 = ArchiveAdapterWideToUtf8(shared.titleTemplate);
        if (!titleUtf8.empty() && titleUtf8.size() < sizeof(g_displayTemplate))
            strcpy_s(g_displayTemplate, sizeof(g_displayTemplate), titleUtf8.c_str());
    }

    RarSetPasswordListPath(g_rarPasswordListPath);
    ArchivePasswordSetListPath(g_rarPasswordListPath);
}

void SaveConfig()
{
    if (!g_iniPath[0])
        BuildIniPath();

    if (!g_iniPath[0])
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "reason=missing_ini_path");
        return;
    }

    char value[32];
    sprintf_s(value,sizeof(value), "%zu", g_configuredBufferSizeMb );

    if (!WritePrivateProfileStringA("ArchiveStream", "BufferSizeMB", value, g_iniPath))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=BufferSizeMB | winerr=%lu", GetLastError());
    }

    sprintf_s(value, sizeof(value), "%zu",g_configuredCacheSizeMb );

    if (!WritePrivateProfileStringA("ArchiveStream", "CacheSizeMB", value, g_iniPath ))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=CacheSizeMB | winerr=%lu", GetLastError());
    }

    if (!WritePrivateProfileStringA("ArchiveStream", "CacheSizeCleanupEnabled", g_cacheSizeCleanupEnabled ? "1" : "0", g_iniPath ))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=CacheSizeCleanupEnabled | winerr=%lu", GetLastError());
    }

    if (!WritePrivateProfileStringA("ArchiveStream", "CacheAgeCleanupEnabled", g_cacheAgeCleanupEnabled ? "1" : "0", g_iniPath))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=CacheAgeCleanupEnabled | winerr=%lu", GetLastError());
    }

    sprintf_s(value, sizeof(value), "%u", g_cacheMaxAgeDays);

    if (!WritePrivateProfileStringA("ArchiveStream", "CacheMaxAgeDays", value, g_iniPath))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=CacheMaxAgeDays | winerr=%lu", GetLastError());
    }

    char encodedPrefix[64];
    char encodedPostfix[64];
    char encodedTemplate[256];
    EncodeConfigText(g_displayPrefix, encodedPrefix, sizeof(encodedPrefix));
    EncodeConfigText(g_displayPostfix, encodedPostfix, sizeof(encodedPostfix));
    EncodeConfigText(g_displayTemplate[0] ? g_displayTemplate : "", encodedTemplate, sizeof(encodedTemplate));

    if (!WritePrivateProfileStringA("ArchiveStream", "DisplayPrefix", encodedPrefix, g_iniPath)) 
    { 
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=DisplayPrefix | winerr=%lu", GetLastError()); 
    }

    if (!WritePrivateProfileStringA("ArchiveStream", "DisplayPostfix", encodedPostfix, g_iniPath)) 
    { 
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=DisplayPostfix | winerr=%lu", GetLastError()); 
    }

    if (!WritePrivateProfileStringA("ArchiveStream", "DisplayTemplate", encodedTemplate, g_iniPath)) 
    { 
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=DisplayTemplate | winerr=%lu", GetLastError()); 
    }

    if (!g_rarPasswordListPath[0])
    {
        BuildDefaultPasswordListPath(g_rarPasswordListPath,sizeof(g_rarPasswordListPath));
    }

    if (!g_debugLogPath[0])
    {
        BuildDefaultDebugLogPath( g_debugLogPath,sizeof(g_debugLogPath));
    }

    if (!WritePrivateProfileStringA("ArchiveStream","DebugLoggingEnabled",g_debugLoggingEnabled ? "1" : "0",g_iniPath))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=DebugLoggingEnabled | winerr=%lu", GetLastError());
    }

    if (!WritePrivateProfileStringA("ArchiveStream","DebugLogPath",g_debugLogPath,g_iniPath))
    {
        DebugLogWrite("ERROR", "CONFIG", "SAVE_FAIL", "key=DebugLogPath | winerr=%lu", GetLastError());
    }

    /*
        Create the file if it does not exist yet.
        Do not overwrite existing contents.
    */
    if (g_rarPasswordListPath[0])
    {
        DWORD attr = GetFileAttributesA(g_rarPasswordListPath);

        if (attr == INVALID_FILE_ATTRIBUTES)
        {
            HANDLE h = CreateFileA(g_rarPasswordListPath,GENERIC_WRITE, FILE_SHARE_READ,NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

            if (h != INVALID_HANDLE_VALUE)
            {
                CloseHandle(h);
            }
            else
            {
                DebugLogWrite("WARN", "CONFIG", "PASSWORD_FILE_CREATE_FAIL", "path=%s | winerr=%lu", g_rarPasswordListPath, GetLastError());
            }
        }
    }

    RarSetPasswordListPath(g_rarPasswordListPath);
    ArchivePasswordSetListPath(g_rarPasswordListPath);

    auto common = ArchiveAdapterPreferencesLoad();
    common.memoryMb = static_cast<unsigned>(g_configuredBufferSizeMb);
    common.cacheLimitMb = static_cast<unsigned>(g_configuredCacheSizeMb);
    common.cacheAgeDays = g_cacheMaxAgeDays;
    common.passwordListPath = ArchiveAdapterUtf8ToWide(g_rarPasswordListPath);
    if (!ArchiveAdapterPreferencesSave(common))
        DebugLogWrite("WARN", "CONFIG", "SHARED_SAVE_FAIL", "source=winamp");
}

void ReallocateAudioBuffer(size_t newSizeMb)
{
    newSizeMb = ClampBufferMb(newSizeMb);
    size_t newSizeBytes = newSizeMb * 1024 * 1024;
    unsigned char* newBuffer = (unsigned char*)malloc(newSizeBytes);

    if (!newBuffer)
    {
        DebugLogWrite("ERROR", "CONFIG", "BUFFER_ALLOC_FAIL", "requested_mb=%zu | requested_bytes=%zu", newSizeMb, newSizeBytes);

        MessageBoxA(
            g_mod.hMainWindow,
            "Could not allocate the requested archive buffer size.",
            "Archive Stream - Buffer Error",
            MB_OK | MB_ICONERROR
        );

        return;
    }

    Stop();

    if (g_audioBuffer)
        free(g_audioBuffer);

    g_audioBuffer = newBuffer;
    g_audioBufferSize = newSizeBytes;
    g_configuredBufferSizeMb = newSizeMb;

    SaveConfig();
}

static void SetCacheSize(size_t newSizeMb)
{
    g_configuredCacheSizeMb = ClampCacheMb(newSizeMb);
    SaveConfig();
}

static void SetCacheCleanupOptions(bool sizeCleanupEnabled, bool ageCleanupEnabled, unsigned int maxAgeDays)
{
    g_cacheSizeCleanupEnabled = sizeCleanupEnabled;
    g_cacheAgeCleanupEnabled = ageCleanupEnabled;
    g_cacheMaxAgeDays = ClampCacheAgeDays(maxAgeDays);

    SaveConfig();
}

static void SetWindowTextFormat(HWND hwnd, const char* fmt, ...)
{
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    SetWindowTextA(hwnd, buffer);
}

static LRESULT CALLBACK ConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            HWND hTitle = CreateWindowExA(0, "STATIC", "Archive Stream Player Settings", WS_CHILD | WS_VISIBLE, 16, 14, 360, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hTitle, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hBufferLabel = CreateWindowExA(0, "STATIC", "Archive buffer size (MB):", WS_CHILD | WS_VISIBLE, 16, 48, 170, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hBufferLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_bufferEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 195, 45, 90, 24, hwnd, (HMENU)IDC_BUFFER_EDIT, g_hInstance, NULL);
            SendMessage(g_bufferEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            char value[32];
            sprintf_s(value, sizeof(value), "%zu", g_configuredBufferSizeMb);
            SetWindowTextA(g_bufferEdit, value);

            HWND hBufferRange = CreateWindowExA(0, "STATIC", "Allowed range: 1 MB to 512 MB", WS_CHILD | WS_VISIBLE, 16, 78, 300, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hBufferRange, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hCacheSection = CreateWindowExA(0, "STATIC", "Managed cache", WS_CHILD | WS_VISIBLE, 16, 110, 170, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hCacheSection, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_cacheSizeCleanupCheck = CreateWindowExA(0, "BUTTON", "Auto-remove oldest files if cache exceeds:", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 138, 270, 22, hwnd, (HMENU)IDC_CACHE_SIZE_CLEANUP_CHECK, g_hInstance, NULL);
            SendMessage(g_cacheSizeCleanupCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_cacheSizeCleanupCheck, BM_SETCHECK, g_cacheSizeCleanupEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            g_cacheEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 292, 136, 90, 24, hwnd, (HMENU)IDC_CACHE_EDIT, g_hInstance, NULL);
            SendMessage(g_cacheEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            char value2[32];
            sprintf_s(value2, sizeof(value2), "%zu", g_configuredCacheSizeMb);
            SetWindowTextA(g_cacheEdit, value2);

            HWND hCacheMbLabel = CreateWindowExA(0, "STATIC", "MB", WS_CHILD | WS_VISIBLE, 390, 140, 40, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hCacheMbLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hCacheRange = CreateWindowExA(0, "STATIC", "Allowed range: 0 MB to 20480 MB. 0 disables size cleanup.", WS_CHILD | WS_VISIBLE, 36, 165, 430, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hCacheRange, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_cacheAgeCleanupCheck = CreateWindowExA(0, "BUTTON", "Auto-delete cache files older than:", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 195, 240, 22, hwnd, (HMENU)IDC_CACHE_AGE_CLEANUP_CHECK, g_hInstance, NULL);
            SendMessage(g_cacheAgeCleanupCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_cacheAgeCleanupCheck, BM_SETCHECK, g_cacheAgeCleanupEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            g_cacheAgeEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL, 260, 193, 60, 24, hwnd, (HMENU)IDC_CACHE_AGE_EDIT, g_hInstance, NULL);
            SendMessage(g_cacheAgeEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            sprintf_s(value, sizeof(value), "%u", g_cacheMaxAgeDays);
            SetWindowTextA(g_cacheAgeEdit, value);

            HWND hDaysLabel = CreateWindowExA(0, "STATIC", "days (1 to 365)", WS_CHILD | WS_VISIBLE, 330, 197, 120, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hDaysLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hViewCache = CreateWindowExA(0, "BUTTON", "View Cache", WS_CHILD | WS_VISIBLE, 16, 245, 105, 28, hwnd, (HMENU)IDC_VIEW_CACHE_BUTTON, g_hInstance, NULL);
            SendMessage(hViewCache, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hClearCache = CreateWindowExA(0, "BUTTON", "Clear Cache", WS_CHILD | WS_VISIBLE, 130, 245, 105, 28, hwnd, (HMENU)IDC_CLEAR_CACHE_BUTTON, g_hInstance, NULL);
            SendMessage(hClearCache, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hDisplaySection = CreateWindowExA(0, "STATIC", "Playlist display name", WS_CHILD | WS_VISIBLE, 16, 292, 200, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hDisplaySection, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hPrefixLabel = CreateWindowExA(0, "STATIC", "Prefix:", WS_CHILD | WS_VISIBLE, 16, 320, 55, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hPrefixLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_displayPrefixEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 75, 317, 80, 24, hwnd, (HMENU)IDC_DISPLAY_PREFIX_EDIT, g_hInstance, NULL);
            SendMessage(g_displayPrefixEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_displayPrefixEdit, EM_LIMITTEXT, 20, 0);
            SetWindowTextA(g_displayPrefixEdit, g_displayPrefix);

            HWND hPostfixLabel = CreateWindowExA(0, "STATIC", "Postfix:", WS_CHILD | WS_VISIBLE, 170, 320, 60, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hPostfixLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_displayPostfixEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 235, 317, 80, 24, hwnd, (HMENU)IDC_DISPLAY_POSTFIX_EDIT, g_hInstance, NULL);
            SendMessage(g_displayPostfixEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_displayPostfixEdit, EM_LIMITTEXT, 20, 0);
            SetWindowTextA(g_displayPostfixEdit, g_displayPostfix);

            HWND hPresetLabel = CreateWindowExA(0, "STATIC", "Preset:", WS_CHILD | WS_VISIBLE, 16, 352, 55, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hPresetLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_displayPresetCombo = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 90, 348, 380, 180, hwnd, (HMENU)IDC_DISPLAY_PRESET_COMBO, g_hInstance, NULL);
            SendMessage(g_displayPresetCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"<filename>");
            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"<archivetype>: <filename>");
            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"<#>. <filename>");
            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"<filename> [<filetype>]");
            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"<archivetype>: <#> - <filename>");
            SendMessageA(g_displayPresetCombo, CB_ADDSTRING, 0, (LPARAM)"[<archive>] <#> - <filename>");
            SendMessage(g_displayPresetCombo, CB_SETCURSEL, 0, 0);

            HWND hTemplateLabel = CreateWindowExA(0, "STATIC", "Template:", WS_CHILD | WS_VISIBLE, 16, 384, 70, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hTemplateLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_displayTemplateEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 90, 381, 380, 24, hwnd, (HMENU)IDC_DISPLAY_TEMPLATE_EDIT, g_hInstance, NULL);
            SendMessage(g_displayTemplateEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_displayTemplateEdit, EM_LIMITTEXT, 128, 0);
            SetWindowTextA(g_displayTemplateEdit, g_displayTemplate[0] ? g_displayTemplate : "<filename>");

            HWND hTokenHelp = CreateWindowExA(0, "STATIC", "Tokens: <archive> <archivetype> <filename> <filetype> <#>", WS_CHILD | WS_VISIBLE, 90, 410, 380, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hTokenHelp, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hPasswordLabel = CreateWindowExA(0, "STATIC", "Archive password file:", WS_CHILD | WS_VISIBLE, 16, 440, 120, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hPasswordLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_passwordListEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 135, 437, 280, 24, hwnd, (HMENU)IDC_PASSWORD_LIST_EDIT, g_hInstance, NULL);
            SendMessage(g_passwordListEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetWindowTextA(g_passwordListEdit, g_rarPasswordListPath);

            HWND hPasswordBrowse = CreateWindowExA(0, "BUTTON", "Browse...", WS_CHILD | WS_VISIBLE, 425, 436, 90, 26, hwnd, (HMENU)IDC_PASSWORD_LIST_BROWSE, g_hInstance, NULL);
            SendMessage(hPasswordBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hPasswordHelp = CreateWindowExA(0, "STATIC", "Text file format: one password per line. Blank lines and # comments ignored.", WS_CHILD | WS_VISIBLE, 135, 466, 390, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hPasswordHelp, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_debugLoggingCheck = CreateWindowExA(0, "BUTTON", "Enable debug logging", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 16, 492, 180, 22, hwnd, (HMENU)IDC_DEBUG_LOGGING_CHECK, g_hInstance, NULL);
            SendMessage(g_debugLoggingCheck, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(g_debugLoggingCheck, BM_SETCHECK, g_debugLoggingEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND hLogLabel = CreateWindowExA(0, "STATIC", "Log file:", WS_CHILD | WS_VISIBLE, 16, 522, 70, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hLogLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

            g_debugLogPathEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 90, 519, 330, 24, hwnd, (HMENU)IDC_DEBUG_LOG_PATH_EDIT, g_hInstance, NULL);
            SendMessage(g_debugLogPathEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            if (!g_debugLogPath[0])
                BuildDefaultDebugLogPath(g_debugLogPath, sizeof(g_debugLogPath));

            SetWindowTextA(g_debugLogPathEdit, g_debugLogPath);

            HWND hOpenLogFolder = CreateWindowExA(0, "BUTTON", "Open Log Folder", WS_CHILD | WS_VISIBLE, 430, 518, 110, 28, hwnd, (HMENU)IDC_OPEN_LOG_FOLDER_BUTTON, g_hInstance, NULL);
            SendMessage(hOpenLogFolder, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hAboutNote = CreateWindowExA(0, "STATIC", "Advanced settings and diagnostics are also available in this player.", WS_CHILD | WS_VISIBLE, 16, 560, 520, 20, hwnd, NULL, g_hInstance, NULL);
            SendMessage(hAboutNote, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hShared = CreateWindowExA(0, "BUTTON", "Shared settings (all players)...", WS_CHILD | WS_VISIBLE, 16, 592, 225, 28, hwnd, (HMENU)4501, g_hInstance, NULL);
            SendMessage(hShared, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

            HWND hSave = CreateWindowExA(0, "BUTTON", "Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 260, 600, 90, 28, hwnd, (HMENU)IDC_SAVE_BUTTON, g_hInstance, NULL);
            SendMessage(hSave, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hCancel = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 420, 600, 90, 28, hwnd, (HMENU)IDC_CANCEL_BUTTON, g_hInstance, NULL);
            SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDC_DISPLAY_PRESET_COMBO:
            {
                if (HIWORD(wParam) == CBN_SELCHANGE && g_displayPresetCombo && g_displayTemplateEdit)
                {
                    int sel = (int)SendMessage(g_displayPresetCombo, CB_GETCURSEL, 0, 0);
                    char preset[160];
                    preset[0] = '\0';

                    if (sel >= 0)
                    {
                        SendMessageA(g_displayPresetCombo, CB_GETLBTEXT, sel, (LPARAM)preset);

                        if (preset[0])
                            SetWindowTextA(g_displayTemplateEdit, preset);
                    }
                }

                return 0;
            }

            case 4501:
            {
                ArchiveAdapterOpenSettings(g_hInstance, hwnd);
                return 0;
            }

            case IDC_PASSWORD_LIST_BROWSE:
            {
                char path[MAX_PATH * 4];
                path[0] = '\0';

                if (g_passwordListEdit)
                    GetWindowTextA(g_passwordListEdit, path, sizeof(path));

                OPENFILENAMEA ofn;
                memset(&ofn, 0, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = path;
                ofn.nMaxFile = sizeof(path);
                ofn.lpstrFilter = "Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
                ofn.lpstrTitle = "Select Archive password list";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

                if (GetOpenFileNameA(&ofn))
                    SetWindowTextA(g_passwordListEdit, path);

                return 0;
            }

            case IDC_OPEN_LOG_FOLDER_BUTTON:
            {
                if (!g_debugLogPath[0])
                    BuildDefaultDebugLogPath(g_debugLogPath, sizeof(g_debugLogPath));

                if (!OpenFolderForPath(hwnd, g_debugLogPath))
                {
                    DebugLogWrite("ERROR", "UI", "OPEN_LOG_FOLDER_FAIL", "path=%s", g_debugLogPath);
                    MessageBoxA(hwnd, "Could not open the log folder.", "Archive Stream", MB_OK | MB_ICONERROR);
                }

                return 0;
            }

            case IDC_SAVE_BUTTON:
            {
                char text[64];
                GetWindowTextA(g_bufferEdit, text, sizeof(text));
                int requestedBufferMb = atoi(text);

                if (requestedBufferMb <= 0)
                {
                    MessageBoxA(hwnd, "Enter a valid archive buffer size in MB.", "Archive Stream", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                size_t clampedBufferMb = ClampBufferMb((size_t)requestedBufferMb);

                if ((size_t)requestedBufferMb != clampedBufferMb)
                {
                    char msg[256];
                    sprintf_s(msg, sizeof(msg), "Buffer size must be between %zu MB and %zu MB.", MIN_BUFFER_SIZE_MB, MAX_BUFFER_SIZE_MB);
                    MessageBoxA(hwnd, msg, "Archive Stream", MB_OK | MB_ICONWARNING);

                    char corrected[32];
                    sprintf_s(corrected, sizeof(corrected), "%zu", clampedBufferMb);
                    SetWindowTextA(g_bufferEdit, corrected);
                    SetFocus(g_bufferEdit);
                    return 0;
                }

                GetWindowTextA(g_cacheEdit, text, sizeof(text));
                int requestedCacheMb = atoi(text);

                if (requestedCacheMb < 0)
                {
                    MessageBoxA(hwnd, "Enter a valid max cache size in MB. Use 0 to disable automatic cleanup.", "Archive Stream", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                size_t clampedCacheMb = ClampCacheMb((size_t)requestedCacheMb);

                if ((size_t)requestedCacheMb != clampedCacheMb)
                {
                    char msg[256];
                    sprintf_s(msg, sizeof(msg), "Cache size must be between %zu MB and %zu MB.", MIN_CACHE_SIZE_MB, MAX_CACHE_SIZE_MB);
                    MessageBoxA(hwnd, msg, "Archive Stream", MB_OK | MB_ICONWARNING);

                    char corrected[32];
                    sprintf_s(corrected, sizeof(corrected), "%zu", clampedCacheMb);
                    SetWindowTextA(g_cacheEdit, corrected);
                    SetFocus(g_cacheEdit);
                    return 0;
                }

                GetWindowTextA(g_cacheAgeEdit, text, sizeof(text));
                int requestedAgeDays = atoi(text);

                if (requestedAgeDays <= 0)
                {
                    MessageBoxA(hwnd, "Enter a valid cache age in days.", "Archive Stream", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                unsigned int clampedAgeDays = ClampCacheAgeDays((unsigned int)requestedAgeDays);

                if ((unsigned int)requestedAgeDays != clampedAgeDays)
                {
                    char msg[256];
                    sprintf_s(msg, sizeof(msg), "Cache age must be between %u and %u days.", MIN_CACHE_AGE_DAYS, MAX_CACHE_AGE_DAYS);
                    MessageBoxA(hwnd, msg, "Archive Stream", MB_OK | MB_ICONWARNING);
                    char corrected[32];
                    sprintf_s(corrected, sizeof(corrected), "%u", clampedAgeDays);
                    SetWindowTextA(g_cacheAgeEdit, corrected);
                    SetFocus(g_cacheAgeEdit);
                    return 0;
                }

                bool sizeCleanupEnabled = SendMessage(g_cacheSizeCleanupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool ageCleanupEnabled = SendMessage(g_cacheAgeCleanupCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                char prefix[32];
                char postfix[32];
                char displayTemplate[160];
                char passwordListPath[MAX_PATH * 4];
                prefix[20] = '\0';
                postfix[20] = '\0';
                displayTemplate[0] = '\0';
                passwordListPath[0] = '\0';
                GetWindowTextA(g_displayPrefixEdit, prefix, sizeof(prefix));
                GetWindowTextA(g_displayPostfixEdit, postfix, sizeof(postfix));
                GetWindowTextA(g_displayTemplateEdit, displayTemplate, sizeof(displayTemplate));

                if (g_passwordListEdit)
                    GetWindowTextA(g_passwordListEdit, passwordListPath, sizeof(passwordListPath));

                prefix[20] = '\0';
                postfix[20] = '\0';
                strcpy_s(g_displayPrefix, sizeof(g_displayPrefix), prefix);
                strcpy_s(g_displayPostfix, sizeof(g_displayPostfix), postfix);

                if (displayTemplate[0])
                    strcpy_s(g_displayTemplate, sizeof(g_displayTemplate), displayTemplate);
                else
                    strcpy_s(g_displayTemplate, sizeof(g_displayTemplate), "<filename>");

                strcpy_s(g_rarPasswordListPath, sizeof(g_rarPasswordListPath), passwordListPath);
                RarSetPasswordListPath(g_rarPasswordListPath);
                bool debugLoggingEnabled = SendMessage(g_debugLoggingCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_debugLoggingEnabled = debugLoggingEnabled;

                if (!g_debugLogPath[0])
                    BuildDefaultDebugLogPath(g_debugLogPath, sizeof(g_debugLogPath));

                ReallocateAudioBuffer(clampedBufferMb);
                SetCacheSize(clampedCacheMb);
                SetCacheCleanupOptions(sizeCleanupEnabled, ageCleanupEnabled, clampedAgeDays);
                DebugLogConfigure(g_debugLoggingEnabled, g_debugLogPath, false);

                RefreshPlaylistTitlesAfterDisplayConfigChange();

                DestroyWindow(hwnd);

                return 0;
            }

            case IDC_CLEAR_CACHE_BUTTON:
            {
                int answer = MessageBoxA(hwnd, "Clear the shared managed cache (all players) and Winamp's older cache files? Active tracks may remain locked.", "Archive Stream - Clear Cache", MB_YESNO | MB_ICONQUESTION);

                if (answer != IDYES)
                    return 0;

                const bool sharedCleared = ArchiveUnifiedCache::Cleanup(0);
                const bool legacyCleared = ArchiveCacheClear();
                if (sharedCleared && legacyCleared)
                {
                    MessageBoxA(hwnd, "Shared and Winamp legacy caches cleared.", "Archive Stream", MB_OK | MB_ICONINFORMATION);
                }
                else
                {
                    DebugLogWrite("WARN", "UI", "CLEAR_CACHE", "result=partial_or_failed");
                    MessageBoxA(hwnd, "Some cache files could not be deleted. They may currently be in use.", "Archive Stream", MB_OK | MB_ICONWARNING);
                }

                return 0;
            }

            case IDC_VIEW_CACHE_BUTTON:
            {
                const std::wstring folder = ArchiveUnifiedCache::Folder();
                if (folder.empty())
                {
                    DebugLogWrite("ERROR", "UI", "VIEW_CACHE_FAIL", "reason=shared_cache_folder_failed");
                    MessageBoxA(hwnd, "Could not open the shared cache directory.", "Archive Stream", MB_OK | MB_ICONERROR);
                    return 0;
                }

                HINSTANCE result = ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((INT_PTR)result <= 32)
                {
                    DebugLogWrite("ERROR", "UI", "VIEW_CACHE_FAIL", "result=%ld", (long)(INT_PTR)result);
                    MessageBoxA(hwnd, "Could not open the shared cache folder in Explorer.", "Archive Stream", MB_OK | MB_ICONERROR);
                }

                return 0;
            }

            case IDC_CANCEL_BUTTON:
                DestroyWindow(hwnd);
                return 0;
            }

            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            g_configWnd = NULL;
            g_bufferEdit = NULL;
            g_cacheEdit = NULL;
            g_cacheSizeCleanupCheck = NULL;
            g_cacheAgeCleanupCheck = NULL;
            g_cacheAgeEdit = NULL;
            g_displayPrefixEdit = NULL;
            g_displayPostfixEdit = NULL;
            g_displayTemplateEdit = NULL;
            g_displayPresetCombo = NULL;
            g_passwordListEdit = NULL;
            g_debugLoggingCheck = NULL;
            g_debugLogPathEdit = NULL;
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void Config(HWND hwndParent)
{
    if (g_configWnd)
    {
        SetForegroundWindow(g_configWnd);
        return;
    }

    const char* className = "ArchiveStreamConfigWindowClass";
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ConfigWndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = className;

    if (!RegisterClassExA(&wc))
    {
        DWORD err = GetLastError();

        if (err != ERROR_CLASS_ALREADY_EXISTS)
            DebugLogWrite("WARN", "UI", "CONFIG_WINDOW_CLASS_FAIL", "winerr=%lu", err);
    }

    g_configWnd = CreateWindowExA(WS_EX_DLGMODALFRAME, className, "Archive Stream Player Settings", WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 560, 700, hwndParent, NULL, g_hInstance, NULL);

    if (!g_configWnd)
    {
        DebugLogWrite("ERROR", "UI", "CONFIG_WINDOW_CREATE_FAIL", "winerr=%lu", GetLastError());
        MessageBoxA(hwndParent, "Could not create configuration window.", "Archive Stream",MB_OK | MB_ICONERROR);

        return;
    }

    SetWindowTextA(g_configWnd, "Archive Stream Player Settings");

    RECT rc;
    GetWindowRect(g_configWnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    RECT parentRc;
    GetWindowRect(hwndParent, &parentRc);
    int x = parentRc.left + ((parentRc.right - parentRc.left) - width) / 2;
    int y = parentRc.top + ((parentRc.bottom - parentRc.top) - height) / 2;
    SetWindowPos(g_configWnd, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    EnableWindow(hwndParent, FALSE);
    MSG msg;

    while (g_configWnd && GetMessage(&msg, NULL, 0, 0))
    {
        if (!IsDialogMessage(g_configWnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(hwndParent, TRUE);
    SetForegroundWindow(hwndParent);
}

static const char* GetArchiveStreamAboutText()
{
    return
        "Archive Stream Player\r\n"
        "\r\n"
        "Purpose\r\n"
        "-------\r\n"
        "A Winamp plugin set that lets supported archives behave like playlists.\r\n"
        "Supported audio files can be expanded into virtual entries and played\r\n"
        "directly from ZIP, 7Z, and RAR archives where possible.\r\n"
        "\r\n"
        "Plugin components\r\n"
        "-----------------\r\n"
        "in_archive_stream.dll\r\n"
        "  Winamp input plugin.\r\n"
        "  Handles playback of virtual archive entries.\r\n"
        "\r\n"
        "gen_archive_stream.dll\r\n"
        "  Winamp general-purpose plugin.\r\n"
        "  Expands supported archives into playlist entries.\r\n"
        "\r\n"
        "Archive capabilities\r\n"
        "--------------------\r\n"
        "ZIP\r\n"
        "  - Small entries: memory playback\r\n"
        "  - Large entries: true streaming playback\r\n"
        "\r\n"
        "7Z\r\n"
        "  - Small entries: memory playback\r\n"
        "  - Large non-solid entries: streaming playback\r\n"
        "  - Solid / unsupported entries: managed cache fallback\r\n"
        "\r\n"
        "RAR\r\n"
        "  - Small entries: memory playback\r\n"
        "  - Large non-solid entries: progressive streaming playback\r\n"
        "  - Solid / encrypted / split / unsupported entries: cache fallback or clean failure\r\n"
        "\r\n"
        "Built-in audio support\r\n"
        "----------------------\r\n"
        "Native decoders\r\n"
        "  - MP3, WAV, FLAC via dr_libs\r\n"
        "  - OGG/Vorbis via stb_vorbis\r\n"
        "\r\n"
        "Windows Media Foundation\r\n"
        "  - M4A / AAC / WMA\r\n"
        "  - Docs: https://learn.microsoft.com/windows/win32/medfound/media-foundation-start-page\r\n"
        "\r\n"
        "Library sources\r\n"
        "---------------\r\n"
        "miniz\r\n"
        "  ZIP backend\r\n"
        "  https://github.com/richgel999/miniz\r\n"
        "\r\n"
        "LZMA SDK / 7-Zip SDK\r\n"
        "  7Z backend\r\n"
        "  https://www.7-zip.org/sdk.html\r\n"
        "\r\n"
        "Official UnRAR source / RARLAB\r\n"
        "  RAR backend\r\n"
        "  https://www.rarlab.com\r\n"
        "  License: https://www.rarlab.com/license.htm\r\n"
        "\r\n"
        "dr_libs\r\n"
        "  Native MP3 / WAV / FLAC decoding\r\n"
        "  https://github.com/mackron/dr_libs\r\n"
        "\r\n"
        "stb_vorbis\r\n"
        "  Native OGG/Vorbis decoding\r\n"
        "  https://github.com/nothings/stb\r\n"
        "\r\n"
        "Build / deployment notes\r\n"
        "------------------------\r\n"
        "No external 7z.dll is required.\r\n"
        "No external UnRAR DLL is required.\r\n"
        "No separate Winamp AAC/WMA/M4A input plugin is required.\r\n"
        "\r\n"
        "Microsoft Visual C++ Runtime\r\n"
        "  Built with the dynamic MSVC runtime (/MD).\r\n"
        "  Install the Microsoft Visual C++ Redistributable.\r\n"
        "  Because this is a Win32/x86 plugin, install the x86 redistributable.\r\n"
        "  https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\r\n"
        "\r\n"
        "Developer\r\n"
        "---------\r\n"
        "Developed by arden\r\n"
        "Website: https://arden.ie\r\n";
}

static LRESULT CALLBACK ArchiveAboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HWND hEdit = CreateWindowExA( WS_EX_CLIENTEDGE, "EDIT", GetArchiveStreamAboutText(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 12, 696, 438,  hwnd, (HMENU)IDC_ABOUT_TEXT, g_hInstance, NULL);
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hArden = CreateWindowExA(0, "BUTTON","Open arden.ie", WS_CHILD | WS_VISIBLE, 12, 462, 120, 28, hwnd, (HMENU)IDC_ABOUT_ARDEN_BUTTON, g_hInstance, NULL);
            SendMessage(hArden, WM_SETFONT, (WPARAM)hFont, TRUE);

            HWND hClose = CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 626, 462, 82, 28, hwnd, (HMENU)IDC_ABOUT_CLOSE_BUTTON, g_hInstance, NULL);
            SendMessage(hClose, WM_SETFONT, (WPARAM)hFont, TRUE);

            return 0;
        }

        case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
                case IDC_ABOUT_ARDEN_BUTTON:
                {
                    HINSTANCE result = ShellExecuteA(hwnd, "open", "https://arden.ie", NULL, NULL, SW_SHOWNORMAL);

                    if ((INT_PTR)result <= 32)
                        DebugLogWrite("ERROR", "UI", "ABOUT_LINK_FAIL", "url=https://arden.ie | result=%ld", (long)(INT_PTR)result);

                    return 0;
                }

                case IDC_ABOUT_CLOSE_BUTTON:
                    DestroyWindow(hwnd);
                    return 0;
            }

            break;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void ShowArchiveAboutWindow(HWND hwndParent)
{
    static const char* className = "ArchiveStreamAboutWindow";
    static bool classRegistered = false;

    if (!classRegistered)
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = ArchiveAboutWndProc;
        wc.hInstance = g_hInstance;
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);

        if (!RegisterClassA(&wc))
        {
            DWORD err = GetLastError();

            if (err != ERROR_CLASS_ALREADY_EXISTS)
                DebugLogWrite("WARN", "UI", "ABOUT_WINDOW_CLASS_FAIL", "winerr=%lu", err);
        }

        classRegistered = true;
    }

    HWND hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, className, "About Archive Stream Player", WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 732, 540, hwndParent, NULL, g_hInstance, NULL);

    if (!hwnd)
    {
        DebugLogWrite("ERROR", "UI", "ABOUT_WINDOW_CREATE_FAIL", "winerr=%lu", GetLastError());
        MessageBoxA(hwndParent, GetArchiveStreamAboutText(), "About Archive Stream Player", MB_OK | MB_ICONINFORMATION );

        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

void About(HWND hwndParent)
{
    ShowArchiveAboutWindow(hwndParent);
}