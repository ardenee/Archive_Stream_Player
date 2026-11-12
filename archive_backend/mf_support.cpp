#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mutex>

#include "mf_support.h"
#include "debug_log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

static std::mutex g_mfStartupMutex;
static bool g_mfStarted = false;
static bool g_mfStartAttempted = false;
static HRESULT g_mfStartHr = S_OK;

bool MfEnsureStarted()
{
    std::lock_guard<std::mutex> lock(g_mfStartupMutex);

    if (g_mfStarted)
        return true;

    if (g_mfStartAttempted)
        return g_mfStarted;

    g_mfStartAttempted = true;

    HMODULE mfplat = LoadLibraryW(L"mfplat.dll");

    if (!mfplat)
    {
        DWORD err = GetLastError();
        g_mfStartHr = HRESULT_FROM_WIN32(err);
        DebugLogWrite("ERROR", "MF", "STARTUP_FAIL", "dll=mfplat.dll | winerr=%lu | hr=0x%08lX | reason=load_library_failed", err, (unsigned long)g_mfStartHr);
        return false;
    }

    FreeLibrary(mfplat);

    g_mfStartHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);

    if (FAILED(g_mfStartHr))
    {
        DebugLogWrite("ERROR", "MF", "STARTUP_FAIL", "hr=0x%08lX | reason=mfstartup_failed", (unsigned long)g_mfStartHr);
        return false;
    }

    g_mfStarted = true;
    return true;
}

HRESULT MfGetStartupResult()
{
    std::lock_guard<std::mutex> lock(g_mfStartupMutex);
    return g_mfStartHr;
}

void MfShutdownIfStarted()
{
    std::lock_guard<std::mutex> lock(g_mfStartupMutex);
    if (g_mfStarted)
    {
        HRESULT hr = MFShutdown();

        if (FAILED(hr))
            DebugLogWrite("ERROR", "MF", "SHUTDOWN_FAIL", "hr=0x%08lX", (unsigned long)hr);

        g_mfStarted = false;
    }

    // Allow a subsequent host initialization to retry after a normal shutdown.
    g_mfStartAttempted = false;
    g_mfStartHr = S_OK;
}
