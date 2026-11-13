#pragma once
// One logging policy and per-host file layout for Archive Stream Player.
// The common settings page controls enable/disable and log directory.
// Each player uses a distinct filename to avoid cross-process write clashes.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include "archive_adapter_preferences.h"
#include "debug_log.h"

namespace ArchiveAdapterLogging
{
    inline std::wstring Directory(const ArchiveAdapterPreferences& options)
    {
        if (!options.logDirectory.empty())
            return options.logDirectory;
        wchar_t local[MAX_PATH * 4]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, _countof(local));
        if (!n || n >= _countof(local))
            return {};
        return std::wstring(local) + L"\\ArchiveStreamPlayer\\Logs";
    }

    inline std::wstring FilePath(const ArchiveAdapterPreferences& options, const wchar_t* hostName)
    {
        if (!hostName || !*hostName)
            return {};
        const auto folder = Directory(options);
        if (folder.empty())
            return {};
        for (const wchar_t* p = hostName; *p; ++p)
        {
            const wchar_t c = *p;
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                (c >= L'0' && c <= L'9') || c == L'-' || c == L'_'))
                return {};
        }
        return folder + L"\\archive_stream_" + hostName + L".log";
    }

    inline std::string AnsiPath(const std::wstring& unicode)
    {
        if (unicode.empty() || unicode.size() > 4095)
            return {};
        BOOL substituted = FALSE;
        const int count = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
            unicode.c_str(), -1, nullptr, 0, nullptr, &substituted);
        if (count <= 1 || substituted)
            return {};
        std::string output(static_cast<size_t>(count), '\0');
        substituted = FALSE;
        if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
            unicode.c_str(), -1, output.data(), count, nullptr, &substituted) != count || substituted)
            return {};
        output.resize(static_cast<size_t>(count - 1));
        return output;
    }

    inline bool Configure(const wchar_t* hostName, bool enabled)
    {
        const auto options = ArchiveAdapterPreferencesLoad();
        const std::wstring path = FilePath(options, hostName);
        if (enabled && path.empty())
        {
            DebugLogConfigureWide(false, nullptr, false);
            return false;
        }
        DebugLogConfigureWide(enabled, path.c_str(), false);
        return true;
    }

    inline bool ConfigureShared(const wchar_t* hostName)
    {
        const auto options = ArchiveAdapterPreferencesLoad();
        return Configure(hostName, options.debugLogging);
    }
}
