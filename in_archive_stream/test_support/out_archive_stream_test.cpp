// QA-only Winamp output plugin. Never packaged or installed to a real player.
// Simulates a realtime bounded PCM queue without any physical audio device.
#define NOMINMAX
#include <windows.h>
#include <OUT.H>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    bool g_open = false;
    bool g_paused = false;
    bool g_firstWrite = true;
    uint64_t g_written = 0;
    uint64_t g_totalWritten = 0;
    uint64_t g_capacity = 0;
    uint64_t g_bytesPerSecond = 1;
    ULONGLONG g_startTick = 0;
    ULONGLONG g_pausedAt = 0;
    int g_baseMs = 0;

    void Report(const char* action, unsigned long long value, unsigned long long extra)
    {
        char file[4096]{};
        const DWORD length = GetEnvironmentVariableA("AST_WINAMP_TEST_SINK_REPORT", file, sizeof(file));
        if (!length || length >= sizeof(file))
            return;
        HANDLE output = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
            return;
        char line[256]{};
        const int len = sprintf_s(line, "%s %llu %llu\n", action, value, extra);
        if (len > 0)
        {
            DWORD written = 0;
            WriteFile(output, line, static_cast<DWORD>(len), &written, nullptr);
        }
        CloseHandle(output);
    }

    uint64_t ConsumedLocked()
    {
        if (!g_open || !g_bytesPerSecond)
            return 0;
        const ULONGLONG now = g_paused ? g_pausedAt : GetTickCount64();
        const ULONGLONG elapsed = now >= g_startTick ? now - g_startTick : 0;
        return std::min(g_written, (elapsed * g_bytesPerSecond) / 1000);
    }

    void Config(HWND) {}
    void About(HWND) {}
    void Init() { Report("INIT", 0, 0); }
    void Quit() { Report("QUIT", 0, 0); }

    int Open(int sampleRate, int channels, int bits, int, int)
    {
        if (sampleRate <= 0 || channels <= 0 || bits <= 0 || bits % 8)
            return -1;
        const uint64_t bytes = static_cast<uint64_t>(sampleRate) * channels * (bits / 8);
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_bytesPerSecond = bytes;
            g_capacity = bytes * 2;
            g_written = 0;
            g_totalWritten = 0;
            g_startTick = GetTickCount64();
            g_baseMs = 0;
            g_paused = false;
            g_firstWrite = true;
            g_open = true;
        }
        Report("OPEN", sampleRate, channels);
        return 2000;
    }

    void Close()
    {
        uint64_t total = 0;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            total = g_totalWritten;
            g_open = false;
        }
        Report("CLOSE", total, 0);
    }

    int CanWrite()
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        if (!g_open)
            return 0;
        const uint64_t consumed = ConsumedLocked();
        const uint64_t pending = g_written > consumed ? g_written - consumed : 0;
        const uint64_t space = pending >= g_capacity ? 0 : g_capacity - pending;
        return static_cast<int>(std::min<uint64_t>(8192, space));
    }

    int Write(char*, int length)
    {
        if (length <= 0)
            return 1;
        bool first = false;
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            if (!g_open)
                return 1;
            const uint64_t consumed = ConsumedLocked();
            const uint64_t pending = g_written > consumed ? g_written - consumed : 0;
            if (pending + static_cast<uint64_t>(length) > g_capacity)
                return 1;
            g_written += static_cast<uint64_t>(length);
            g_totalWritten += static_cast<uint64_t>(length);
            if (g_firstWrite)
            {
                first = true;
                g_firstWrite = false;
            }
        }
        if (first)
            Report("WRITE_FIRST", length, 0);
        return 0;
    }

    int IsPlaying()
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        return g_open && g_written > ConsumedLocked() ? 1 : 0;
    }

    int Pause(int paused)
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        const int previous = g_paused ? 1 : 0;
        if (paused && !g_paused)
        {
            g_pausedAt = GetTickCount64();
            g_paused = true;
        }
        else if (!paused && g_paused)
        {
            g_startTick += GetTickCount64() - g_pausedAt;
            g_paused = false;
        }
        return previous;
    }

    void SetVolume(int) {}
    void SetPan(int) {}

    void Flush(int milliseconds)
    {
        {
            std::lock_guard<std::mutex> guard(g_mutex);
            g_written = 0;
            g_baseMs = milliseconds;
            g_startTick = GetTickCount64();
            g_firstWrite = true;
            g_paused = false;
        }
        Report("FLUSH", milliseconds, 0);
    }

    int GetOutputTime()
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        return g_baseMs + static_cast<int>((ConsumedLocked() * 1000) / g_bytesPerSecond);
    }

    int GetWrittenTime()
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        return g_baseMs + static_cast<int>((g_written * 1000) / g_bytesPerSecond);
    }

    char g_description[] = "Archive Stream QA output sink (silent, test-only)";
    Out_Module g_module =
    {
        OUT_VER, g_description, 65537, nullptr, nullptr,
        Config, About, Init, Quit, Open, Close, Write, CanWrite,
        IsPlaying, Pause, SetVolume, SetPan, Flush, GetOutputTime, GetWrittenTime
    };
}

extern "C" __declspec(dllexport) Out_Module* winampGetOutModule()
{
    return &g_module;
}
