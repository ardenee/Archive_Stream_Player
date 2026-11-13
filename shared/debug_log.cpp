#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <cstring>
#include <cstdarg>
#include <string.h>

#define DEBUG_LOG_IMPLEMENTATION
#include "debug_log.h"

static bool g_debugLogEnabled = false;
static char g_debugLogPathInternal[MAX_PATH * 4] = "";
static wchar_t g_debugLogPathWide[MAX_PATH * 4] = L"";
static CRITICAL_SECTION g_debugLogLock;
static bool g_debugLogLockReady = false;

static unsigned long long g_debugLogTraceId = 0;

static const char* DebugLogBaseName(const char* path)
{
    if (!path)
    {
        return "";
    }

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* slash = slash1;

    if (slash2 && (!slash1 || slash2 > slash1))
    {
        slash = slash2;
    }

    if (slash)
    {
        return slash + 1;
    }

    return path;
}

static void DebugLogEnsureLock()
{
    if (!g_debugLogLockReady)
    {
        InitializeCriticalSection(&g_debugLogLock);
        g_debugLogLockReady = true;
    }
}

static void DebugLogFormatTimestamp(char* out, size_t outSize)
{
    if (!out || outSize == 0)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    sprintf_s(out, outSize, "%04u-%02u-%02u %02u:%02u:%02u.%03u", (unsigned int)st.wYear, (unsigned int)st.wMonth, (unsigned int)st.wDay, (unsigned int)st.wHour, (unsigned int)st.wMinute, (unsigned int)st.wSecond, (unsigned int)st.wMilliseconds);
}

static void DebugLogWriteFallbackFailure(const char* logPath, DWORD winerr)
{
    char tempPath[MAX_PATH];
    char fallbackPath[MAX_PATH];

    tempPath[0] = '\0';
    fallbackPath[0] = '\0';

    DWORD len = GetTempPathA(sizeof(tempPath), tempPath);

    if (len == 0 || len >= sizeof(tempPath))
        return;

    sprintf_s(fallbackPath, sizeof(fallbackPath), "%sarchive_stream_debug_log_failure.txt", tempPath);

    FILE* f = nullptr;

    if (fopen_s(&f, fallbackPath, "ab") != 0 || !f)
        return;

    char timestamp[64];
    DebugLogFormatTimestamp(timestamp, sizeof(timestamp));

    fprintf(f, "%s | ERROR | LOGGER | OPEN_FAIL | path=%s | winerr=%lu\r\n", timestamp, logPath ? logPath : "", (unsigned long)winerr);
    fclose(f);
}

static bool DebugLogEnsureFolderExistsForFile(const char* filePath)
{
    if (!filePath || !filePath[0])
        return false;

    char folder[MAX_PATH * 4];
    strcpy_s(folder, sizeof(folder), filePath);

    char* lastSlash = strrchr(folder, '\\');

    if (!lastSlash)
        return true;

    *lastSlash = '\0';

    if (!folder[0])
        return true;

    char partial[MAX_PATH * 4];
    partial[0] = '\0';

    size_t len = strlen(folder);

    if (len >= sizeof(partial))
        return false;

    strcpy_s(partial, sizeof(partial), folder);

    for (char* p = partial; *p; ++p)
    {
        if (*p == '\\')
        {
            if (p == partial)
                continue;

            if (p > partial && *(p - 1) == ':')
                continue;

            *p = '\0';

            if (partial[0])
                CreateDirectoryA(partial, NULL);

            *p = '\\';
        }
    }

    if (!CreateDirectoryA(partial, NULL))
    {
        DWORD err = GetLastError();

        if (err != ERROR_ALREADY_EXISTS)
            return false;
    }

    return true;
}

void DebugLogConfigure(bool enabled, const char* logPath, bool resetFile)
{
    DebugLogEnsureLock();
    EnterCriticalSection(&g_debugLogLock);

    g_debugLogEnabled = enabled;
    g_debugLogPathInternal[0] = '\0';
    g_debugLogPathWide[0] = L'\0';

    if (logPath && logPath[0])
    {
        strcpy_s(g_debugLogPathInternal, sizeof(g_debugLogPathInternal), logPath);
    }

    if (g_debugLogEnabled && g_debugLogPathInternal[0])
    {
        DebugLogEnsureFolderExistsForFile(g_debugLogPathInternal);

        if (resetFile)
        {
            DeleteFileA(g_debugLogPathInternal);
        }

        LeaveCriticalSection(&g_debugLogLock);
        DebugLogWriteEx(__FILE__, __LINE__, "INFO", "LOGGER", "FORMAT_VERSION", "version=trace_src_v1");
        return;
    }

    LeaveCriticalSection(&g_debugLogLock);
}

void DebugLogConfigureWide(bool enabled, const wchar_t* logPath, bool resetFile)
{
    DebugLogEnsureLock();
    EnterCriticalSection(&g_debugLogLock);

    g_debugLogEnabled = enabled;
    g_debugLogPathInternal[0] = '\0';
    g_debugLogPathWide[0] = L'\0';
    if (logPath && logPath[0] && wcslen(logPath) < _countof(g_debugLogPathWide))
        wcscpy_s(g_debugLogPathWide, logPath);

    if (g_debugLogEnabled && g_debugLogPathWide[0])
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_debugLogPathWide).parent_path(), ec);
        if (resetFile)
            DeleteFileW(g_debugLogPathWide);
        LeaveCriticalSection(&g_debugLogLock);
        DebugLogWriteEx(__FILE__, __LINE__, "INFO", "LOGGER", "FORMAT_VERSION", "version=trace_src_v1");
        return;
    }

    LeaveCriticalSection(&g_debugLogLock);
}

void DebugLogShutdown()
{
    if (!g_debugLogLockReady)
        return;

    EnterCriticalSection(&g_debugLogLock);

    g_debugLogEnabled = false;
    g_debugLogPathInternal[0] = '\0';
    g_debugLogPathWide[0] = L'\0';

    LeaveCriticalSection(&g_debugLogLock);

    DeleteCriticalSection(&g_debugLogLock);
    g_debugLogLockReady = false;
}

bool DebugLogIsEnabled()
{
    return g_debugLogEnabled;
}

const char* DebugLogGetPath()
{
    return g_debugLogPathInternal;
}

void DebugLogWriteVEx(const char* sourceFile, int sourceLine, const char* level, const char* area, const char* eventName, const char* fmt, va_list args)
{
    DebugLogEnsureLock();
    EnterCriticalSection(&g_debugLogLock);

    if (!g_debugLogEnabled || (!g_debugLogPathInternal[0] && !g_debugLogPathWide[0]))
    {
        LeaveCriticalSection(&g_debugLogLock);
        return;
    }

    if (!g_debugLogPathWide[0])
        DebugLogEnsureFolderExistsForFile(g_debugLogPathInternal);

    FILE* f = nullptr;
    const int opened = g_debugLogPathWide[0] ? _wfopen_s(&f, g_debugLogPathWide, L"ab") : fopen_s(&f, g_debugLogPathInternal, "ab");
    if (opened != 0 || !f)
    {
        DWORD err = GetLastError();
        DebugLogWriteFallbackFailure(g_debugLogPathInternal, err);
        LeaveCriticalSection(&g_debugLogLock);
        return;
    }

    char timestamp[64];
    DebugLogFormatTimestamp(timestamp, sizeof(timestamp));

    unsigned long long traceId = ++g_debugLogTraceId;
    const char* baseSourceFile = DebugLogBaseName(sourceFile);
    fprintf(f, "%s | %-5s | %-6s | %-18s | trace=%llu | src=%s:%d", timestamp, level ? level : "INFO", area ? area : "GEN", eventName ? eventName : "EVENT", traceId, baseSourceFile, sourceLine);

    if (fmt && fmt[0])
    {
        fprintf(f, " | ");
        vfprintf(f, fmt, args);
    }

    fprintf(f, "\r\n");
    fclose(f);

    LeaveCriticalSection(&g_debugLogLock);
}







void DebugLogWriteEx(const char* sourceFile, int sourceLine, const char* level, const char* area, const char* eventName, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DebugLogWriteVEx(sourceFile, sourceLine, level, area, eventName, fmt, args);
    va_end(args);
}

void DebugLogWritePlain(const char* level, const char* area, const char* eventName, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DebugLogWriteVEx("", 0, level, area, eventName, fmt, args);
    va_end(args);
}

void DebugLogWriteVPlain(const char* level, const char* area, const char* eventName, const char* fmt, va_list args)
{
    DebugLogWriteVEx("", 0, level, area, eventName, fmt, args);
}

void DebugLogWriteRaw(const char* level, const char* area, const char* eventName, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    DebugLogWriteVEx("", 0, level, area, eventName, fmt, args);
    va_end(args);
}

void DebugLogWriteVRaw(const char* level, const char* area, const char* eventName, const char* fmt, va_list args)
{
    DebugLogWriteVEx("", 0, level, area, eventName, fmt, args);
}