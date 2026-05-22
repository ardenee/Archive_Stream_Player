#pragma once

#include <windows.h>
#include "in2.h"
#include "archive_common.h"

extern In_Module g_mod;

extern HANDLE g_playThread;
extern volatile LONG g_stopRequested;
extern volatile LONG g_isPlaying;
extern volatile LONG g_seekRequested;
extern volatile LONG g_seekTargetMs;
extern volatile LONG g_outputTimeBaseMs;
extern volatile LONG g_trackStartTick;
extern volatile LONG g_currentAudioFormat;

extern unsigned char* g_audioBuffer;
extern size_t g_audioBufferSize;
extern size_t g_configuredBufferSizeMb;
extern size_t g_configuredCacheSizeMb;
extern bool g_cacheSizeCleanupEnabled;
extern bool g_cacheAgeCleanupEnabled;
extern unsigned int g_cacheMaxAgeDays;
extern char g_displayPrefix[32];
extern char g_displayPostfix[32];
extern char g_displayTemplate[160];
extern char g_displayTemplate[160];
extern char g_rarPasswordListPath[MAX_PATH * 4];
extern size_t g_currentAudioSize;
extern bool g_debugLoggingEnabled;
extern char g_debugLogPath[MAX_PATH * 4];

extern char g_currentTitle[1024];
extern int g_currentLengthMs;
extern int g_currentBitrateKbps;
extern int g_currentSampleRate;
extern int g_currentChannels;

extern char g_iniPath[MAX_PATH];

extern HWND g_configWnd;
extern HWND g_bufferEdit;
extern HWND g_cacheEdit;
extern HINSTANCE g_hInstance;

extern volatile LONG g_playbackMode;
extern char g_currentArchivePath[MAX_PATH * 4];
extern mz_uint g_currentArchiveIndex;
extern char g_currentCachePath[MAX_PATH * 4];

extern ArchiveStreamContext g_prevalidatedStream;
extern volatile LONG g_prevalidatedStreamReady;

extern bool g_debugLoggingEnabled;
extern char g_debugLogPath[MAX_PATH * 4];

void Stop();