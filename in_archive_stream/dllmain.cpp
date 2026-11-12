#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <cstdarg>
#include <shellapi.h>
#include "archive_common.h"
#include "archive_backend.h"
#include "archive_cache.h"
#include "archive_metadata.h"
#include "plugin_state.h"
#include "config_ui.h"
#include "winamp_playlist_bridge.h"
#include "winamp_bridge_protocol.h"
#include "playback.h"
#include "archive_reader.h"
#include "mf_support.h"
#include "mf_audio_decoder.h"
#include "debug_log.h"
#include "archive_passwords.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "Mfuuid.lib")

#ifndef WM_WA_IPC
#define WM_WA_IPC WM_USER
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

#define IDC_ABOUT_TEXT 9101
#define IDC_ABOUT_ARDEN_BUTTON  9102
#define IDC_ABOUT_CLOSE_BUTTON  9103

#ifndef WINAMP_BUTTON4
#define WINAMP_BUTTON4 40047
#endif

In_Module g_mod;

static char g_description[] = "Archive Stream Player v0.1";

static char g_fileExtensions[] =
"ZIP\0"
"ZIP archive audio files (*.ZIP)\0"
"7Z\0"
"7-Zip archive audio files (*.7Z)\0"
"001\0"
"Split archive first volume (*.7Z.001; *.ZIP.001)\0"
"RAR\0"
"RAR archive audio files (*.RAR)\0"
"ARCHIVE_STREAM\0"
"Archive Stream virtual track (*.ARCHIVE_STREAM)\0"
"\0";

HANDLE g_playThread = NULL;
volatile LONG g_stopRequested = 0;
volatile LONG g_passwordPromptCancelTick = 0;
volatile LONG g_isPlaying = 0;

size_t g_currentAudioSize = 0;
char g_currentTitle[1024] = { 0 };
int g_currentLengthMs = -1000;
int g_currentBitrateKbps = 0;
int g_currentSampleRate = 0;
int g_currentChannels = 0;

volatile LONG g_seekRequested = 0;
volatile LONG g_seekTargetMs = 0;
volatile LONG g_outputTimeBaseMs = 0;
volatile LONG g_trackStartTick = 0;

unsigned char* g_audioBuffer = nullptr;
size_t g_audioBufferSize = DEFAULT_BUFFER_SIZE_MB * 1024 * 1024;
size_t g_configuredBufferSizeMb = DEFAULT_BUFFER_SIZE_MB;
size_t g_configuredCacheSizeMb = DEFAULT_CACHE_SIZE_MB;

bool g_cacheSizeCleanupEnabled = true;
bool g_cacheAgeCleanupEnabled = true;
unsigned int g_cacheMaxAgeDays = DEFAULT_CACHE_AGE_DAYS;

char g_displayPrefix[32] = "";
char g_displayPostfix[32] = "";
char g_displayTemplate[160] = "<filename>";

char g_rarPasswordListPath[MAX_PATH * 4] = "";
bool g_debugLoggingEnabled = false;
char g_debugLogPath[MAX_PATH * 4] = "";
char g_iniPath[MAX_PATH] = { 0 };

HWND g_configWnd = NULL;
HWND g_bufferEdit = NULL;
HWND g_cacheEdit = NULL;

HINSTANCE g_hInstance = NULL;

volatile LONG g_currentAudioFormat = AUDIO_FORMAT_UNKNOWN;
volatile LONG g_playbackMode = ARCHIVE_PLAYBACK_MEMORY;
char g_currentArchivePath[MAX_PATH * 4] = { 0 };
mz_uint g_currentArchiveIndex = 0;
char g_currentCachePath[MAX_PATH * 4] = { 0 };

ArchiveStreamContext g_prevalidatedStream;
volatile LONG g_prevalidatedStreamReady = 0;

static HANDLE g_metadataWorkerThread = NULL;
static HANDLE g_metadataWorkerWakeEvent = NULL;
static CRITICAL_SECTION g_metadataQueueLock;
static bool g_metadataQueueLockInitialized = false;
static std::vector<std::string> g_metadataQueue;
static volatile LONG g_metadataWorkerStop = 0;
static HANDLE g_solid7zCacheWarmerThread = NULL;

static HANDLE g_solid7zCacheWarmerWakeEvent = NULL;
static CRITICAL_SECTION g_solid7zCacheWarmerLock;
static bool g_solid7zCacheWarmerLockInitialized = false;
static CRITICAL_SECTION g_solid7zExtractionLock;
static bool g_solid7zExtractionLockInitialized = false;
static std::string g_solid7zWarmArchivePath;
static mz_uint g_solid7zWarmStartIndex = 0;
static std::string g_solid7zActiveArchivePath;
static std::string g_solid7zCompletedArchivePath;
static mz_uint g_solid7zPriorityIndex = 0;
static bool g_solid7zPriorityPending = false;
static volatile LONG g_solid7zCacheWarmerStop = 0;

static DWORD WINAPI BackgroundMetadataWorkerProc(LPVOID param);
static void StopBackgroundMetadataWorker();
static void QueueBackgroundMetadataLookup(const char* virtualPath);
static void QueueBackgroundMetadataForArchive(const char* archivePath);
static void OnArchivePasswordAccepted(const char* archivePath);
static DWORD WINAPI Solid7zCacheWarmerProc(LPVOID param);
static void StopSolid7zCacheWarmer();

#ifndef THREAD_MODE_BACKGROUND_BEGIN
#define THREAD_MODE_BACKGROUND_BEGIN 0x00010000
#endif

#ifndef THREAD_MODE_BACKGROUND_END
#define THREAD_MODE_BACKGROUND_END 0x00020000
#endif


struct PasswordPromptState
{
    const char* archivePath;
    const char* entryName;
    char* outPassword;
    size_t outPasswordSize;
    bool* outSavePassword;
    HWND editPassword;
    HWND checkSave;
    bool accepted;
};

static LRESULT CALLBACK PasswordPromptWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PasswordPromptState* state = (PasswordPromptState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        state = (PasswordPromptState*)cs->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)state);

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        CreateWindowExA(0, "STATIC", "Password required for encrypted archive entry:", WS_CHILD | WS_VISIBLE, 12, 12, 360, 18, hwnd, NULL, g_hInstance, NULL);
        CreateWindowExA(0, "STATIC", state->entryName ? state->entryName : "", WS_CHILD | WS_VISIBLE, 12, 34, 360, 18, hwnd, NULL, g_hInstance, NULL);

        state->editPassword = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL, 12, 64, 360, 22, hwnd, (HMENU)1001, g_hInstance, NULL);
        state->checkSave = CreateWindowExA(0, "BUTTON", "Save password to keeps.txt", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, 94, 220, 22, hwnd, (HMENU)1002, g_hInstance, NULL);

        HWND okButton = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 214, 128, 75, 24, hwnd, (HMENU)IDOK, g_hInstance, NULL);
        HWND cancelButton = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE, 297, 128, 75, 24, hwnd, (HMENU)IDCANCEL, g_hInstance, NULL);

        SendMessage(state->editPassword, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(state->checkSave, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(okButton, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessage(cancelButton, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessage(state->editPassword, EM_LIMITTEXT, (WPARAM)(state->outPasswordSize - 1), 0);
        SetFocus(state->editPassword);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK)
        {
            GetWindowTextA(state->editPassword, state->outPassword, (int)state->outPasswordSize);
            *state->outSavePassword = SendMessage(state->checkSave, BM_GETCHECK, 0, 0) == BST_CHECKED;
            state->accepted = state->outPassword && state->outPassword[0];
            DestroyWindow(hwnd);
            return 0;
        }

        if (LOWORD(wParam) == IDCANCEL)
        {
            state->accepted = false;
            DestroyWindow(hwnd);
            return 0;
        }

        break;

    case WM_CLOSE:
        state->accepted = false;
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// -----------------------------------------------------------------------------
// RequestStopAfterPasswordPromptFailure
//
// The password prompt can be reached from playback/stream/MF read paths.
// Do not call Stop() directly here because that risks stopping from inside the
// playback path itself. Instead, mark the current playback as stopping and post
// Winamp's Stop command back to the main window.
//
// This handles Cancel and empty-password OK the same way: the current attempted
// playback should stop cleanly instead of sitting in a silent "playing" state.
// -----------------------------------------------------------------------------
static void RequestStopAfterPasswordPromptFailure()
{
    InterlockedExchange(&g_stopRequested, 1);
    InterlockedExchange(&g_passwordPromptCancelTick, (LONG)GetTickCount());

    if (g_mod.hMainWindow)
    {
        PostMessage(g_mod.hMainWindow, WM_COMMAND, WINAMP_BUTTON4, 0);
    }

    DebugLogWrite("WARN", "PASSWORD", "STOP_REQUESTED", "reason=password_prompt_cancelled_or_empty");
}

static bool PromptForArchivePassword(const char* archivePath, const char* entryName, char* outPassword, size_t outPasswordSize, bool* outSavePassword)
{
    if (!outPassword || outPasswordSize == 0 || !outSavePassword)
    {
        return false;
    }

    outPassword[0] = '\0';
    *outSavePassword = false;

    const char* className = "ArchiveStreamPasswordPrompt";

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));

    wc.lpfnWndProc = PasswordPromptWndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = className;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    RegisterClassA(&wc);

    PasswordPromptState state;
    memset(&state, 0, sizeof(state));

    state.archivePath = archivePath;
    state.entryName = entryName;
    state.outPassword = outPassword;
    state.outPasswordSize = outPasswordSize;
    state.outSavePassword = outSavePassword;
    state.accepted = false;

    HWND hwnd = CreateWindowExA(WS_EX_DLGMODALFRAME, className, "Archive Password", WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 400, 205, g_mod.hMainWindow, NULL, g_hInstance, &state);

    if (!hwnd)
    {
        DebugLogWrite("ERROR", "PASSWORD", "PROMPT_CREATE_FAIL", "archive=%s | entry=%s | winerr=%lu", archivePath ? archivePath : "", entryName ? entryName : "", GetLastError());
        return false;
    }

    EnableWindow(g_mod.hMainWindow, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;

    while (IsWindow(hwnd) && GetMessage(&msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessage(hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    EnableWindow(g_mod.hMainWindow, TRUE);
    SetActiveWindow(g_mod.hMainWindow);

    if (!state.accepted)
    {
        RequestStopAfterPasswordPromptFailure();
    }

    return state.accepted;
}




void Stop();

static void Init()
{
    LoadConfig();

    DebugLogConfigure(g_debugLoggingEnabled, g_debugLogPath, true);
    ArchivePasswordSetPromptCallback(PromptForArchivePassword);
    ArchivePasswordSetAcceptedCallback(OnArchivePasswordAccepted);

    if (!g_audioBuffer)
    {
        g_audioBuffer = (unsigned char*)malloc(g_audioBufferSize);

        if (!g_audioBuffer)
        {
            DebugLogWrite("ERROR", "INIT", "BUFFER_ALLOC_FAIL", "bytes=%zu | configured_mb=%zu", g_audioBufferSize, g_configuredBufferSizeMb);

            MessageBoxA(g_mod.hMainWindow, "Could not allocate archive playback buffer.", "Archive Stream - Buffer Error", MB_OK | MB_ICONERROR);
        }
    }
}

void Quit()
{
    StopSolid7zCacheWarmer();
    StopBackgroundMetadataWorker();
    ArchivePasswordSetPromptCallback(nullptr);
    ArchivePasswordSetAcceptedCallback(nullptr);
    Stop();

    if (g_audioBuffer)
    {
        free(g_audioBuffer);
        g_audioBuffer = nullptr;
    }

    MfShutdownIfStarted();
    DebugLogShutdown();
}

// -----------------------------------------------------------------------------
// IsOurFile
//
// Tells Winamp whether this input plugin owns the supplied path.
//
// This accepts:
//     virtual archive stream paths
//     .zip
//     .7z
//     .7z.001 first-volume split 7z archives
//     .rar
//
// It deliberately does not accept arbitrary .001 files. The plugin registers the
// 001 extension only so Winamp will call us for split 7z first volumes; the actual
// ownership check requires the full ".7z.001" suffix.
// -----------------------------------------------------------------------------
static int IsOurFile(const char* filename)
{
    if (!filename)
    {
        return 0;
    }

    if (IsVirtualArchivePath(filename))
    {
        return 1;
    }

    if (IsZipArchivePath(filename))
    {
        return 1;
    }

    if (IsSevenZipArchivePath(filename))
    {
        return 1;
    }

    if (IsRarArchivePath(filename))
    {
        return 1;
    }

    return 0;
}

static bool ParseVirtualDisplayPath(const char* file, char* outArchivePath, size_t outArchivePathSize, mz_uint* outIndex, char* outInternalPath,size_t outInternalPathSize)
{
    return ParseVirtualArchivePath(file, outArchivePath, outArchivePathSize, outIndex, outInternalPath, outInternalPathSize );
}

static std::string StripDisplayExtension(const char* value)
{
    if (!value || !value[0]) return std::string();

    std::string result = value;

    size_t dot = result.find_last_of('.');
    size_t slash1 = result.find_last_of('\\');
    size_t slash2 = result.find_last_of('/');
    size_t slash = std::string::npos;

    if (slash1 != std::string::npos && slash2 != std::string::npos) slash = slash1 > slash2 ? slash1 : slash2;
    else if (slash1 != std::string::npos) slash = slash1;
    else slash = slash2;

    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) result = result.substr(0, dot);

    return result;
}

static void ReplaceAllDisplayTokens(std::string& text, const char* token, const std::string& value)
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

static std::string GetBaseNameOnly(const char* path)
{
    if (!path || !path[0]) return std::string();

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* base = path;

    if (slash1 && slash1 + 1 > base) base = slash1 + 1;
    if (slash2 && slash2 + 1 > base) base = slash2 + 1;

    return std::string(base);
}
// -----------------------------------------------------------------------------
// GetArchiveNameFromVirtualPath
//
// Extracts the source archive base name from a virtual archive stream path.
//
// Example:
//     C:\Music\Album.zip.archive_stream|3|Disc1\Track.mp3
//
// returns:
//     Album
//
// This is used by the <archive> display-template token. It deliberately strips
// the .archive_stream marker before basename/extension handling so the archive
// name is used, not the internal track name.
// -----------------------------------------------------------------------------
static std::string GetArchiveNameFromVirtualPath(const char* virtualPath)
{
    if (!virtualPath || !virtualPath[0])
    {
        return std::string();
    }

    std::string archivePath = virtualPath;

    size_t marker = archivePath.find(".archive_stream|");

    if (marker != std::string::npos)
    {
        archivePath = archivePath.substr(0, marker);
    }

    std::string archiveBaseName = GetBaseNameOnly(archivePath.c_str());
    return StripDisplayExtension(archiveBaseName.c_str());
}

static std::string StripKnownAudioExtension(const std::string& value)
{
    std::string result = value;

    const char* audioExts[] = { ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac", ".wma" };

    for (size_t i = 0; i < sizeof(audioExts) / sizeof(audioExts[0]); ++i)
    {
        const char* ext = audioExts[i];
        size_t extLen = strlen(ext);

        if (result.length() >= extLen && _stricmp(result.c_str() + result.length() - extLen, ext) == 0)
        {
            result.resize(result.length() - extLen);
            return result;
        }
    }

    return result;
}
// -----------------------------------------------------------------------------
// FormatStableVirtualTitle
//
// Builds the playlist title for a virtual archive entry.
//
// Supported display-template tokens:
//     <archive>   source archive base name, without extension
//     <filename>  internal audio file base name, without audio extension
//     <#>         one-based archive entry number
//
// This title is used by the input plugin when Winamp asks for file info, so it
// must match the general plugin expansion title logic. Without replacing
// <archive> here, titles created during expansion can later be changed by
// Winamp's metadata/title refresh.
// -----------------------------------------------------------------------------
static void FormatStableVirtualTitle(const char* virtualPath, const char* internalPath, mz_uint archiveIndex, char* outTitle, size_t outTitleSize)
{
    if (!outTitle || outTitleSize == 0)
    {
        return;
    }

    outTitle[0] = '\0';

    std::string filename = StripKnownAudioExtension(GetBaseNameOnly(internalPath));
    std::string archive = GetArchiveNameFromVirtualPath(virtualPath);

    if (filename.empty())
    {
        filename = StripKnownAudioExtension(GetBaseNameOnly(virtualPath));
    }

    if (filename.empty())
    {
        filename = "archive track";
    }

    if (archive.empty())
    {
        archive = "archive";
    }

    char numberText[32];
    sprintf_s(numberText, sizeof(numberText), "%u", (unsigned int)archiveIndex + 1);

    std::string displayTemplate = g_displayTemplate[0] ? g_displayTemplate : "<filename>";

    if (
        displayTemplate.find("<archive>") == std::string::npos &&
        displayTemplate.find("<filename>") == std::string::npos &&
        displayTemplate.find("<#>") == std::string::npos
        )
    {
        displayTemplate += "<filename>";
    }

    std::string finalTitle;
    finalTitle.reserve(strlen(g_displayPrefix) + displayTemplate.length() + strlen(g_displayPostfix) + 1);

    finalTitle += g_displayPrefix;
    finalTitle += displayTemplate;
    finalTitle += g_displayPostfix;

    bool hasArchiveToken = finalTitle.find("<archive>") != std::string::npos;
    bool hasFilenameToken = finalTitle.find("<filename>") != std::string::npos;
    bool hasNumberToken = finalTitle.find("<#>") != std::string::npos;

    if (!hasArchiveToken && !hasFilenameToken && !hasNumberToken)
    {
        finalTitle += "<filename>";
    }

    ReplaceAllDisplayTokens(finalTitle, "<archive>", archive);
    ReplaceAllDisplayTokens(finalTitle, "<filename>", filename);
    ReplaceAllDisplayTokens(finalTitle, "<#>", numberText);

    strcpy_s(outTitle, outTitleSize, finalTitle.c_str());
}

static const char* AudioFormatToLogName(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_MP3:
        return "MP3";
    case AUDIO_FORMAT_WAV:
        return "WAV";
    case AUDIO_FORMAT_FLAC:
        return "FLAC";
    case AUDIO_FORMAT_OGG:
        return "OGG";
    case AUDIO_FORMAT_M4A:
        return "M4A";
    case AUDIO_FORMAT_AAC:
        return "AAC";
    case AUDIO_FORMAT_WMA:
        return "WMA";
    default:
        return "UNKNOWN";
    }
}

static const char* PlaybackModeToLogName(ArchivePlaybackMode mode)
{
    switch (mode)
    {
    case ARCHIVE_PLAYBACK_MEMORY:
        return "BUFFER";
    case ARCHIVE_PLAYBACK_STREAM:
        return "STREAM";
    case ARCHIVE_PLAYBACK_CACHE_FILE:
        return "CACHE";
    default:
        return "UNKNOWN";
    }
}

static const char* ArchiveTypeToLogName(const char* archivePath)
{
    if (IsZipArchivePath(archivePath))
        return "ZIP";

    if (IsSevenZipArchivePath(archivePath))
        return "7Z";

    if (IsRarArchivePath(archivePath))
        return "RAR";

    return "UNKNOWN";
}

static bool IsLikelyMultipartArchivePath(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
        return false;

    const char* ext = strrchr(archivePath, '.');

    if (!ext)
        return false;

    if (_stricmp(ext, ".001") == 0)
        return true;

    if (_stricmp(ext, ".r00") == 0)
        return true;

    if (_stricmp(ext, ".part1.rar") == 0)
        return true;

    if (_stricmp(ext, ".part01.rar") == 0)
        return true;

    if (strstr(archivePath, ".part") && IsRarArchivePath(archivePath))
        return true;

    return false;
}

static void LogArchiveRouteProfile(const char* archivePath, mz_uint selectedIndex, const char* selectedEntry, mz_uint64 selectedSize, AudioFormat audioFormat, ArchivePlaybackMode playbackMode, const char* decisionReason)
{
    if (IsRarArchivePath(archivePath))
    {
        DebugLogWrite("INFO", "ROUTE", "ARCHIVE_PROFILE_FAST", "archive=%s | archive_type=RAR | index=%u | entry=%s | audio_type=%s | route=%s | reason=%s | entry_size=%llu | buffer_size=%zu | reason_fast=skip_rar_diagnostic_rescan", archivePath ? archivePath : "", (unsigned int)selectedIndex, selectedEntry ? selectedEntry : "", AudioFormatToLogName(audioFormat), PlaybackModeToLogName(playbackMode), decisionReason ? decisionReason : "", (unsigned long long)selectedSize, g_audioBufferSize);
        return;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;
    wchar_t wideArchivePath[MAX_PATH * 4];

    wideArchivePath[0] = L'\0';

    bool pathOk = CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0]));
    bool listOk = false;

    if (pathOk)
    {
        listOk = ArchiveListEntries(wideArchivePath, entries, error);
    }
    else
    {
        error = "Could not convert archive path to wide path";
    }

    size_t folderCount = 0;
    size_t audioCount = 0;
    bool hasFolderPaths = false;
    bool anySolid = false;
    bool selectedSolid = false;
    bool selectedStreamCandidate = false;
    uint32_t selectedFolderIndex = 0xFFFFFFFF;
    uint32_t selectedFilesInFolder = 0;
    uint32_t selectedMethodId = 0;
    bool selectedHasMethodId = false;

    if (listOk)
    {
        for (const ArchiveEntry& entry : entries)
        {
            if (entry.isFolder)
            {
                ++folderCount;
            }

            if (entry.isSupportedAudio)
            {
                ++audioCount;

                if (strchr(entry.path.c_str(), '\\') || strchr(entry.path.c_str(), '/'))
                {
                    hasFolderPaths = true;
                }
            }

            if (entry.isSolid)
            {
                anySolid = true;
            }

            if (entry.index == selectedIndex)
            {
                selectedSolid = entry.isSolid;
                selectedStreamCandidate = entry.streamCandidate;
                selectedFolderIndex = entry.folderIndex;
                selectedFilesInFolder = entry.filesInFolder;
                selectedMethodId = entry.methodId;
                selectedHasMethodId = entry.hasMethodId;
            }
        }
    }

    DebugLogWrite("INFO", "ROUTE", "ARCHIVE_PROFILE", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=%s | reason=%s | entry_size=%llu | buffer_size=%zu | path_ok=%d | list_ok=%d | total_entries=%zu | audio_entries=%zu | folders=%zu | has_folders=%d | any_solid=%d | selected_solid=%d | selected_stream_candidate=%d | selected_folder_index=%u | selected_files_in_folder=%u | selected_method_id=%u | selected_has_method_id=%d | multipart_guess=%d | password_list_configured=%d", archivePath ? archivePath 
        : "", ArchiveTypeToLogName(archivePath), (unsigned int)selectedIndex, selectedEntry ? selectedEntry 
        : "", AudioFormatToLogName(audioFormat), PlaybackModeToLogName(playbackMode), decisionReason ? decisionReason 
        : "", (unsigned long long)selectedSize, g_audioBufferSize, pathOk ? 1 
        : 0, listOk ? 1 
        : 0, entries.size(), audioCount, folderCount, (folderCount > 0 || hasFolderPaths) ? 1 
        : 0, anySolid ? 1 
        : 0, selectedSolid ? 1 
        : 0, selectedStreamCandidate ? 1 
        : 0, selectedFolderIndex, selectedFilesInFolder, selectedMethodId, selectedHasMethodId ? 1 
        : 0, IsLikelyMultipartArchivePath(archivePath) ? 1 
        : 0, g_rarPasswordListPath[0] ? 1 
        : 0);

    if (!listOk)
    {
        DebugLogWrite("WARN", "ROUTE", "ARCHIVE_PROFILE_LIST_FAIL", "archive=%s | archive_type=%s | index=%u | path_ok=%d | error=%s", archivePath ? archivePath : "", ArchiveTypeToLogName(archivePath), (unsigned int)selectedIndex, pathOk ? 1 : 0, error.c_str());
    }
}

static void EnsureMetadataQueueLock()
{
    if (!g_metadataQueueLockInitialized)
    {
        InitializeCriticalSection(&g_metadataQueueLock);
        g_metadataQueueLockInitialized = true;
    }
}

static bool MetadataQueueContainsLocked(const char* virtualPath)
{
    if (!virtualPath || !virtualPath[0])
    {
        return true;
    }

    for (const std::string& queuedPath : g_metadataQueue)
    {
        if (_stricmp(queuedPath.c_str(), virtualPath) == 0)
        {
            return true;
        }
    }

    return false;
}

static void StartBackgroundMetadataWorkerIfNeeded()
{
    EnsureMetadataQueueLock();

    if (!g_metadataWorkerWakeEvent)
    {
        g_metadataWorkerWakeEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    }

    if (!g_metadataWorkerThread)
    {
        InterlockedExchange(&g_metadataWorkerStop, 0);
        g_metadataWorkerThread = CreateThread(NULL, 0, BackgroundMetadataWorkerProc, NULL, 0, NULL);

        if (!g_metadataWorkerThread)
        {
            DebugLogWrite("ERROR", "METADATA_BG", "THREAD_START_FAIL", "reason=create_thread_failed");
        }
    }
}

static void QueueBackgroundMetadataLookup(const char* virtualPath)
{
    if (!virtualPath || !virtualPath[0])
    {
        return;
    }

    StartBackgroundMetadataWorkerIfNeeded();

    if (!g_metadataWorkerWakeEvent)
    {
        return;
    }

    EnterCriticalSection(&g_metadataQueueLock);

    if (!MetadataQueueContainsLocked(virtualPath))
    {
        g_metadataQueue.push_back(virtualPath);
        DebugLogWrite("INFO", "METADATA_BG", "QUEUE_ADD", "path=%s | queue_size=%zu", virtualPath, g_metadataQueue.size());
    }

    LeaveCriticalSection(&g_metadataQueueLock);
    SetEvent(g_metadataWorkerWakeEvent);
}

// -----------------------------------------------------------------------------
// QueueBackgroundMetadataForArchive
//
// Queues metadata refresh for every currently-expanded virtual playlist entry
// from the same archive.
//
// This is called after a password is proven valid. It lets any tracks that
// failed metadata earlier because no password was available get retried in the
// background using the runtime/session password.
// -----------------------------------------------------------------------------
static void QueueBackgroundMetadataForArchive(const char* archivePath)
{
    if (!g_mod.hMainWindow || !archivePath || !archivePath[0])
    {
        return;
    }

    int playlistLength = (int)SendMessage(g_mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    int queued = 0;

    for (int i = 0; i < playlistLength; ++i)
    {
        char* playlistPath = (char*)SendMessage(g_mod.hMainWindow, WM_WA_IPC, i, IPC_GETPLAYLISTFILE);

        if (!playlistPath || !playlistPath[0] || !IsVirtualArchivePath(playlistPath))
        {
            continue;
        }

        char itemArchivePath[MAX_PATH * 4];
        char itemInternalPath[MAX_PATH * 4];
        mz_uint itemIndex = 0;

        itemArchivePath[0] = '\0';
        itemInternalPath[0] = '\0';

        if (!ParseVirtualArchivePath(playlistPath, itemArchivePath, sizeof(itemArchivePath), &itemIndex, itemInternalPath, sizeof(itemInternalPath)))
        {
            continue;
        }

        if (_stricmp(itemArchivePath, archivePath) != 0)
        {
            continue;
        }

        QueueBackgroundMetadataLookup(playlistPath);
        ++queued;
    }

    DebugLogWrite("INFO", "METADATA_BG", "QUEUE_ARCHIVE_AFTER_PASSWORD", "archive=%s | queued=%d", archivePath, queued);
}

// -----------------------------------------------------------------------------
// OnArchivePasswordAccepted
//
// Called by the shared password layer after a password has successfully decrypted
// data for an archive.
// -----------------------------------------------------------------------------
static void OnArchivePasswordAccepted(const char* archivePath)
{
    QueueBackgroundMetadataForArchive(archivePath);
}

static bool PopBackgroundMetadataItem(std::string& outPath)
{
    outPath.clear();

    EnsureMetadataQueueLock();
    EnterCriticalSection(&g_metadataQueueLock);

    if (!g_metadataQueue.empty())
    {
        outPath = g_metadataQueue.front();
        g_metadataQueue.erase(g_metadataQueue.begin());
    }

    bool hasMore = !g_metadataQueue.empty();

    if (!hasMore && g_metadataWorkerWakeEvent)
    {
        ResetEvent(g_metadataWorkerWakeEvent);
    }

    LeaveCriticalSection(&g_metadataQueueLock);
    return !outPath.empty();
}

static void RefreshPlaylistCacheForVirtualPath(const char* virtualPath)
{
    if (!g_mod.hMainWindow || !virtualPath || !virtualPath[0])
    {
        return;
    }

    wchar_t widePath[4096];
    widePath[0] = L'\0';

    int converted = MultiByteToWideChar(CP_ACP, 0, virtualPath, -1, widePath, (int)(sizeof(widePath) / sizeof(widePath[0])));

    if (converted <= 0 || widePath[0] == L'\0')
    {
        DebugLogWrite("WARN", "METADATA_BG", "REFRESH_SKIP", "path=%s | reason=wide_conversion_failed | winerr=%lu", virtualPath, GetLastError());
        return;
    }

    SendMessage(g_mod.hMainWindow, WM_WA_IPC, (WPARAM)widePath, IPC_REFRESHPLCACHE);
    DebugLogWrite("INFO", "METADATA_BG", "REFRESH_PATH", "path=%s", virtualPath);
}

static DWORD WINAPI BackgroundMetadataWorkerProc(LPVOID param)
{
    (void)param;

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

    while (InterlockedCompareExchange(&g_metadataWorkerStop, 0, 0) == 0)
    {
        if (!g_metadataWorkerWakeEvent)
        {
            Sleep(250);
            continue;
        }

        WaitForSingleObject(g_metadataWorkerWakeEvent, 1000);

        if (InterlockedCompareExchange(&g_metadataWorkerStop, 0, 0) != 0)
        {
            break;
        }

        std::string virtualPath;

        while (PopBackgroundMetadataItem(virtualPath))
        {
            if (InterlockedCompareExchange(&g_metadataWorkerStop, 0, 0) != 0)
            {
                break;
            }

            char title[1024];
            int lengthMs = -1000;

            title[0] = '\0';

            bool previousPromptAllowed = ArchivePasswordGetPromptAllowedForCurrentThread();
            ArchivePasswordSetPromptAllowedForCurrentThread(false);

            bool metadataOk = GetVirtualArchiveEntryInfoBackground(virtualPath.c_str(), title, sizeof(title), &lengthMs) && lengthMs > 0;

            ArchivePasswordSetPromptAllowedForCurrentThread(previousPromptAllowed);

            if (metadataOk)
            {
                DebugLogWrite("INFO", "METADATA_BG", "LOOKUP_DONE", "path=%s | length_ms=%d", virtualPath.c_str(), lengthMs);
                RefreshPlaylistCacheForVirtualPath(virtualPath.c_str());
            }
            else
            {
                DebugLogWrite("INFO", "METADATA_BG", "LOOKUP_DEFERRED_OR_FAIL", "path=%s", virtualPath.c_str());
            }

            Sleep(200);
        }
    }

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}

static void StopBackgroundMetadataWorker()
{
    InterlockedExchange(&g_metadataWorkerStop, 1);

    if (g_metadataWorkerWakeEvent)
    {
        SetEvent(g_metadataWorkerWakeEvent);
    }

    if (g_metadataWorkerThread)
    {
        WaitForSingleObject(g_metadataWorkerThread, 3000);
        CloseHandle(g_metadataWorkerThread);
        g_metadataWorkerThread = NULL;
    }

    if (g_metadataWorkerWakeEvent)
    {
        CloseHandle(g_metadataWorkerWakeEvent);
        g_metadataWorkerWakeEvent = NULL;
    }

    if (g_metadataQueueLockInitialized)
    {
        EnterCriticalSection(&g_metadataQueueLock);
        g_metadataQueue.clear();
        LeaveCriticalSection(&g_metadataQueueLock);
    }
}

// -----------------------------------------------------------------------------
// EnsureSolid7zCacheWarmerLocks
//
// Initializes the synchronization objects used by the solid 7z cache warmer.
//
// The warmer has its own queue/state lock and a separate extraction lock. The
// extraction lock prevents playback and the background warmer from extracting
// from the same solid 7z archive at the same time, which avoids duplicated solid
// decompression work and reduces backend contention.
// -----------------------------------------------------------------------------
static void EnsureSolid7zCacheWarmerLocks()
{
    if (!g_solid7zCacheWarmerLockInitialized)
    {
        InitializeCriticalSection(&g_solid7zCacheWarmerLock);
        g_solid7zCacheWarmerLockInitialized = true;
    }

    if (!g_solid7zExtractionLockInitialized)
    {
        InitializeCriticalSection(&g_solid7zExtractionLock);
        g_solid7zExtractionLockInitialized = true;
    }
}

// -----------------------------------------------------------------------------
// BuildVirtualPathForCacheWarmRefresh
//
// Builds the virtual playlist path used when refreshing Winamp playlist metadata
// after a background cache-warm extraction learns a duration.
//
// The current playlist expansion path uses the suffix marker form:
//     archive.7z.archive_stream|index|internal/path.mp3
//
// Refreshing this exact path lets Winamp ask GetFileInfo() again, where the newly
// stored metadata cache entry can return the learned duration.
// -----------------------------------------------------------------------------
static bool BuildVirtualPathForCacheWarmRefresh(const char* archivePath, mz_uint index, const char* internalName, std::string& outVirtualPath)
{
    outVirtualPath.clear();

    if (!archivePath || !archivePath[0] || !internalName || !internalName[0])
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "VIRTUAL_PATH_FAIL", "archive=%s | index=%u | entry=%s | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index, internalName ? internalName : "");
        return false;
    }

    outVirtualPath.reserve(strlen(archivePath) + strlen(internalName) + 64);
    outVirtualPath += archivePath;
    outVirtualPath += ".archive_stream|";
    outVirtualPath += std::to_string((unsigned int)index);
    outVirtualPath += "|";
    outVirtualPath += internalName;

    return true;
}

// -----------------------------------------------------------------------------
// StartSolid7zCacheWarmerIfNeeded
//
// Starts the low-priority solid 7z cache warmer thread on demand.
//
// The worker is only used for confirmed solid 7z archives. It is intentionally
// separate from the metadata worker because cache warming performs extraction and
// should not be mixed with cheap cache-only metadata refreshes.
// -----------------------------------------------------------------------------
static void StartSolid7zCacheWarmerIfNeeded()
{
    EnsureSolid7zCacheWarmerLocks();

    if (!g_solid7zCacheWarmerWakeEvent)
    {
        g_solid7zCacheWarmerWakeEvent = CreateEventA(NULL, TRUE, FALSE, NULL);

        if (!g_solid7zCacheWarmerWakeEvent)
        {
            DebugLogWrite("ERROR", "SOLID_CACHE_WARM", "EVENT_CREATE_FAIL", "reason=create_event_failed | winerr=%lu", GetLastError());
            return;
        }
    }

    if (!g_solid7zCacheWarmerThread)
    {
        InterlockedExchange(&g_solid7zCacheWarmerStop, 0);
        g_solid7zCacheWarmerThread = CreateThread(NULL, 0, Solid7zCacheWarmerProc, NULL, 0, NULL);

        if (!g_solid7zCacheWarmerThread)
        {
            DebugLogWrite("ERROR", "SOLID_CACHE_WARM", "THREAD_START_FAIL", "reason=create_thread_failed | winerr=%lu", GetLastError());
        }
    }
}

// -----------------------------------------------------------------------------
// QueueSolid7zCacheWarm
//
// Queues or reprioritizes solid 7z cache warming.
//
// If the warmer is idle or working on a different archive, this starts a new
// archive warm job. If it is already warming the same archive, this does not
// restart the whole pass; it only promotes the requested index so the worker can
// handle that entry next. This avoids repeated same-archive START/DONE cycles
// when the user skips through tracks.
//
// All queue failures are logged because this worker is responsible for making
// later solid-archive skips fast.
// -----------------------------------------------------------------------------
static void QueueSolid7zCacheWarm(const char* archivePath, mz_uint startIndex)
{
    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "QUEUE_FAIL", "archive=%s | start_index=%u | reason=missing_archive_path", archivePath ? archivePath : "", (unsigned int)startIndex);

        return;
    }

    if (!IsSevenZipArchivePath(archivePath) && !IsRarArchivePath(archivePath))
    {
        return;
    }

    StartSolid7zCacheWarmerIfNeeded();

    if (!g_solid7zCacheWarmerWakeEvent)
    {
        DebugLogWrite("ERROR", "SOLID_CACHE_WARM", "QUEUE_FAIL", "archive=%s | start_index=%u | reason=wake_event_missing", archivePath, (unsigned int)startIndex);

        return;
    }

    EnterCriticalSection(&g_solid7zCacheWarmerLock);
    bool activeSameArchive = !g_solid7zActiveArchivePath.empty() && _stricmp(g_solid7zActiveArchivePath.c_str(), archivePath) == 0;
    bool queuedSameArchive = !g_solid7zWarmArchivePath.empty() && _stricmp(g_solid7zWarmArchivePath.c_str(), archivePath) == 0;
    bool completedSameArchive = !g_solid7zCompletedArchivePath.empty() && _stricmp(g_solid7zCompletedArchivePath.c_str(), archivePath) == 0;

    if (completedSameArchive && !activeSameArchive && !queuedSameArchive)
    {
        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_SKIP", "archive=%s | start_index=%u | reason=archive_already_fully_warmed", archivePath, (unsigned int)startIndex);

        return;
    }

    if (activeSameArchive)
    {
        g_solid7zPriorityIndex = startIndex;
        g_solid7zPriorityPending = true;
        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "PRIORITY_SET", "archive=%s | index=%u | reason=same_archive_active", archivePath, (unsigned int)startIndex);
        SetEvent(g_solid7zCacheWarmerWakeEvent);
        return;
    }

    if (queuedSameArchive)
    {
        g_solid7zWarmStartIndex = startIndex;
        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_UPDATE", "archive=%s | start_index=%u | reason=same_archive_already_queued", archivePath, (unsigned int)startIndex);
        SetEvent(g_solid7zCacheWarmerWakeEvent);
        return;
    }

    if (!g_solid7zCompletedArchivePath.empty() && _stricmp(g_solid7zCompletedArchivePath.c_str(), archivePath) != 0)
    {
        g_solid7zCompletedArchivePath.clear();
    }

    g_solid7zWarmArchivePath = archivePath;
    g_solid7zWarmStartIndex = startIndex;
    LeaveCriticalSection(&g_solid7zCacheWarmerLock);
    DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_SET", "archive=%s | start_index=%u", archivePath, (unsigned int)startIndex);
    SetEvent(g_solid7zCacheWarmerWakeEvent);
}

// -----------------------------------------------------------------------------
// TakeSolid7zCacheWarmJob
//
// Copies the latest queued solid 7z cache-warm request into local variables.
//
// Only the latest archive request is kept. This avoids building a large queue of
// stale cache-warm work if the user quickly switches archives or starts playback
// from a different solid archive.
// -----------------------------------------------------------------------------
static bool TakeSolid7zCacheWarmJob(std::string& outArchivePath, mz_uint& outStartIndex)
{
    outArchivePath.clear();
    outStartIndex = 0;

    EnsureSolid7zCacheWarmerLocks();

    EnterCriticalSection(&g_solid7zCacheWarmerLock);

    if (!g_solid7zWarmArchivePath.empty())
    {
        outArchivePath = g_solid7zWarmArchivePath;
        outStartIndex = g_solid7zWarmStartIndex;
        g_solid7zWarmArchivePath.clear();
    }

    if (g_solid7zWarmArchivePath.empty() && g_solid7zCacheWarmerWakeEvent)
    {
        ResetEvent(g_solid7zCacheWarmerWakeEvent);
    }

    LeaveCriticalSection(&g_solid7zCacheWarmerLock);

    return !outArchivePath.empty();
}

// -----------------------------------------------------------------------------
// TakeSolid7zPriorityIndex
//
// Takes a pending priority index for the currently active solid 7z archive.
//
// This lets a user skip promote the selected track without restarting the whole
// cache-warm pass. The worker checks this between entries and warms the promoted
// entry next if it is not already cached.
//
// Returns true only when a priority entry belongs to the active archive.
// -----------------------------------------------------------------------------
static bool TakeSolid7zPriorityIndex(const char* activeArchivePath, mz_uint& outPriorityIndex)
{
    outPriorityIndex = 0;

    if (!activeArchivePath || !activeArchivePath[0])
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "PRIORITY_TAKE_FAIL", "reason=missing_active_archive");
        return false;
    }

    EnsureSolid7zCacheWarmerLocks();

    EnterCriticalSection(&g_solid7zCacheWarmerLock);

    bool hasPriority =
        g_solid7zPriorityPending &&
        !g_solid7zActiveArchivePath.empty() &&
        _stricmp(g_solid7zActiveArchivePath.c_str(), activeArchivePath) == 0;

    if (hasPriority)
    {
        outPriorityIndex = g_solid7zPriorityIndex;
        g_solid7zPriorityPending = false;
    }

    LeaveCriticalSection(&g_solid7zCacheWarmerLock);

    return hasPriority;
}

// -----------------------------------------------------------------------------
// FindArchiveAudioEntryByIndex
//
// Finds an audio entry in an already listed archive-entry vector.
//
// This is used by the solid 7z cache warmer when playback/skipping promotes a
// specific archive index. Keeping this as a local vector lookup avoids relisting
// the archive just to service a priority request.
// -----------------------------------------------------------------------------
static bool FindArchiveAudioEntryByIndex(const std::vector<ArchiveAudioEntry>& entries, mz_uint index, ArchiveAudioEntry& outEntry)
{
    for (const ArchiveAudioEntry& entry : entries)
    {
        if (entry.index == index)
        {
            outEntry = entry;
            return true;
        }
    }

    return false;
}

// -----------------------------------------------------------------------------
// Solid7zCacheWarmOneEntry
//
// Extracts one solid 7z audio entry to the managed cache if it is not already
// cached, then learns metadata from that cache file and refreshes the Winamp
// playlist row.
//
// This function deliberately uses the same managed cache path as playback so the
// extraction cost produces reusable output instead of temporary metadata-only
// buffers. All extraction failures are logged because they affect future skip/play
// responsiveness, even though playback can still retry on demand later.
// -----------------------------------------------------------------------------
static bool Solid7zCacheWarmOneEntry(const char* archivePath, const ArchiveAudioEntry& entry)
{
    if (!archivePath || !archivePath[0] || entry.name.empty())
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "ENTRY_SKIP", "archive=%s | index=%u | reason=invalid_entry", archivePath ? archivePath : "", (unsigned int)entry.index);
        return false;
    }

    bool isSolid = false;
    mz_uint32 folderIndex = 0;
    mz_uint32 filesInFolder = 0;

    if (!ArchiveGetEntrySolidInfo(archivePath, entry.index, &isSolid, &folderIndex, &filesInFolder))
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "ENTRY_SKIP", "archive=%s | index=%u | entry=%s | reason=solid_info_failed", archivePath, (unsigned int)entry.index, entry.name.c_str());
        return false;
    }

    if (!isSolid)
    {
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "ENTRY_SKIP", "archive=%s | index=%u | entry=%s | reason=entry_not_solid", archivePath, (unsigned int)entry.index, entry.name.c_str());
        return false;
    }

    char cachePath[MAX_PATH * 4];
    cachePath[0] = '\0';

    if (ArchiveFindExistingCacheFile(archivePath, entry.index, entry.name.c_str(), cachePath, sizeof(cachePath)))
    {
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "CACHE_HIT", "archive=%s | index=%u | entry=%s | cache=%s", archivePath, (unsigned int)entry.index, entry.name.c_str(), cachePath);
    }
    else
    {
        AudioFormat format = GetAudioFormatFromPath(entry.name.c_str());

        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "CACHE_BUILD_START", "archive=%s | index=%u | entry=%s | audio_type=%s | size=%llu | folder_index=%u | files_in_folder=%u", archivePath, (unsigned int)entry.index, entry.name.c_str(), AudioFormatToLogName(format), (unsigned long long)entry.uncompSize, (unsigned int)folderIndex, (unsigned int)filesInFolder);

        EnterCriticalSection(&g_solid7zExtractionLock);

        bool extracted = ArchiveExtractEntryToCacheFile(archivePath, entry.index, entry.name.c_str(), cachePath, sizeof(cachePath));

        LeaveCriticalSection(&g_solid7zExtractionLock);

        if (!extracted)
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "CACHE_BUILD_FAIL", "archive=%s | index=%u | entry=%s | audio_type=%s | size=%llu | reason=extract_to_cache_failed", archivePath, (unsigned int)entry.index, entry.name.c_str(), AudioFormatToLogName(format), (unsigned long long)entry.uncompSize);
            return false;
        }

        if (!cachePath[0])
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "CACHE_BUILD_FAIL", "archive=%s | index=%u | entry=%s | audio_type=%s | size=%llu | reason=empty_cache_path", archivePath, (unsigned int)entry.index, entry.name.c_str(), AudioFormatToLogName(format), (unsigned long long)entry.uncompSize);
            return false;
        }

        if (!ArchiveCacheFileExists(cachePath, entry.uncompSize))
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "CACHE_BUILD_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu | reason=cache_file_missing_or_size_mismatch", archivePath, (unsigned int)entry.index, entry.name.c_str(), cachePath, (unsigned long long)entry.uncompSize);
            return false;
        }

        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "CACHE_BUILD_DONE", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", archivePath, (unsigned int)entry.index, entry.name.c_str(), cachePath, (unsigned long long)entry.uncompSize);
    }

    int lengthMs = -1000;

    if (GetAudioDurationMsFromArchiveEntryBackground(archivePath, entry.index, entry.uncompSize, entry.name.c_str(), &lengthMs) && lengthMs > 0)
    {
        std::string virtualPath;

        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "METADATA_DONE", "archive=%s | index=%u | entry=%s | length_ms=%d", archivePath, (unsigned int)entry.index, entry.name.c_str(), lengthMs);

        if (BuildVirtualPathForCacheWarmRefresh(archivePath, entry.index, entry.name.c_str(), virtualPath))
        {
            RefreshPlaylistCacheForVirtualPath(virtualPath.c_str());
        }
    }
    else
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "METADATA_FAIL", "archive=%s | index=%u | entry=%s | reason=duration_lookup_failed_after_cache", archivePath, (unsigned int)entry.index, entry.name.c_str());
    }

    return true;
}



// -----------------------------------------------------------------------------
// Solid7zCacheWarmBatchEntries
//
// Warms multiple solid 7z entries through the cache layer in one backend batch.
//
// This is Stage 3 of the solid 7z cache warmer. Instead of calling
// ArchiveExtractEntryToCacheFile() once per track, it asks the cache layer to
// build all cache paths and asks the 7z backend to keep one archive session open
// while extracting all missing entries. For one-solid-block albums, this avoids
// repeatedly opening the archive and repeatedly decoding the same solid block.
//
// Metadata is learned after cache creation and each playlist row is refreshed.
// Individual metadata failures are logged but do not fail the batch.
// -----------------------------------------------------------------------------
static bool Solid7zCacheWarmBatchEntries(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries)
{
    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "BATCH_FAIL", "reason=missing_archive_path");
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "BATCH_SKIP", "archive=%s | reason=no_entries", archivePath);
        return true;
    }

    DebugLogWrite("INFO", "SOLID_CACHE_WARM", "BATCH_START", "archive=%s | entries=%zu", archivePath, entries.size());

    std::vector<ArchiveCacheWarmResult> results;

    EnterCriticalSection(&g_solid7zExtractionLock);

    bool cacheOk = false;

    if (IsRarArchivePath(archivePath))
    {
        cacheOk = ArchiveCacheWarmRarSolidEntries(archivePath, entries, results);
    }
    else
    {
        cacheOk = ArchiveCacheWarmSevenZipSolidEntries(archivePath, entries, results);
    }

    LeaveCriticalSection(&g_solid7zExtractionLock);

    if (!cacheOk)
    {
        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "BATCH_CACHE_FAIL", "archive=%s | entries=%zu | reason=cache_layer_returned_false", archivePath, entries.size());
        return false;
    }

    size_t metadataOkCount = 0;
    size_t cachedCount = 0;

    for (const ArchiveCacheWarmResult& result : results)
    {
        if (!result.cached)
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "BATCH_ENTRY_NOT_CACHED", "archive=%s | index=%u | reason=cache_result_false", archivePath, (unsigned int)result.index);
            continue;
        }

        ArchiveAudioEntry entry;

        if (!FindArchiveAudioEntryByIndex(entries, result.index, entry))
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "BATCH_METADATA_SKIP", "archive=%s | index=%u | reason=entry_not_found_after_cache", archivePath, (unsigned int)result.index);
            continue;
        }

        ++cachedCount;

        int lengthMs = -1000;

        if (GetAudioDurationMsFromArchiveEntryBackground(archivePath, entry.index, entry.uncompSize, entry.name.c_str(), &lengthMs) && lengthMs > 0)
        {
            std::string virtualPath;

            ++metadataOkCount;

            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "BATCH_METADATA_DONE", "archive=%s | index=%u | entry=%s | length_ms=%d | cache=%s", archivePath, (unsigned int)entry.index, entry.name.c_str(), lengthMs, result.cachePath);

            if (BuildVirtualPathForCacheWarmRefresh(archivePath, entry.index, entry.name.c_str(), virtualPath))
            {
                RefreshPlaylistCacheForVirtualPath(virtualPath.c_str());
            }
        }
        else
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "BATCH_METADATA_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | reason=duration_lookup_failed_after_batch_cache", archivePath, (unsigned int)entry.index, entry.name.c_str(), result.cachePath);
        }
    }

    DebugLogWrite("INFO", "SOLID_CACHE_WARM", "BATCH_DONE", "archive=%s | entries=%zu | cached=%zu | metadata_ok=%zu", archivePath, entries.size(), cachedCount, metadataOkCount);

    return cachedCount > 0;
}

// -----------------------------------------------------------------------------
// Solid7zCacheWarmerProc
//
// Background worker for solid 7z cache warming.
//
// The worker processes one archive at a time. For the active archive, it warms
// entries after the selected track first, then wraps to earlier entries. If the
// user skips within the same archive, QueueSolid7zCacheWarm() records a priority
// index; this worker handles that index next without restarting the whole archive
// pass.
//
// Work remains sequential because solid archive extraction is expensive and
// parallel extraction usually hurts playback responsiveness.
// -----------------------------------------------------------------------------
static DWORD WINAPI Solid7zCacheWarmerProc(LPVOID param)
{
    (void)param;

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

    while (InterlockedCompareExchange(&g_solid7zCacheWarmerStop, 0, 0) == 0)
    {
        if (!g_solid7zCacheWarmerWakeEvent)
        {
            Sleep(250);
            continue;
        }

        WaitForSingleObject(g_solid7zCacheWarmerWakeEvent, 1000);

        if (InterlockedCompareExchange(&g_solid7zCacheWarmerStop, 0, 0) != 0)
        {
            break;
        }

        std::string archivePath;
        mz_uint startIndex = 0;

        if (!TakeSolid7zCacheWarmJob(archivePath, startIndex))
        {
            continue;
        }

        EnterCriticalSection(&g_solid7zCacheWarmerLock);
        g_solid7zActiveArchivePath = archivePath;
        g_solid7zPriorityPending = false;
        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
        std::vector<ArchiveAudioEntry> entries;

        if (!ArchiveListAudioEntries(archivePath.c_str(), entries))
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "LIST_FAIL", "archive=%s | start_index=%u | reason=list_audio_entries_failed", archivePath.c_str(), (unsigned int)startIndex);
            EnterCriticalSection(&g_solid7zCacheWarmerLock);

            if (_stricmp(g_solid7zActiveArchivePath.c_str(), archivePath.c_str()) == 0)
            {
                g_solid7zActiveArchivePath.clear();
                g_solid7zPriorityPending = false;
            }
            LeaveCriticalSection(&g_solid7zCacheWarmerLock);

            continue;
        }

        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "START", "archive=%s | start_index=%u | audio_entries=%zu", archivePath.c_str(), (unsigned int)startIndex, entries.size());

        std::vector<ArchiveAudioEntry> orderedEntries;
        orderedEntries.reserve(entries.size());

        for (const ArchiveAudioEntry& entry : entries)
        {
            if (entry.index > startIndex)
            {
                orderedEntries.push_back(entry);
            }
        }

        for (const ArchiveAudioEntry& entry : entries)
        {
            if (entry.index < startIndex)
            {
                orderedEntries.push_back(entry);
            }
        }

        if (InterlockedCompareExchange(&g_solid7zCacheWarmerStop, 0, 0) == 0)
        {
            std::string pendingArchive;

            EnterCriticalSection(&g_solid7zCacheWarmerLock);

            if (!g_solid7zWarmArchivePath.empty())
            {
                pendingArchive = g_solid7zWarmArchivePath;
            }

            LeaveCriticalSection(&g_solid7zCacheWarmerLock);

            if (!pendingArchive.empty() && _stricmp(pendingArchive.c_str(), archivePath.c_str()) != 0)
            {
                DebugLogWrite("INFO", "SOLID_CACHE_WARM", "ABORT_FOR_NEW_ARCHIVE", "current_archive=%s | pending_archive=%s", archivePath.c_str(), pendingArchive.c_str());
            }
            else
            {
                mz_uint priorityIndex = 0;

                if (TakeSolid7zPriorityIndex(archivePath.c_str(), priorityIndex))
                {
                    ArchiveAudioEntry priorityEntry;

                    if (FindArchiveAudioEntryByIndex(entries, priorityIndex, priorityEntry))
                    {
                        std::vector<ArchiveAudioEntry> priorityFirst;
                        priorityFirst.reserve(orderedEntries.size() + 1);
                        priorityFirst.push_back(priorityEntry);

                        for (const ArchiveAudioEntry& entry : orderedEntries)
                        {
                            if (entry.index != priorityEntry.index)
                            {
                                priorityFirst.push_back(entry);
                            }
                        }

                        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "PRIORITY_BATCH_RUN", "archive=%s | index=%u | entry=%s | batch_entries=%zu", archivePath.c_str(), (unsigned int)priorityEntry.index, priorityEntry.name.c_str(), priorityFirst.size());

                        if (Solid7zCacheWarmBatchEntries(archivePath.c_str(), priorityFirst))
                        {
                            EnterCriticalSection(&g_solid7zCacheWarmerLock);
                            g_solid7zCompletedArchivePath = archivePath;
                            LeaveCriticalSection(&g_solid7zCacheWarmerLock);
                            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "COMPLETE_MARK", "archive=%s | reason=priority_batch_success", archivePath.c_str());
                        }
                    }
                    else
                    {
                        DebugLogWrite("WARN", "SOLID_CACHE_WARM", "PRIORITY_SKIP", "archive=%s | index=%u | reason=entry_not_found", archivePath.c_str(), (unsigned int)priorityIndex);

                        if (Solid7zCacheWarmBatchEntries(archivePath.c_str(), orderedEntries))
                        {
                            EnterCriticalSection(&g_solid7zCacheWarmerLock);
                            g_solid7zCompletedArchivePath = archivePath;
                            LeaveCriticalSection(&g_solid7zCacheWarmerLock);
                            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "COMPLETE_MARK", "archive=%s | reason=priority_skip_fallback_batch_success", archivePath.c_str());
                        }
                    }
                }
                else
                {
                    if (Solid7zCacheWarmBatchEntries(archivePath.c_str(), orderedEntries))
                    {
                        EnterCriticalSection(&g_solid7zCacheWarmerLock);
                        g_solid7zCompletedArchivePath = archivePath;
                        LeaveCriticalSection(&g_solid7zCacheWarmerLock);

                        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "COMPLETE_MARK", "archive=%s | reason=batch_success", archivePath.c_str());
                    }
                }
            }
        }

        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "DONE", "archive=%s | start_index=%u", archivePath.c_str(), (unsigned int)startIndex);

        EnterCriticalSection(&g_solid7zCacheWarmerLock);

        if (_stricmp(g_solid7zActiveArchivePath.c_str(), archivePath.c_str()) == 0)
        {
            g_solid7zActiveArchivePath.clear();
            g_solid7zPriorityPending = false;
        }

        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
    }

    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);

    return 0;
}

// -----------------------------------------------------------------------------
// StopSolid7zCacheWarmer
//
// Stops the solid 7z cache warmer during plugin shutdown.
//
// The worker may be inside a backend extraction call, so shutdown waits briefly
// but avoids force-terminating the thread. If extraction is still active after the
// timeout, the handle is closed and the worker will exit on its next stop check.
// -----------------------------------------------------------------------------
static void StopSolid7zCacheWarmer()
{
    InterlockedExchange(&g_solid7zCacheWarmerStop, 1);

    if (g_solid7zCacheWarmerWakeEvent)
    {
        SetEvent(g_solid7zCacheWarmerWakeEvent);
    }

    if (g_solid7zCacheWarmerThread)
    {
        DWORD waitResult = WaitForSingleObject(g_solid7zCacheWarmerThread, 3000);

        if (waitResult == WAIT_TIMEOUT)
        {
            DebugLogWrite("WARN", "SOLID_CACHE_WARM", "THREAD_STOP_TIMEOUT", "reason=worker_busy_or_extracting");
        }

        CloseHandle(g_solid7zCacheWarmerThread);
        g_solid7zCacheWarmerThread = NULL;
    }

    if (g_solid7zCacheWarmerWakeEvent)
    {
        CloseHandle(g_solid7zCacheWarmerWakeEvent);
        g_solid7zCacheWarmerWakeEvent = NULL;
    }

    if (g_solid7zCacheWarmerLockInitialized)
    {
        EnterCriticalSection(&g_solid7zCacheWarmerLock);
        g_solid7zWarmArchivePath.clear();
        g_solid7zActiveArchivePath.clear();
        g_solid7zCompletedArchivePath.clear();
        g_solid7zPriorityPending = false;
        LeaveCriticalSection(&g_solid7zCacheWarmerLock);
    }
}

// -----------------------------------------------------------------------------
// ShouldSkipBackgroundMetadataForSolid7z
//
// Decides whether a playlist/GetFileInfo metadata miss should avoid the generic
// background metadata worker.
//
// For solid 7z archives, the generic metadata worker is expensive because it can
// extract/decode entries only to learn duration, while playback will later extract
// the same entry again to the managed cache. Solid 7z entries should instead let
// the playback cache route, and later the solid archive cache-warmer, populate
// metadata from reusable cache files.
//
// Returns true only for confirmed solid 7z entries. Any backend lookup failure is
// logged and treated as "do not skip" so non-solid and unknown cases preserve the
// old behaviour.
// -----------------------------------------------------------------------------
static bool ShouldSkipBackgroundMetadataForSolid7z(const char* archivePath, mz_uint index, const char* virtualPath)
{
    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("WARN", "METADATA_BG", "SOLID_7Z_QUEUE_CHECK_FAIL", "path=%s | index=%u | reason=missing_archive_path", virtualPath ? virtualPath : "", (unsigned int)index);
        return false;
    }

    if (!IsSevenZipArchivePath(archivePath) && !IsRarArchivePath(archivePath))
    {
        return false;
    }

    bool isSolid = false;
    mz_uint32 folderIndex = 0;
    mz_uint32 filesInFolder = 0;

    if (!ArchiveGetEntrySolidInfo(archivePath, index, &isSolid, &folderIndex, &filesInFolder))
    {
        DebugLogWrite("WARN", "METADATA_BG", "SOLID_7Z_QUEUE_CHECK_FAIL", "archive=%s | index=%u | path=%s | reason=get_entry_solid_info_failed", archivePath, (unsigned int)index, virtualPath ? virtualPath : "");
        return false;
    }

    if (!isSolid)
    {
        return false;
    }

    return true;
}

static void GetFileInfo(const char* file, char* title, int* length_in_ms)
{
    /*
        Virtual archive playlist entries must be handled first.
        Do NOT use g_currentTitle / g_currentLengthMs for these,
        otherwise every archive track will show the currently playing track's info.

        Winamp playlist lookup -> stable title only, no archive open, no extraction, no stream open, no cache lookup Playback/explicit expansion -> still allowed to do archive work
    */
    if (file && IsVirtualArchivePath(file))
    {
        char archivePath[MAX_PATH * 4];
        char internalPath[1024];
        mz_uint index = 0;

        archivePath[0] = '\0';
        internalPath[0] = '\0';

        bool parsed = ParseVirtualDisplayPath(file, archivePath, sizeof(archivePath), &index, internalPath, sizeof(internalPath));

        if (title)
        {
            if (parsed)
            {
                FormatStableVirtualTitle(file, internalPath, index, title, 1024);
            }
            else
            {
                CopyBaseName(title, 1024, file);
            }
        }

        if (length_in_ms)
        {
            int metadataLengthMs = -1000;

            if (GetVirtualArchiveEntryInfoCacheOnly(file, NULL, 0, &metadataLengthMs) && metadataLengthMs > 0)
            {
                *length_in_ms = metadataLengthMs;
                DebugLogWrite("INFO", "METADATA", "PLAYLIST_INFO_DURATION", "file=%s | parsed=%d | archive=%s | index=%u | length_ms=%d | source=metadata_cache_only", file ? file : "", parsed ? 1 : 0, archivePath, (unsigned int)index, metadataLengthMs);
            }
            else if (parsed && g_currentLengthMs > 0 && g_currentArchivePath[0] && _stricmp(archivePath, g_currentArchivePath) == 0 && index == g_currentArchiveIndex)
            {
                *length_in_ms = g_currentLengthMs;
                DebugLogWrite("INFO", "METADATA", "PLAYLIST_INFO_DURATION", "file=%s | parsed=%d | archive=%s | index=%u | length_ms=%d | source=current_playing_track", file ? file : "", parsed ? 1 : 0, archivePath, (unsigned int)index, g_currentLengthMs);
            }
            else
            {
                *length_in_ms = -1000;

                const char* missReason = "no_metadata_available_not_parsed";

                if (parsed)
                {
                    if (ShouldSkipBackgroundMetadataForSolid7z(archivePath, index, file))
                    {
                        missReason = IsRarArchivePath(archivePath) ? "no_metadata_available_solid_rar_background_skipped" : "no_metadata_available_solid_7z_background_skipped";
                    }
                    else
                    {
                        QueueBackgroundMetadataLookup(file);
                        missReason = "no_metadata_available_queued_background";
                    }
                }

                if (strcmp(missReason, "no_metadata_available_solid_rar_background_skipped") != 0 && strcmp(missReason, "no_metadata_available_solid_7z_background_skipped") != 0)
                {
                    DebugLogWrite("INFO", "METADATA", "PLAYLIST_INFO_DURATION_MISS", "file=%s | parsed=%d | archive=%s | index=%u | reason=%s", file ? file : "", parsed ? 1 : 0, archivePath, (unsigned int)index, missReason);
                }
            }
        }

        return;
    }

    /*
        Plain archive placeholder rows should not borrow the currently playing
        track length. That made archive rows show wrong metadata before expansion.

        We intentionally leave length unknown here. Calculating the real archive
        total would require scanning/decoding entries before the user expands it,
        which duplicates work and slows normal playlist loading.
    */
    if (file && file[0] && IsSupportedArchivePath(file))
    {
        if (title)
        {
            char tempTitle[1024];
            CopyBaseName(tempTitle, sizeof(tempTitle), file);
            lstrcpynA(title, tempTitle, 1024);
        }

        /*
            Plain archive placeholders should show no duration in the playlist.

            Do not return 0 here. 0 means a real zero-second track and Winamp displays
            0:00. Unknown length should be -1000.
        */
        if (length_in_ms)
            *length_in_ms = -1000;

        return;
    }

    if (length_in_ms) {
        if (g_currentLengthMs > 0) *length_in_ms = g_currentLengthMs;
        else *length_in_ms = -1000;
    }

    if (!title) return;

    if (file && file[0]) {
        char tempTitle[1024];
        CopyBaseName(tempTitle, sizeof(tempTitle), file);
        lstrcpynA(title, tempTitle, 1024);
        return;
    }

    if (g_currentTitle[0])
    {
        lstrcpynA(title, g_currentTitle, 2048);
        return;
    }

    lstrcpynA(title, "Archive Stream", 2048);
}

static const char* GetArchiveStreamAboutText()
{
    return
        "Archive Stream Player\r\n"
        "\r\n"
        "Purpose\r\n"
        "-------\r\n"
        "A Winamp archive-player plugin system that lets Winamp treat supported archives like playlists.\r\n"
        "Archives are expanded into virtual playlist entries, and supported audio files are played directly from the archive where possible.\r\n"
        "\r\n"
        "Plugin components\r\n"
        "-----------------\r\n"
        "in_archive_stream.dll\r\n"
        "  Winamp input plugin. Handles playback of virtual archive entries.\r\n"
        "\r\n"
        "gen_archive_stream.dll\r\n"
        "  Winamp general-purpose plugin. Expands archives in the playlist.\r\n"
        "\r\n"
        "archive_backend_test.exe\r\n"
        "  Console/backend test tool used during development.\r\n"
        "\r\n"
        "Archive support\r\n"
        "---------------\r\n"
        "ZIP\r\n"
        "  Small entries: memory playback.\r\n"
        "  Large entries: true streaming playback.\r\n"
        "\r\n"
        "7Z\r\n"
        "  Small entries: memory playback.\r\n"
        "  Large non-solid Copy/LZMA2 entries: true streaming playback.\r\n"
        "  Solid or unsupported 7Z entries: managed cache fallback.\r\n"
        "\r\n"
        "RAR\r\n"
        "  Small entries: memory playback.\r\n"
        "  Large non-solid entries: progressive streaming playback using UnRAR callback data.\r\n"
        "  Solid, encrypted, split-volume, or unsupported RAR entries: managed cache fallback or clean failure.\r\n"
        "\r\n"
        "Audio playback support\r\n"
        "----------------------\r\n"
        "Native built-in decoders:\r\n"
        "  MP3, WAV, FLAC, OGG\r\n"
        "\r\n"
        "Media Foundation decoder path:\r\n"
        "  M4A tested and working.\r\n"
        "  AAC and WMA are supported through the same Media Foundation path when Windows codecs are present.\r\n"
        "\r\n"
        "Playback and cache behaviour\r\n"
        "----------------------------\r\n"
        "The plugin streams from archives where possible.\r\n"
        "The managed cache is used only for formats/archive layouts that cannot safely stream.\r\n"
        "The cache has configurable maximum size, Clear Cache, and View Cache controls.\r\n"
        "\r\n"
        "Known limits\r\n"
        "------------\r\n"
        "7Z solid archives are not true-random-access streamable and use cache fallback.\r\n"
        "RAR solid archives may depend on earlier decompression history and use cache fallback.\r\n"
        "RAR password-list and multipart handling paths exist, but need regression testing.\r\n"
        "Encrypted, solid or split-volume RAR entries may require cache fallback or fail cleanly.\r\n"
        "AAC/WMA require Windows Media Foundation codec support.\r\n"
        "Windows N editions may require the Microsoft Media Feature Pack.\r\n"
        "\r\n"
        "Project dependencies / source libraries\r\n"
        "---------------------------------------\r\n"
        "libarchive\r\n"
        "  Used for ZIP listing/extraction and selected RAR listing and metadata paths.\r\n"
        "  Source: https://libarchive.org\r\n"
        "\r\n"
        "7-Zip LZMA SDK\r\n"
        "  Used for 7Z listing/extraction and 7Z Copy/LZMA2 non-solid streaming.\r\n"
        "  Version used/current upstream checked during development: 26.01.\r\n"
        "  Source: https://www.7-zip.org/sdk.html\r\n"
        "\r\n"
        "Official UnRAR source / RARLAB\r\n"
        "  Used for RAR listing, extraction, and first-pass non-solid RAR streaming via callback data.\r\n"
        "  Extraction-only use. The UnRAR license does not allow recreating the RAR compression algorithm.\r\n"
        "  Source: https://www.rarlab.com\r\n"
        "  License: https://www.rarlab.com/license.htm\r\n"
        "\r\n"
        "dr_libs\r\n"
        "  Used for native MP3, WAV, and FLAC decoding.\r\n"
        "  Source: https://github.com/mackron/dr_libs\r\n"
        "\r\n"
        "stb_vorbis\r\n"
        "  Used for native OGG/Vorbis decoding.\r\n"
        "  Source: https://github.com/nothings/stb\r\n"
        "\r\n"
        "Microsoft Media Foundation\r\n"
        "  Used for M4A/AAC/WMA decoding through Windows' media stack.\r\n"
        "  Source/docs: https://learn.microsoft.com/windows/win32/medfound/media-foundation-start-page\r\n"
        "\r\n"
        "Build/package notes\r\n"
        "-------------------\r\n"
        "No external 7z.dll is required.\r\n"
        "No external UnRAR DLL is required; UnRAR is linked through unrar_static.lib.\r\n"
        "No Winamp M4A/AAC/WMA input plugin is required for those formats; decoding is handled inside in_archive_stream through Media Foundation.\r\n"
        "\r\n"
        "Developer\r\n"
        "---------\r\n"
        "Developed by Arden\r\n"
        "Website: https://arden.ie\r\n";
}

static LRESULT CALLBACK ArchiveAboutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", GetArchiveStreamAboutText(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 12, 12, 660, 410, hwnd, (HMENU)IDC_ABOUT_TEXT, g_hInstance, NULL);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hArden = CreateWindowExA(0, "BUTTON", "Open arden.ie", WS_CHILD | WS_VISIBLE, 12, 435, 120, 28, hwnd, (HMENU)IDC_ABOUT_ARDEN_BUTTON, g_hInstance, NULL);
        SendMessage(hArden, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hClose = CreateWindowExA(0, "BUTTON", "Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 590, 435, 82, 28, hwnd, (HMENU)IDC_ABOUT_CLOSE_BUTTON, g_hInstance, NULL);
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

static int InfoBox(const char* file, HWND hwndParent)
{
    return 0;
}

static void ClearPrevalidatedStream()
{
    if (InterlockedExchange(&g_prevalidatedStreamReady, 0) != 0)
    {
        ArchiveCloseEntryStream(&g_prevalidatedStream);
    }

    memset(&g_prevalidatedStream, 0, sizeof(g_prevalidatedStream));
}
// -----------------------------------------------------------------------------
// ExtractEntryToPlaybackCache
//
// Extracts one archive entry into the managed cache and verifies that the cache
// file exists before playback starts. This is used when a route has deliberately
// chosen CACHE, especially for solid archives where BUFFER would waste the same
// extraction work without leaving a reusable file behind.
//
// Returns true only when the cache file path is valid and the cached file exists.
// All failure paths are logged because cache extraction failures directly block
// playback.
// -----------------------------------------------------------------------------
static bool ExtractEntryToPlaybackCache(const char* archivePath, mz_uint index, const char* internalName, AudioFormat selectedFormat, mz_uint64 uncompressedSize, const char* routeReason, char* outCachePath, size_t outCachePathSize)
{
    if (!archivePath || !archivePath[0] || !internalName || !internalName[0] || !outCachePath || outCachePathSize == 0)
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_FAIL", "archive=%s | index=%u | entry=%s | audio_type=%s | reason=invalid_input | route_reason=%s", archivePath ? archivePath : "", (unsigned int)index, internalName ? internalName : "", AudioFormatToLogName(selectedFormat), routeReason ? routeReason : "");
        return false;
    }

    outCachePath[0] = '\0';

    DebugLogWrite("INFO", "CACHE", "BUILD_START", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompressedSize, routeReason ? routeReason : "");

    bool extractedToCache = false;

    if (IsSevenZipArchivePath(archivePath))
    {
        EnsureSolid7zCacheWarmerLocks();

        DebugLogWrite("INFO", "CACHE", "BUILD_LOCK_WAIT", "archive=%s | archive_type=%s | index=%u | entry=%s | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, routeReason ? routeReason : "");

        EnterCriticalSection(&g_solid7zExtractionLock);

        /*
            Re-check the managed cache after waiting for the solid 7z extraction lock.

            The background warmer may have completed this exact entry while playback
            was blocked waiting for the extraction lock. In that case, avoid a second
            extraction attempt and reuse the cache file that was just created.
        */
        if (ArchiveFindExistingCacheFile(archivePath, index, internalName, outCachePath, outCachePathSize))
        {
            extractedToCache = true;
            DebugLogWrite("INFO", "CACHE", "BUILD_SKIP_EXISTING_AFTER_WAIT", "archive=%s | archive_type=%s | index=%u | entry=%s | cache=%s | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, outCachePath, routeReason ? routeReason : "");
        }
        else
        {
            extractedToCache = ArchiveExtractEntryToCacheFile(archivePath, index, internalName, outCachePath, outCachePathSize);
        }

        LeaveCriticalSection(&g_solid7zExtractionLock);
    }
    else
    {
        extractedToCache = ArchiveExtractEntryToCacheFile(archivePath, index, internalName, outCachePath, outCachePathSize);
    }

    if (!extractedToCache)
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_FAIL", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | reason=extract_to_cache_failed | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompressedSize, routeReason ? routeReason : "");
        return false;
    }

    if (!outCachePath[0])
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_FAIL", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | reason=empty_cache_path | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompressedSize, routeReason ? routeReason : "");
        return false;
    }

    if (!ArchiveCacheFileExists(outCachePath, uncompressedSize))
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_FAIL", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | cache=%s | reason=cache_file_missing_or_size_mismatch | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompressedSize, outCachePath, routeReason ? routeReason : "");
        return false;
    }

    DebugLogWrite("INFO", "CACHE", "BUILD_DONE", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | cache=%s | route_reason=%s", archivePath, ArchiveTypeToLogName(archivePath), (unsigned int)index, internalName, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompressedSize, outCachePath, routeReason ? routeReason : "");

    return true;
}

static int Play(const char* fn)
{
    Stop();
    ClearPrevalidatedStream();

    InterlockedExchange(&g_seekRequested, 0);
    InterlockedExchange(&g_seekTargetMs, 0);
    InterlockedExchange(&g_outputTimeBaseMs, 0);
    InterlockedExchange(&g_trackStartTick, GetTickCount());

    char actualZipPath[MAX_PATH * 4];
    char virtualInternalPath[1024];
    mz_uint forcedMp3Index = 0;
    bool usingVirtualPath = false;

    actualZipPath[0] = '\0';
    virtualInternalPath[0] = '\0';

    /*
        Plain archives and virtual archive entries must be routed differently.

        Plain archive:
            C:\Music\Album.rar

        Virtual archive entry:
            C:\Music\Album.rar.archive_stream|3|Track01.mp3
            or VIRTUAL_PREFIX + archivePath + "|" + index + "|" + internalPath

        Do not call ParseVirtualArchivePath() for plain archives.
        Its warning logs are useful and should only fire when something that
        was expected to be virtual is malformed.
    */
    const bool isVirtualPath = fn && IsVirtualArchivePath(fn);

    if (!isVirtualPath)
    {
        strcpy_s(actualZipPath, sizeof(actualZipPath), fn ? fn : "");

        if (IsSupportedArchivePath(actualZipPath))
        {
            //plain archive expansion route log
            ArchivePasswordClearArchiveCancelled(actualZipPath);

            DebugLogWrite("INFO", "ROUTE", "PLAIN_ARCHIVE_EXPAND", "archive=%s | archive_type=%s | route=expand_playlist | multipart_guess=%d | password_list_configured=%d", actualZipPath, ArchiveTypeToLogName(actualZipPath), IsLikelyMultipartArchivePath(actualZipPath) ? 1 : 0, g_rarPasswordListPath[0] ? 1 : 0);

            bool requestedExpand = WinampRequestArchiveExpansion(g_mod.hMainWindow, actualZipPath) ? true : false;

            if (!requestedExpand)
            {
                DebugLogWrite("ERROR", "PLAY", "EXPAND_REQUEST_FAIL", "archive=%s | reason=plain_archive_expand_request_failed", actualZipPath);
                return 1;
            }

            /*
                Important:
                Returning 1 tells Winamp that Play() failed. Winamp may then try to
                continue playback from another playlist position, often index 0.
                That is what causes the first archive to expand after clicking a
                later archive.

                The expansion request succeeded, so report success here. The general
                plugin will rebuild the playlist and start the inserted virtual item
                through the delayed play timer.
            */
            return 0;
        }

        DebugLogWrite("ERROR", "PLAY", "UNSUPPORTED_INPUT", "file=%s | reason=not_virtual_or_supported_archive | zip=%d | sevenzip=%d | rar=%d", fn ? fn : "", IsZipArchivePath(fn) ? 1 : 0, IsSevenZipArchivePath(fn) ? 1 : 0, IsRarArchivePath(fn) ? 1 : 0);

        return 1;
    }

    if (!ParseVirtualArchivePath(fn, actualZipPath, sizeof(actualZipPath), &forcedMp3Index, virtualInternalPath, sizeof(virtualInternalPath)))
    {
        DebugLogWrite("ERROR", "PLAY", "VIRTUAL_PARSE_FAIL", "file=%s | reason=parse_virtual_archive_path_failed", fn ? fn : "");
        return 1;
    }

    /*
        Password cancel reset is route-based.

        For solid RAR, password prompting can happen during cache extraction inside
        Play(), before the normal playback-thread setup resets g_stopRequested. After
        Cancel, Winamp may immediately auto-advance to the next virtual entry. Do not
        clear cancellation for that immediate retry, or it will prompt once per file.

        A later manual Play should clear cancellation and allow another password
        attempt, matching the ZIP route behavior.
    */
    {
        DWORD nowTick = GetTickCount();
        DWORD cancelTick = (DWORD)InterlockedCompareExchange(&g_passwordPromptCancelTick, 0, 0);
        DWORD elapsedSincePasswordCancel = cancelTick ? (nowTick - cancelTick) : 0;
        bool suppressImmediateRetry = InterlockedCompareExchange(&g_stopRequested, 0, 0) != 0 && cancelTick != 0 && elapsedSincePasswordCancel < 750;

        if (!suppressImmediateRetry)
        {
            ArchivePasswordClearArchiveCancelled(actualZipPath);
            InterlockedExchange(&g_passwordPromptCancelTick, 0);
        }
    }
    

    //virtual track route log
    DebugLogWrite("INFO", "ROUTE", "VIRTUAL_TRACK_SELECTED", "archive=%s | archive_type=%s | index=%u | internal=%s | virtual_path=%s | multipart_guess=%d | password_list_configured=%d", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)forcedMp3Index, virtualInternalPath, fn ? fn : "", IsLikelyMultipartArchivePath(actualZipPath) ? 1 : 0, g_rarPasswordListPath[0] ? 1 : 0);

    usingVirtualPath = true;

    if (!g_audioBuffer)
    {
        DebugLogWrite("ERROR", "PLAY", "BUFFER_MISSING", "file=%s", fn ? fn : "");
        MessageBoxA(g_mod.hMainWindow, "Audio buffer was not allocated.", "Archive Stream - Buffer Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    mz_uint mp3Index = 0;
    mz_uint64 uncompSize = 0;
    char firstAudio[1024];

    if (usingVirtualPath)
    {
        mp3Index = forcedMp3Index;
        firstAudio[0] = '\0';

        if (!ArchiveGetEntryInfo(actualZipPath, mp3Index, firstAudio, sizeof(firstAudio), &uncompSize))
        {
            DebugLogWrite("WARN", "PLAY", "ENTRY_INFO_INDEX_FAIL", "archive=%s | index=%u | internal=%s | virtual_path=%s | action=try_path_fallback", actualZipPath, (unsigned int)mp3Index, virtualInternalPath, fn ? fn : "");

            uint32_t fallbackIndex = (uint32_t)mp3Index;

            if (!ArchiveGetEntryInfoByPath(actualZipPath, virtualInternalPath, firstAudio, sizeof(firstAudio), &uncompSize, &fallbackIndex))
            {
                DebugLogWrite("ERROR", "PLAY", "ENTRY_INFO_FAIL", "archive=%s | index=%u | internal=%s | virtual_path=%s | reason=index_and_path_lookup_failed", actualZipPath, (unsigned int)mp3Index, virtualInternalPath, fn ? fn : "");
                MessageBoxA(g_mod.hMainWindow, "Could not read archive entry information for virtual archive track.", "Archive Stream - Archive Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            if (fallbackIndex != (uint32_t)mp3Index)
            {
                DebugLogWrite("WARN", "PLAY", "ENTRY_INDEX_REMAP", "archive=%s | old_index=%u | new_index=%u | internal=%s", actualZipPath, (unsigned int)mp3Index, (unsigned int)fallbackIndex, firstAudio);
                mp3Index = (mz_uint)fallbackIndex;
            }
        }
    }
    else
    {
        if (!ArchiveFindFirstAudioEntry(actualZipPath, &mp3Index, firstAudio, sizeof(firstAudio), &uncompSize))
        {
            DebugLogWrite("ERROR", "PLAY", "NO_AUDIO_FOUND", "archive=%s | reason=find_first_audio_failed", actualZipPath);
            MessageBoxA(g_mod.hMainWindow, "Archive opened, but no supported audio file was found, or the archive could not be read.", "Archive Stream - Archive Scan", MB_OK | MB_ICONWARNING);
            return 1;
        }
    }
    //selected entry info log
    DebugLogWrite("INFO", "ROUTE", "ENTRY_SELECTED", "archive=%s | archive_type=%s | index=%u | entry=%s | size=%llu | buffer_size=%zu | fits_buffer=%d", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, (unsigned long long)uncompSize, g_audioBufferSize, uncompSize <= g_audioBufferSize ? 1 : 0);

    AudioFormat selectedFormat = GetAudioFormatFromPath(firstAudio);

    if (selectedFormat == AUDIO_FORMAT_UNKNOWN)
    {
        DebugLogWrite("ERROR", "PLAY", "UNSUPPORTED_AUDIO", "archive=%s | index=%u | entry=%s", actualZipPath, (unsigned int)mp3Index, firstAudio);
        MessageBoxA(g_mod.hMainWindow, "Unsupported audio format.", "Archive Stream", MB_OK | MB_ICONERROR);
        return 1;
    }

    ArchivePlaybackMode playbackMode = ARCHIVE_PLAYBACK_MEMORY;

    char cachePath[MAX_PATH * 4];
    cachePath[0] = '\0';

    bool existingCacheHit = ArchiveFindExistingCacheFile(actualZipPath, mp3Index, firstAudio, cachePath, sizeof(cachePath));

    /*
        Use shared archive-path detection here instead of final-extension checks.

        Multipart 7z first volumes end in ".7z.001", so strrchr(path, '.') returns
        ".001" and a plain final-extension comparison incorrectly marks the archive as
        not-7z. That causes large entries to fall into the no_stream_or_cache_path
        error even though the backend can now open the split 7z set.

        The shared helpers know about:
            .zip
            .7z
            .7z.001
            .rar
    */
    bool archiveIsZip = IsZipArchivePath(actualZipPath);
    bool archiveIs7z = IsSevenZipArchivePath(actualZipPath);
    bool archiveIsRar = IsRarArchivePath(actualZipPath);

    DebugLogWrite("INFO", "ROUTE", "ARCHIVE_TYPE_CHECK", "archive=%s | zip=%d | sevenzip=%d | rar=%d | archive_type=%s", actualZipPath, archiveIsZip ? 1 : 0, archiveIs7z ? 1 : 0, archiveIsRar ? 1 : 0, ArchiveTypeToLogName(actualZipPath));


    bool selectedSolidKnown = false;
    bool selectedIsSolid = false;
    mz_uint32 selectedSolidFolderIndex = 0;
    mz_uint32 selectedSolidFilesInFolder = 0;
    bool queueSolidCacheWarmAfterPlaybackStart = false;

    /*
        Solid 7z route probe.

        Solid 7z entries should not use BUFFER as the normal playback route.
        Even when the file fits the configured memory buffer, extracting to memory
        wastes the decompression work because the next play/skip/metadata lookup may
        need to walk the same solid block again. For solid 7z, a cache miss should
        build a managed cache file and playback should use CACHE.

        Non-solid 7z behaviour is left unchanged.
    */
    if (archiveIs7z || archiveIsRar)
    {
        const char* solidArchiveType = archiveIsRar ? "RAR" : "7Z";

        if (ArchiveGetEntrySolidInfo(actualZipPath, mp3Index, &selectedIsSolid, &selectedSolidFolderIndex, &selectedSolidFilesInFolder))
        {
            selectedSolidKnown = true;
            DebugLogWrite("INFO", "ROUTE", "SOLID_CHECK", "archive=%s | archive_type=%s | index=%u | entry=%s | solid=%d | folder_index=%u | files_in_folder=%u", actualZipPath, solidArchiveType, (unsigned int)mp3Index, firstAudio, selectedIsSolid ? 1 : 0, (unsigned int)selectedSolidFolderIndex, (unsigned int)selectedSolidFilesInFolder);
        }
        else
        {
            DebugLogWrite("WARN", "ROUTE", "SOLID_CHECK_FAIL", "archive=%s | archive_type=%s | index=%u | entry=%s | reason=get_entry_solid_info_failed", actualZipPath, solidArchiveType, (unsigned int)mp3Index, firstAudio);
        }
    }

    if (existingCacheHit)
    {
        playbackMode = ARCHIVE_PLAYBACK_CACHE_FILE;
        DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=CACHE | reason=existing_cache_hit | cache=%s | entry_size=%llu | buffer_size=%zu", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), cachePath, (unsigned long long)uncompSize, g_audioBufferSize);

        if (archiveIs7z && selectedSolidKnown && selectedIsSolid)
        {
            queueSolidCacheWarmAfterPlaybackStart = true;
        }
        else if (archiveIsRar && selectedSolidKnown && selectedIsSolid)
        {
            queueSolidCacheWarmAfterPlaybackStart = true;
            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_DEFER", "archive=%s | index=%u | reason=rar_existing_cache_hit_warm_rest_after_playback_start", actualZipPath, (unsigned int)mp3Index);
        }
    }
    else if ((archiveIs7z || archiveIsRar) && selectedSolidKnown && selectedIsSolid)
    {
        const char* solidArchiveType = archiveIsRar ? "RAR" : "7Z";
        const char* solidReason = archiveIsRar ? "solid_rar_cache_required" : "solid_7z_cache_required";

        ClearPrevalidatedStream();

        if (!ExtractEntryToPlaybackCache(actualZipPath, mp3Index, firstAudio, selectedFormat, uncompSize, solidReason, cachePath, sizeof(cachePath)))
        {
            DebugLogWrite("ERROR", "CACHE", "SOLID_ARCHIVE_CACHE_ROUTE_FAIL", "archive=%s | archive_type=%s | index=%u | entry=%s | size=%llu", actualZipPath, solidArchiveType, (unsigned int)mp3Index, firstAudio, (unsigned long long)uncompSize);

            return 1;
        }

        playbackMode = ARCHIVE_PLAYBACK_CACHE_FILE;
        DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=CACHE | reason=%s | cache=%s | entry_size=%llu | buffer_size=%zu | folder_index=%u | files_in_folder=%u", actualZipPath, solidArchiveType, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), solidReason, cachePath, (unsigned long long)uncompSize, g_audioBufferSize, (unsigned int)selectedSolidFolderIndex, (unsigned int)selectedSolidFilesInFolder);

        if (archiveIs7z)
        {
            queueSolidCacheWarmAfterPlaybackStart = true;
            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_DEFER", "archive=%s | index=%u | reason=playback_cache_ready_start_playback_first", actualZipPath, (unsigned int)mp3Index);
        }
        else
        {
            queueSolidCacheWarmAfterPlaybackStart = true;
            DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_DEFER", "archive=%s | index=%u | reason=rar_selected_cache_ready_warm_rest_after_playback_start", actualZipPath, (unsigned int)mp3Index);
        }
    }
    else if (uncompSize > g_audioBufferSize)
    {
        if (archiveIsZip)
        {
            bool isSplitZipFirstVolume = false;
            size_t archivePathLen = strlen(actualZipPath);

            if (archivePathLen >= 8 && _stricmp(actualZipPath + archivePathLen - 8, ".zip.001") == 0)
            {
                isSplitZipFirstVolume = true;
            }

            if (isSplitZipFirstVolume)
            {
                playbackMode = ARCHIVE_PLAYBACK_STREAM;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=ZIP | index=%u | entry=%s | audio_type=%s | route=STREAM | reason=libarchive_zip_stream_path | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize);
            }
            else
            {
                playbackMode = ARCHIVE_PLAYBACK_STREAM;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=ZIP | index=%u | entry=%s | audio_type=%s | route=STREAM | reason=zip_entry_larger_than_buffer | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize);
            }
        }
        else if (archiveIs7z)
        {
            bool canUse7zStream = false;

            /*
                Preflight the backend stream open.

                Non-solid 7z is stream-eligible, but the backend currently supports
                only simple stream layouts. If the validated open fails, fall back
                to managed cache so playback still works.
            */
            if (ArchiveEntryCanStream(actualZipPath, mp3Index))
            {
                std::string streamPreflightError;

                if (ArchiveOpenValidatedEntryStream(&g_prevalidatedStream, actualZipPath, mp3Index, 16384, &streamPreflightError))
                {
                    InterlockedExchange(&g_prevalidatedStreamReady, 1);
                    canUse7zStream = true;
                }
                else
                {
                    DebugLogWrite("WARN", "PLAY", "STREAM_PREFLIGHT_FAIL", "archive_type=7Z | archive=%s | index=%u | reason=open_validated_stream_failed | error=%s", actualZipPath, (unsigned int)mp3Index, streamPreflightError.c_str());
                }
            }
            else
            {
                DebugLogWrite("INFO", "STREAM", "PREFLIGHT_SKIP", "archive_type=7Z | archive=%s | index=%u | reason=entry_not_stream_capable", actualZipPath, (unsigned int)mp3Index);
            }

            if (canUse7zStream)
            {
                playbackMode = ARCHIVE_PLAYBACK_STREAM;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=7Z | index=%u | entry=%s | audio_type=%s | route=STREAM | reason=7z_open_validated_stream_ok | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize);
            }
            else
            {
                ClearPrevalidatedStream();

                if (!ExtractEntryToPlaybackCache(actualZipPath, mp3Index, firstAudio, selectedFormat, uncompSize, "7z_stream_not_available_or_preflight_failed", cachePath, sizeof(cachePath)))
                {
                    char msg[1400];
                    sprintf_s(msg, sizeof(msg), "This 7z entry is larger than the current archive buffer and could not be streamed or extracted to the managed cache.\n\nTrack:\n%s\n\nTrack size: %.2f MB\nCurrent buffer size: %zu MB", firstAudio, (double)uncompSize / 1024.0 / 1024.0, g_configuredBufferSizeMb);
                    MessageBoxA(g_mod.hMainWindow, msg, "Archive Stream - 7z Stream/Cache Error", MB_OK | MB_ICONERROR);
                    return 1;
                }

                playbackMode = ARCHIVE_PLAYBACK_CACHE_FILE;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=7Z | index=%u | entry=%s | audio_type=%s | route=CACHE | reason=7z_stream_not_available_or_preflight_failed | cache=%s | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), cachePath, (unsigned long long)uncompSize, g_audioBufferSize);
            }
        }
        else if (archiveIsRar)
        {
            bool canUseRarStream = false;

            if (ArchiveEntryCanStream(actualZipPath, mp3Index))
            {
                std::string streamPreflightError;

                if (ArchiveOpenValidatedEntryStream(&g_prevalidatedStream, actualZipPath, mp3Index, 16384, &streamPreflightError))
                {
                    InterlockedExchange(&g_prevalidatedStreamReady, 1);
                    canUseRarStream = true;
                }
                else
                {
                    DebugLogWrite("WARN", "PLAY", "STREAM_PREFLIGHT_FAIL", "archive_type=RAR | archive=%s | index=%u | reason=open_validated_stream_failed | error=%s", actualZipPath, (unsigned int)mp3Index, streamPreflightError.c_str());
                }
            }
            else
            {
                DebugLogWrite("INFO", "STREAM", "PREFLIGHT_SKIP", "archive_type=RAR | archive=%s | index=%u | reason=entry_not_stream_capable", actualZipPath, (unsigned int)mp3Index);
            }

            if (canUseRarStream)
            {
                playbackMode = ARCHIVE_PLAYBACK_STREAM;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=RAR | index=%u | entry=%s | audio_type=%s | route=STREAM | reason=rar_open_validated_stream_ok | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize);
            }
            else
            {
                ClearPrevalidatedStream();

                if (!ExtractEntryToPlaybackCache(actualZipPath, mp3Index, firstAudio, selectedFormat, uncompSize, "rar_stream_not_available_or_preflight_failed", cachePath, sizeof(cachePath)))
                {
                    char msg[1400];
                    sprintf_s(msg, sizeof(msg), "This RAR entry is larger than the current archive buffer and could not be streamed or extracted to the managed cache.\n\nTrack:\n%s\n\nTrack size: %.2f MB\nCurrent buffer size: %zu MB", firstAudio, (double)uncompSize / 1024.0 / 1024.0, g_configuredBufferSizeMb);
                    MessageBoxA(g_mod.hMainWindow, msg, "Archive Stream - RAR Stream/Cache Error", MB_OK | MB_ICONERROR);
                    return 1;
                }

                playbackMode = ARCHIVE_PLAYBACK_CACHE_FILE;
                DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=RAR | index=%u | entry=%s | audio_type=%s | route=CACHE | reason=rar_stream_not_available_or_preflight_failed | cache=%s | entry_size=%llu | buffer_size=%zu", actualZipPath, (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), cachePath, (unsigned long long)uncompSize, g_audioBufferSize);
            }
        }
        else
        {
            DebugLogWrite("ERROR", "PLAY", "ENTRY_TOO_LARGE", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | size=%llu | buffer_bytes=%zu | zip=%d | sevenzip=%d | rar=%d | reason=no_stream_or_cache_path", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize, archiveIsZip ? 1 : 0, archiveIs7z ? 1 : 0, archiveIsRar ? 1 : 0);
            MessageBoxA(g_mod.hMainWindow, "This archive entry is larger than the configured memory buffer, and this archive type has no stream/cache path.", "Archive Stream - Entry Too Large", MB_OK | MB_ICONWARNING);
            return 1;
        }
    }
    else
    {
        DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=BUFFER | reason=entry_fits_configured_buffer | entry_size=%llu | buffer_size=%zu", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), (unsigned long long)uncompSize, g_audioBufferSize);
    }

    /*
        Media Foundation override.

        Only run this when the route has not already been forced to CACHE.
        Solid 7z cache misses are deliberately cached above, so MF must not reopen
        the archive stream or perform a second cache extraction decision here.
    */
    if (playbackMode != ARCHIVE_PLAYBACK_CACHE_FILE && MfIsNativeMediaFormat(selectedFormat))
    {
        DebugLogWrite("INFO", "ROUTE", "MF_ROUTE_CHECK", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | current_route=%s | reason=native_media_format_requires_mf", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), PlaybackModeToLogName(playbackMode));

        bool canUseArchiveStream = false;

        if (ArchiveEntryCanStream(actualZipPath, mp3Index))
        {
            std::string streamPreflightError;

            if (ArchiveValidateEntryStreamRead(actualZipPath, mp3Index, 16384, &streamPreflightError))
            {
                canUseArchiveStream = true;
            }
            else
            {
                DebugLogWrite("WARN", "PLAY", "STREAM_PREFLIGHT_FAIL", "archive_type=MF | archive=%s | index=%u | reason=read_preflight_failed | error=%s", actualZipPath, (unsigned int)mp3Index, streamPreflightError.c_str());
            }
        }
        else
        {
            DebugLogWrite("INFO", "STREAM", "PREFLIGHT_SKIP", "archive_type=MF | archive=%s | index=%u | reason=entry_not_stream_capable", actualZipPath, (unsigned int)mp3Index);
        }

        if (canUseArchiveStream)
        {
            playbackMode = ARCHIVE_PLAYBACK_STREAM;
            DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=STREAM | decoder=MF | reason=mf_archive_stream_read_preflight_ok", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat));
        }
        else
        {
            if (!ExtractEntryToPlaybackCache(actualZipPath, mp3Index, firstAudio, selectedFormat, uncompSize, "mf_archive_stream_not_available", cachePath, sizeof(cachePath)))
            {
                char msg[1400];
                sprintf_s(msg, sizeof(msg), "This archive entry could not be streamed or extracted to the managed cache for Media Foundation playback.\n\nTrack:\n%s\n\nTrack size: %.2f MB", firstAudio, (double)uncompSize / 1024.0 / 1024.0);
                MessageBoxA(g_mod.hMainWindow, msg, "Archive Stream - Media Foundation Cache Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            playbackMode = ARCHIVE_PLAYBACK_CACHE_FILE;
            DebugLogWrite("INFO", "ROUTE", "ROUTE_DECISION", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | route=CACHE | decoder=MF | reason=mf_archive_stream_not_available | cache=%s", actualZipPath, ArchiveTypeToLogName(actualZipPath), (unsigned int)mp3Index, firstAudio, AudioFormatToLogName(selectedFormat), cachePath);
        }
    }

    size_t extractedSize = 0;

    if (playbackMode == ARCHIVE_PLAYBACK_MEMORY)
    {
        std::vector<unsigned char> extractedAudio;
        std::string extractError;

        if (!ArchiveExtractEntryToMemory(actualZipPath, mp3Index, extractedAudio, extractError))
        {
            DebugLogWrite("ERROR", "PLAY", "EXTRACT_FAIL", "archive=%s | index=%u | error=%s", actualZipPath, (unsigned int)mp3Index, extractError.c_str());
            MessageBoxA(g_mod.hMainWindow, "Could not extract archive entry to memory.", "Archive Stream - Extract Error", MB_OK | MB_ICONERROR);
            return 1;
        }

        if (extractedAudio.empty())
        {
            DebugLogWrite("ERROR", "PLAY", "EXTRACT_FAIL", "archive=%s | index=%u | reason=empty_extracted_audio", actualZipPath, (unsigned int)mp3Index);
            MessageBoxA(g_mod.hMainWindow, "Archive entry extracted as empty data.", "Archive Stream - Extract Error", MB_OK | MB_ICONERROR);
            return 1;
        }

        if (extractedAudio.size() > g_audioBufferSize)
        {
            DebugLogWrite("ERROR", "PLAY", "EXTRACT_FAIL", "archive=%s | index=%u | bytes=%zu | buffer_size=%zu | reason=extracted_audio_exceeds_buffer", actualZipPath, (unsigned int)mp3Index, extractedAudio.size(), g_audioBufferSize);
            MessageBoxA(g_mod.hMainWindow, "Extracted audio is larger than the configured memory buffer.", "Archive Stream - Buffer Error", MB_OK | MB_ICONERROR);
            return 1;
        }

        memcpy(g_audioBuffer, extractedAudio.data(), extractedAudio.size());
        g_currentAudioSize = extractedAudio.size();

        //if (!ArchiveExtractEntryToMemory(actualZipPath, mp3Index, g_audioBuffer, (size_t)uncompSize))
        //{
        //    DebugLogWrite("ERROR", "PLAY", "EXTRACT_FAIL", "archive=%s | index=%u | size=%llu | route=memory", actualZipPath, (unsigned int)mp3Index, (unsigned long long)uncompSize);
        //    MessageBoxA(g_mod.hMainWindow, "Failed to decompress audio file into reusable memory buffer.", "Archive Stream - Extract Error", MB_OK | MB_ICONERROR);
        //    return 1;
        //}

        extractedSize = extractedAudio.size();

        if (selectedFormat == AUDIO_FORMAT_MP3)
        {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            drmp3_uint64 totalPCMFrames = 0;
            drmp3_uint64 decodedTestFrames = 0;

            bool decodeOk = TestDecodeMp3FromMemory(g_audioBuffer, extractedSize, &channels, &sampleRate, &totalPCMFrames, &decodedTestFrames);

            if (!decodeOk)
            {
                DebugLogWrite("ERROR", "PLAY", "DECODE_VALIDATE_FAIL", "decoder=MP3 | archive=%s | index=%u | entry=%s | size=%zu | route=memory", actualZipPath, (unsigned int)mp3Index, firstAudio, extractedSize);
                MessageBoxA(g_mod.hMainWindow, "MP3 was decompressed from ZIP, but dr_mp3 could not decode it.", "Archive Stream - MP3 Decode Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            double durationSeconds = sampleRate > 0 ? (double)totalPCMFrames / (double)sampleRate : 0.0;

            g_currentLengthMs = durationSeconds > 0.0 ? (int)(durationSeconds * 1000.0) : -1000;
            g_currentSampleRate = (int)sampleRate;
            g_currentChannels = (int)channels;
            g_currentBitrateKbps = durationSeconds > 0.0 ? (int)(((double)extractedSize * 8.0) / durationSeconds / 1000.0) : 0;
            g_currentAudioSize = extractedSize;
        }
        else if (selectedFormat == AUDIO_FORMAT_WAV)
        {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            drwav_uint64 totalPCMFrames = 0;
            drwav_uint64 decodedTestFrames = 0;

            bool decodeOk = TestDecodeWavFromMemory(g_audioBuffer, extractedSize, &channels, &sampleRate, &totalPCMFrames, &decodedTestFrames);

            if (!decodeOk)
            {
                DebugLogWrite("ERROR", "PLAY", "DECODE_VALIDATE_FAIL", "decoder=WAV | archive=%s | index=%u | entry=%s | size=%zu | route=memory", actualZipPath, (unsigned int)mp3Index, firstAudio, extractedSize);
                MessageBoxA(g_mod.hMainWindow, "WAV was decompressed from ZIP, but dr_wav could not decode it.", "Archive Stream - WAV Decode Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            double durationSeconds = sampleRate > 0 ? (double)totalPCMFrames / (double)sampleRate : 0.0;

            g_currentLengthMs = durationSeconds > 0.0 ? (int)(durationSeconds * 1000.0) : -1000;
            g_currentSampleRate = (int)sampleRate;
            g_currentChannels = (int)channels;
            g_currentBitrateKbps = durationSeconds > 0.0 ? (int)(((double)extractedSize * 8.0) / durationSeconds / 1000.0) : 0;
            g_currentAudioSize = extractedSize;
        }
        else if (selectedFormat == AUDIO_FORMAT_FLAC)
        {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            drflac_uint64 totalPCMFrames = 0;
            drflac_uint64 decodedTestFrames = 0;

            bool decodeOk = TestDecodeFlacFromMemory(g_audioBuffer, extractedSize, &channels, &sampleRate, &totalPCMFrames, &decodedTestFrames);

            if (!decodeOk)
            {
                DebugLogWrite("ERROR", "PLAY", "DECODE_VALIDATE_FAIL", "decoder=FLAC | archive=%s | index=%u | entry=%s | size=%zu | route=memory", actualZipPath, (unsigned int)mp3Index, firstAudio, extractedSize);
                MessageBoxA(g_mod.hMainWindow, "FLAC was decompressed from ZIP, but dr_flac could not decode it.", "Archive Stream - FLAC Decode Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            double durationSeconds = sampleRate > 0 ? (double)totalPCMFrames / (double)sampleRate : 0.0;

            g_currentLengthMs = durationSeconds > 0.0 ? (int)(durationSeconds * 1000.0) : -1000;
            g_currentSampleRate = (int)sampleRate;
            g_currentChannels = (int)channels;
            g_currentBitrateKbps = durationSeconds > 0.0 ? (int)(((double)extractedSize * 8.0) / durationSeconds / 1000.0) : 0;
            g_currentAudioSize = extractedSize;
        }
        else if (selectedFormat == AUDIO_FORMAT_OGG)
        {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            unsigned int totalSamples = 0;
            unsigned int decodedTestSamples = 0;

            bool decodeOk = TestDecodeOggFromMemory(g_audioBuffer, extractedSize, &channels, &sampleRate, &totalSamples, &decodedTestSamples);

            if (!decodeOk)
            {
                DebugLogWrite("ERROR", "PLAY", "DECODE_VALIDATE_FAIL", "decoder=OGG | archive=%s | index=%u | entry=%s | size=%zu | route=memory", actualZipPath, (unsigned int)mp3Index, firstAudio, extractedSize);
                MessageBoxA(g_mod.hMainWindow, "OGG was decompressed from ZIP, but stb_vorbis could not decode it.", "Archive Stream - OGG Decode Error", MB_OK | MB_ICONERROR);
                return 1;
            }

            double durationSeconds = sampleRate > 0 ? (double)totalSamples / (double)sampleRate : 0.0;

            g_currentLengthMs = durationSeconds > 0.0 ? (int)(durationSeconds * 1000.0) : -1000;
            g_currentSampleRate = (int)sampleRate;
            g_currentChannels = (int)channels;
            g_currentBitrateKbps = durationSeconds > 0.0 ? (int)(((double)extractedSize * 8.0) / durationSeconds / 1000.0) : 0;
            g_currentAudioSize = extractedSize;
        }
    }
    else
    {
        /*
            Non-memory mode: do not extract the entry into g_audioBuffer here.
            PlaybackThreadProc will either open the archive stream or the managed cache file.
            Keep any playlist-provided duration if available.
            If we do not know it, leave it unknown.
        */
        int knownLengthMs = -1000;
        char tempTitle[1024];

        if (GetVirtualArchiveEntryInfo(fn, tempTitle, sizeof(tempTitle), &knownLengthMs))
            g_currentLengthMs = knownLengthMs;
        else
            g_currentLengthMs = -1000;

        g_currentSampleRate = 0;
        g_currentChannels = 0;
        g_currentBitrateKbps = 0;
        g_currentAudioSize = (size_t)uncompSize;
    }

    if (usingVirtualPath)
    {
        /*
            Use the same display formatting as GetFileInfo().

            firstAudio can contain the archive folder path, so do not pass it
            directly to ArchiveFormatVirtualTitle().
        */
        FormatStableVirtualTitle(fn, firstAudio, mp3Index, g_currentTitle, sizeof(g_currentTitle));
    }
    else
    {
        CopyBaseName(g_currentTitle, sizeof(g_currentTitle), firstAudio);
    }

    // route summary
    LogArchiveRouteProfile(actualZipPath, mp3Index, firstAudio, uncompSize, selectedFormat, playbackMode, "final_play_route");
    InterlockedExchange(&g_currentAudioFormat, selectedFormat);
    strcpy_s(g_currentArchivePath, sizeof(g_currentArchivePath), actualZipPath);
    g_currentArchiveIndex = mp3Index;
    strcpy_s(g_currentCachePath, sizeof(g_currentCachePath), cachePath);
    InterlockedExchange(&g_playbackMode, playbackMode);
    InterlockedExchange(&g_stopRequested, 0);
    InterlockedExchange(&g_seekRequested, 0);
    InterlockedExchange(&g_seekTargetMs, 0);
    InterlockedExchange(&g_outputTimeBaseMs, 0);
    InterlockedExchange(&g_trackStartTick, GetTickCount());

    DebugLogWrite("INFO", "PLAY", "THREAD_CREATE_REQUEST", "archive=%s | archive_type=%s | index=%u | entry=%s | audio_type=%s | mode=%s | cache=%s | size=%zu", g_currentArchivePath, ArchiveTypeToLogName(g_currentArchivePath), (unsigned int)g_currentArchiveIndex, firstAudio, AudioFormatToLogName(selectedFormat), PlaybackModeToLogName(playbackMode), g_currentCachePath, g_currentAudioSize);

    g_playThread = CreateThread(NULL, 0, PlaybackThreadProc, NULL, 0, NULL);

    if (!g_playThread)
    {
        ClearPrevalidatedStream();
        DebugLogWrite("ERROR", "PLAY", "THREAD_START_FAIL", "archive=%s | index=%u", g_currentArchivePath, (unsigned int)g_currentArchiveIndex);
        MessageBoxA(g_mod.hMainWindow, "Failed to start playback thread.", "Archive Stream - Playback Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    DebugLogWrite("INFO", "PLAY", "THREAD_CREATED", "archive=%s | archive_type=%s | index=%u | thread=0x%p | mode=%s | cache=%s", g_currentArchivePath, ArchiveTypeToLogName(g_currentArchivePath), (unsigned int)g_currentArchiveIndex, g_playThread, PlaybackModeToLogName(playbackMode), g_currentCachePath);

    if (queueSolidCacheWarmAfterPlaybackStart)
    {
        DebugLogWrite("INFO", "SOLID_CACHE_WARM", "QUEUE_AFTER_PLAYBACK_START", "archive=%s | index=%u", actualZipPath, (unsigned int)mp3Index);
        QueueSolid7zCacheWarm(actualZipPath, mp3Index);
    }

    return 0;
}

extern "C" __declspec(dllexport) In_Module* winampGetInModule2()
{
    memset(&g_mod, 0, sizeof(g_mod));

    g_mod.version = IN_VER;
    g_mod.description = g_description;
    g_mod.FileExtensions = g_fileExtensions;
    g_mod.is_seekable = 1;
    g_mod.UsesOutputPlug = 1;
    g_mod.Config = Config;
    g_mod.About = About;
    g_mod.Init = Init;
    g_mod.Quit = Quit;
    g_mod.GetFileInfo = GetFileInfo;
    g_mod.InfoBox = InfoBox;
    g_mod.IsOurFile = IsOurFile;
    g_mod.Play = Play;
    g_mod.Pause = Pause;
    g_mod.UnPause = UnPause;
    g_mod.IsPaused = IsPaused;
    g_mod.Stop = Stop;
    g_mod.GetLength = GetLength;
    g_mod.GetOutputTime = GetOutputTime;
    g_mod.SetOutputTime = SetOutputTime;
    g_mod.SetVolume = SetVolume;
    g_mod.SetPan = SetPan;
    g_mod.EQSet = EQSet;

    return &g_mod;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hInstance = (HINSTANCE)hModule;
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}