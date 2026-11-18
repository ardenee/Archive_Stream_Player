#include <windows.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <memory>
#include <new>
#include "archive_passwords.h"

#include "gen.h"
#include "wa_ipc.h"
#include "playlist_expand.h"
#include "debug_log.h"
#include "../shared/archive_adapter_logging.h"
#include "archive_common.h"
#include "archive_reader.h"
#include "winamp_bridge_protocol.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

static winampGeneralPurposePlugin g_plugin;

static char g_description[] = "Archive Stream Playlist Expander v0.1";

static HINSTANCE g_hInstance = NULL;
static HWND g_hiddenWnd = NULL;
#define WM_ARCHIVE_STREAM_DO_EXPAND (WM_APP + 101)
#define WM_ARCHIVE_STREAM_PLAY_EXPANDED_INDEX (WM_APP + 4202)
#define WM_ARCHIVE_STREAM_METADATA_READY (WM_APP + 4203)
static volatile LONG g_preflightGeneration = 0;
static bool IsWacupHost();
static bool QueueWacupMetadataPreflight(HWND hwnd, const std::string& archivePath, int preferredIndex, bool startPlayback);

#ifndef WINAMP_BUTTON2
#define WINAMP_BUTTON2 40045
#endif

static int g_pendingExpandedPlayIndex = -1;
static std::string g_pendingExpandedPlayPath;

struct PendingExpandRequest
{
    std::string archivePath;
    int playlistIndex = -1;
};

static PendingExpandRequest g_pendingExpand;

static void BuildInputPluginIniPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        // should add loging here
        return;
    }

    outPath[0] = '\0';
    ArchiveBuildPluginDataFilePath("in_archive_stream.ini", outPath, outPathSize);
}

static void BuildGeneralPluginLogPathFromInputLogPath(const char* inputLogPath, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return;

    outPath[0] = '\0';

    if (!inputLogPath || !inputLogPath[0])
        return;

    strcpy_s(outPath, outPathSize, inputLogPath);
    char* slash1 = strrchr(outPath, '\\');
    char* slash2 = strrchr(outPath, '/');
    char* slash = slash1 > slash2 ? slash1 : slash2;
    char* filename = slash ? slash + 1 : outPath;
    char* dot = strrchr(filename, '.');

    if (dot)
    {
        strcpy_s(dot, outPathSize - (dot - outPath), "_gen.log");
    }
    else
    {
        strcat_s(outPath, outPathSize, "_gen.log");
    }
}

static void BuildDefaultGeneralPluginLogPath(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        return;
    }

    outPath[0] = '\0';
    ArchiveBuildPluginDataFilePath("archive_stream_gen.log", outPath, outPathSize);
}

static void ConfigureGeneralPluginLogging()
{
    if (ArchiveAdapterPreferencesConfigured())
    {
        ArchiveAdapterLogging::ConfigureShared(L"winamp-gen");
        return;
    }

    char iniPath[MAX_PATH * 4];
    char inputLogPath[MAX_PATH * 4];
    char genLogPath[MAX_PATH * 4];
    iniPath[0] = '\0';
    inputLogPath[0] = '\0';
    genLogPath[0] = '\0';
    BuildInputPluginIniPath(iniPath, sizeof(iniPath));
    bool enabled = false;

    if (iniPath[0])
    {
        enabled = GetPrivateProfileIntA("ArchiveStream", "DebugLoggingEnabled", 0, iniPath) ? true : false;
        GetPrivateProfileStringA("ArchiveStream", "DebugLogPath", "", inputLogPath, sizeof(inputLogPath), iniPath);
    }

    if (inputLogPath[0])
        BuildGeneralPluginLogPathFromInputLogPath(inputLogPath, genLogPath, sizeof(genLogPath));

    if (!genLogPath[0])
        BuildDefaultGeneralPluginLogPath(genLogPath, sizeof(genLogPath));

    /*
        This is a separate DLL with separate static logger state.

        resetFile=true is okay here because this is a different file from
        the input plugin log.
    */
    DebugLogConfigure(enabled, genLogPath, true);
}

static bool GetPlaylistFileAtIndex(HWND hwndWinamp, int index, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
        return false;

    outPath[0] = '\0';

    if (!hwndWinamp || index < 0)
        return false;

    int playlistLength = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTLENGTH);

    if (index >= playlistLength)
        return false;

    const char* playlistPath = (const char*)SendMessage(hwndWinamp, WM_WA_IPC, index, IPC_GETPLAYLISTFILE);

    if (!playlistPath || !playlistPath[0])
        return false;

    strcpy_s(outPath, outPathSize, playlistPath);

    return true;
}

static bool IsArchiveStreamVirtualPathForArchive(const char* playlistPath, const char* archivePath)
{
    if (!playlistPath || !playlistPath[0] || !archivePath || !archivePath[0])
        return false;

    if (!strstr(playlistPath, ".archive_stream|") && strncmp(playlistPath, "archive_stream:", strlen("archive_stream:")) != 0)
        return false;

    return strstr(playlistPath, archivePath) != NULL;
}

static void Config()
{
    bool ok = false;
    if (IsWacupHost() && g_hiddenWnd)
    {
        int index = static_cast<int>(SendMessage(g_plugin.hwndParent, WM_WA_IPC, -1, IPC_PLAYLIST_GET_NEXT_SELECTED));
        if (index < 0)
            index = static_cast<int>(SendMessage(g_plugin.hwndParent, WM_WA_IPC, 0, IPC_GETLISTPOS));
        char archivePath[MAX_PATH * 4]{};
        if (index >= 0 && GetPlaylistFileAtIndex(g_plugin.hwndParent, index, archivePath, sizeof(archivePath)) &&
            IsSupportedArchivePath(archivePath))
            ok = QueueWacupMetadataPreflight(g_hiddenWnd, archivePath, index, false);
    }
    else
    {
        ok = ExpandSelectedOrCurrentZip(g_plugin.hwndParent);
    }

    if (!ok)
    {
        DebugLogWrite("WARN", "GEN", "MANUAL_EXPAND_FAIL", "reason=no_supported_archive_selected");
        MessageBoxA(g_plugin.hwndParent, "Select a supported archive in the playlist first.", "Archive Stream - Expand Archive", MB_OK | MB_ICONWARNING);
    }
}

static void Quit()
{
    InterlockedIncrement(&g_preflightGeneration);
    if (g_hiddenWnd)
    {
        DestroyWindow(g_hiddenWnd);
        g_hiddenWnd = NULL;
    }

    UnregisterClassA(ARCHIVE_STREAM_HIDDEN_CLASS, g_hInstance);
    DebugLogShutdown();
}

struct WacupPreflightWork
{
    std::string archivePath;
    int preferredIndex = -1;
    LONG generation = 0;
    HWND notifyWindow = nullptr;
    std::vector<ArchivePreparedPlaylistItem> items;
    bool succeeded = false;
    bool startPlayback = true;
    HMODULE module = nullptr;
};

static bool IsWacupHost()
{
    char executable[MAX_PATH]{};
    GetModuleFileNameA(nullptr, executable, sizeof(executable));
    const char* filename = strrchr(executable, '\\');
    return _stricmp(filename ? filename + 1 : executable, "wacup.exe") == 0;
}

static DWORD WINAPI WacupMetadataPreflightWorker(LPVOID parameter)
{
    std::unique_ptr<WacupPreflightWork> work(static_cast<WacupPreflightWork*>(parameter));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    ArchivePasswordSetPromptAllowedForCurrentThread(false);
    work->succeeded = PrepareArchivePlaylistForWacup(work->archivePath.c_str(), work->items, &g_preflightGeneration, work->generation);
    ArchivePasswordSetPromptAllowedForCurrentThread(true);
    DebugLogWrite("INFO", "GEN", "PREFLIGHT_WORKER_DONE", "archive=%s | prepared=%d | entries=%zu", work->archivePath.c_str(), work->succeeded ? 1 : 0, work->items.size());

    const HMODULE module = work->module;
    if (PostMessage(work->notifyWindow, WM_ARCHIVE_STREAM_METADATA_READY, 0, reinterpret_cast<LPARAM>(work.get())))
        work.release();
    else
        work.reset();
    FreeLibraryAndExitThread(module, 0);
    return 0;
}

static bool QueueWacupMetadataPreflight(HWND hwnd, const std::string& archivePath, int preferredIndex, bool startPlayback)
{
    std::unique_ptr<WacupPreflightWork> work(new (std::nothrow) WacupPreflightWork());
    if (!work)
        return false;
    work->archivePath = archivePath;
    work->preferredIndex = preferredIndex;
    work->startPlayback = startPlayback;
    work->generation = InterlockedIncrement(&g_preflightGeneration);
    work->notifyWindow = hwnd;

    // Keep this module loaded for the lifetime of the detached worker.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&WacupMetadataPreflightWorker), &module))
        return false;
    work->module = module;
    HANDLE thread = CreateThread(nullptr, 0, WacupMetadataPreflightWorker, work.get(), 0, nullptr);
    if (!thread)
    {
        FreeLibrary(module);
        DebugLogWrite("ERROR", "GEN", "PREFLIGHT_START_FAIL", "archive=%s | winerr=%lu", archivePath.c_str(), GetLastError());
        return false;
    }
    work.release();
    CloseHandle(thread);
    DebugLogWrite("INFO", "GEN", "PREFLIGHT_QUEUED", "archive=%s | index=%d", archivePath.c_str(), preferredIndex);
    return true;
}

static bool CompleteExpandedPlaylist(HWND hwnd, const std::string& archivePath, int preferredIndex, const std::vector<ArchivePreparedPlaylistItem>* prepared, bool startPlayback)
{
    const bool ok = prepared
        ? ExpandPreparedArchiveAtIndex(g_plugin.hwndParent, archivePath.c_str(), preferredIndex, *prepared, startPlayback)
        : ExpandZipPathInPlaylistAtIndex(g_plugin.hwndParent, archivePath.c_str(), preferredIndex, startPlayback);
    if (!ok)
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_DEFERRED_FAIL", "archive=%s | index=%d | prepared=%d", archivePath.c_str(), preferredIndex, prepared ? 1 : 0);
        return false;
    }

    if (!startPlayback)
        return true;

    // Find the actual inserted position, not a stale index from before the
    // background scan. The user can rearrange the playlist while it runs.
    int playIndex = -1;
    const std::string firstPath = prepared && !prepared->empty() ? prepared->front().path : std::string();
    const int count = static_cast<int>(SendMessage(g_plugin.hwndParent, WM_WA_IPC, 0, IPC_GETLISTLENGTH));
    for (int i = 0; i < count; ++i)
    {
        char path[MAX_PATH * 4]{};
        if (!GetPlaylistFileAtIndex(g_plugin.hwndParent, i, path, sizeof(path)))
            continue;
        if ((!firstPath.empty() && _stricmp(path, firstPath.c_str()) == 0) ||
            (firstPath.empty() && strstr(path, ".archive_stream|") && _strnicmp(path, archivePath.c_str(), archivePath.size()) == 0))
        {
            playIndex = i;
            break;
        }
    }
    if (playIndex < 0)
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_PLAY_CAPTURE_FAIL", "archive=%s | reason=could_not_find_inserted_virtual_entry", archivePath.c_str());
        return false;
    }

    g_pendingExpandedPlayIndex = playIndex;
    g_pendingExpandedPlayPath.clear();
    char actualPath[MAX_PATH * 4]{};
    if (GetPlaylistFileAtIndex(g_plugin.hwndParent, playIndex, actualPath, sizeof(actualPath)))
        g_pendingExpandedPlayPath = actualPath;
    if (!PostMessage(hwnd, WM_ARCHIVE_STREAM_PLAY_EXPANDED_INDEX, 0, 0))
    {
        DebugLogWrite("ERROR", "GEN", "EXPAND_PLAY_POST_FAIL", "archive=%s | index=%d | winerr=%lu", archivePath.c_str(), playIndex, GetLastError());
        g_pendingExpandedPlayIndex = -1;
        g_pendingExpandedPlayPath.clear();
        return false;
    }
    return true;
}

static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COPYDATA)
    {
        COPYDATASTRUCT* cds = (COPYDATASTRUCT*)lParam;

        if (cds && cds->dwData == ARCHIVE_STREAM_COPYDATA_EXPAND_ZIP && cds->lpData && cds->cbData > 1)
        {
            const char* payload = (const char*)cds->lpData;

            /*
                Payload format:
                    index|archivePath

                Backwards-compatible fallback:
                    archivePath
            */
            int playlistIndex = -1;
            const char* archivePath = payload;
            const char* sep = strchr(payload, '|');

            if (sep && sep > payload)
            {
                char indexText[32];
                size_t indexLen = (size_t)(sep - payload);

                if (indexLen < sizeof(indexText))
                {
                    memcpy(indexText, payload, indexLen);
                    indexText[indexLen] = '\0';
                    char* end = NULL;
                    long parsed = strtol(indexText, &end, 10);

                    if (end && *end == '\0' && parsed >= 0 && parsed <= 1000000)
                    {
                        playlistIndex = (int)parsed;
                        archivePath = sep + 1;
                    }
                }
            }

            if (!archivePath || !archivePath[0])
            {
                DebugLogWrite("WARN", "GEN", "COPYDATA_REJECTED", "reason=empty_archive_path");
                return FALSE;
            }

            DebugLogWrite("INFO", "GEN", "EXPAND_REQUEST_RECEIVED", "archive=%s | playlist_index=%d", archivePath, playlistIndex);
            g_pendingExpand.archivePath = archivePath;
            g_pendingExpand.playlistIndex = playlistIndex;

            if (!PostMessage(hwnd, WM_ARCHIVE_STREAM_DO_EXPAND, 0, 0))
            {
                DebugLogWrite("ERROR", "GEN", "EXPAND_DEFER_POST_FAIL", "archive=%s | index=%d | winerr=%lu", archivePath, playlistIndex, GetLastError());
                g_pendingExpand.archivePath.clear();
                g_pendingExpand.playlistIndex = -1;

                return FALSE;
            }

            return TRUE;
        }

        DebugLogWrite("WARN", "GEN", "COPYDATA_REJECTED", "reason=invalid_copydata | dwData=%llu | cbData=%lu | hasData=%d", cds ? (unsigned long long)cds->dwData : 0ULL, cds ? (unsigned long)cds->cbData : 0UL, (cds && cds->lpData) ? 1 : 0);
        
        return FALSE;
    }

    if (msg == WM_ARCHIVE_STREAM_DO_EXPAND)
    {
        if (!g_pendingExpand.archivePath.empty())
        {
            const std::string archivePath = g_pendingExpand.archivePath;
            const int playlistIndex = g_pendingExpand.playlistIndex;
            g_pendingExpand.archivePath.clear();
            g_pendingExpand.playlistIndex = -1;

            if (IsWacupHost())
            {
                // No synchronous archive reads on WACUP's UI thread. The
                // original archive remains in the playlist until every
                // final tag and duration has been prepared in the background.
                if (!QueueWacupMetadataPreflight(hwnd, archivePath, playlistIndex, true))
                    DebugLogWrite("ERROR", "GEN", "PREFLIGHT_QUEUE_FAIL", "archive=%s | index=%d", archivePath.c_str(), playlistIndex);
            }
            else
            {
                CompleteExpandedPlaylist(hwnd, archivePath, playlistIndex, nullptr, true);
            }
        }
        return 0;
    }

    if (msg == WM_ARCHIVE_STREAM_METADATA_READY)
    {
        std::unique_ptr<WacupPreflightWork> result(reinterpret_cast<WacupPreflightWork*>(lParam));
        if (!result)
            return 0;
        if (result->generation != InterlockedCompareExchange(&g_preflightGeneration, 0, 0))
        {
            DebugLogWrite("INFO", "GEN", "PREFLIGHT_STALE", "archive=%s | reason=newer_expansion_requested", result->archivePath.c_str());
            return 0;
        }
        if (!result->succeeded || result->items.empty())
        {
            DebugLogWrite("ERROR", "GEN", "PREFLIGHT_FAILED", "archive=%s | reason=metadata_preparation_failed", result->archivePath.c_str());
            return 0;
        }
        CompleteExpandedPlaylist(hwnd, result->archivePath, result->preferredIndex, &result->items, result->startPlayback);
        return 0;
    }
    if (msg == WM_ARCHIVE_STREAM_PLAY_EXPANDED_INDEX) 
    {
        int playIndex = g_pendingExpandedPlayIndex;
        std::string expectedPath = g_pendingExpandedPlayPath;
        g_pendingExpandedPlayIndex = -1;
        g_pendingExpandedPlayPath.clear();

        if (playIndex < 0) {
            DebugLogWrite("WARN", "GEN", "EXPAND_PLAY_SKIPPED", "reason=no_pending_index");

            return 0;
        }

        char actualPath[MAX_PATH * 4];
        actualPath[0] = '\0';

        if (!GetPlaylistFileAtIndex(g_plugin.hwndParent, playIndex, actualPath, sizeof(actualPath))) {
            DebugLogWrite("WARN", "GEN", "EXPAND_PLAY_VERIFY_FAIL", "index=%d | reason=could_not_read_playlist_item", playIndex);

            return 0;
        }

        if (!strstr(actualPath, ".archive_stream|") && strncmp(actualPath, "archive_stream:", strlen("archive_stream:")) != 0) {
            DebugLogWrite("WARN", "GEN", "EXPAND_PLAY_ABORT", "index=%d | reason=playlist_item_not_virtual | actual=%s", playIndex, actualPath);

            return 0;
        }

        if (!expectedPath.empty() && _stricmp(actualPath, expectedPath.c_str()) != 0) {
            DebugLogWrite("WARN", "GEN", "EXPAND_PLAY_ABORT", "index=%d | reason=playlist_item_changed | expected=%s | actual=%s", playIndex, expectedPath.c_str(), actualPath);

            return 0;
        }

        SendMessage(g_plugin.hwndParent, WM_WA_IPC, playIndex, IPC_SETPLAYLISTPOS);
        PostMessage(g_plugin.hwndParent, WM_COMMAND, WINAMP_BUTTON2, 0);

        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static int Init()
{
    // This DLL links its own backend copy; match the input plugin's data folder.
    if (!ArchiveSetDataDirectoryName("WinampArchiveStream"))
    {
        DebugLogWrite("ERROR", "GEN", "DATA_DIRECTORY_FAIL", "reason=winamp_data_directory_config_failed");
        return 1;
    }

    ConfigureGeneralPluginLogging();
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = ARCHIVE_STREAM_HIDDEN_CLASS;

    if (!RegisterClassExA(&wc))
    {
        DWORD err = GetLastError();

        if (err != ERROR_CLASS_ALREADY_EXISTS)
            DebugLogWrite("ERROR", "GEN", "HIDDEN_WINDOW_CLASS_FAIL", "winerr=%lu", err);
    }

    g_hiddenWnd = CreateWindowExA(0, ARCHIVE_STREAM_HIDDEN_CLASS, "Archive Stream Expander Hidden Window", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInstance, NULL);

    if (!g_hiddenWnd)
        DebugLogWrite("ERROR", "GEN", "HIDDEN_WINDOW_CREATE_FAIL", "winerr=%lu", GetLastError());

    return 0;
}

extern "C" __declspec(dllexport) winampGeneralPurposePlugin* winampGetGeneralPurposePlugin()
{
    memset(&g_plugin, 0, sizeof(g_plugin));
    g_plugin.version = GPPHDR_VER;
    g_plugin.description = g_description;
    g_plugin.init = Init;
    g_plugin.config = Config;
    g_plugin.quit = Quit;

    return &g_plugin;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
        g_hInstance = (HINSTANCE)hModule;

    return TRUE;
}