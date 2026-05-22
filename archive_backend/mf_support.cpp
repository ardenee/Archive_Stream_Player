#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <strsafe.h>

#include "mf_support.h"
#include "debug_log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

static bool g_mfStarted = false;
static bool g_mfStartAttempted = false;
static HRESULT g_mfStartHr = S_OK;

bool MfEnsureStarted(HWND hwndParent)
{
    if (g_mfStarted)
        return true;

    if (g_mfStartAttempted)
    {
        if (!g_mfStarted)
            MfShowUnavailableMessage(hwndParent);

        return g_mfStarted;
    }

    g_mfStartAttempted = true;

    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");

    if (!mfplat)
    {
        DWORD err = GetLastError();
        g_mfStartHr = HRESULT_FROM_WIN32(err);
        DebugLogWrite("ERROR", "MF", "STARTUP_FAIL", "dll=mfplat.dll | winerr=%lu | hr=0x%08lX | reason=load_library_failed", err, (unsigned long)g_mfStartHr);
        MfShowUnavailableMessage(hwndParent);
        return false;
    }

    FreeLibrary(mfplat);

    g_mfStartHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);

    if (FAILED(g_mfStartHr))
    {
        DebugLogWrite("ERROR", "MF", "STARTUP_FAIL", "hr=0x%08lX | reason=mfstartup_failed", (unsigned long)g_mfStartHr);
        MfShowUnavailableMessage(hwndParent);
        return false;
    }

    g_mfStarted = true;
    return true;
}

void MfShutdownIfStarted()
{
    if (g_mfStarted)
    {
        HRESULT hr = MFShutdown();

        if (FAILED(hr))
            DebugLogWrite("ERROR", "MF", "SHUTDOWN_FAIL", "hr=0x%08lX", (unsigned long)hr);

        g_mfStarted = false;
    }
}

void MfShowUnavailableMessage(HWND hwndParent)
{
    char msg[1200];

    StringCchPrintfA(msg, ARRAYSIZE(msg),
        "Media Foundation could not be started.\n\n"
        "M4A, AAC, and WMA playback require Windows Media Foundation.\n\n"
        "On normal Windows editions this is built in. On Windows N editions, install the Microsoft Media Feature Pack.\n\n"
        "HRESULT: 0x%08lX",
        (unsigned long)g_mfStartHr);

    MessageBoxA(hwndParent, msg, "Archive Stream - Media Foundation Required", MB_OK | MB_ICONWARNING);
}