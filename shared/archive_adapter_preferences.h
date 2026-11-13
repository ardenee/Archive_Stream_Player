#pragma once

// Shared user preferences for XMPlay, AIMP and local playback-bridge adapters.
// Winamp/foobar2000 keep their own existing player-specific settings.
// Settings are ordinary values in HKCU\Software\ArchiveStreamPlayer\Adapters.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <string>

inline std::wstring ArchiveAdapterDefaultPasswordList()
{
    wchar_t local[MAX_PATH * 4]{};
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    if (!written || written >= std::size(local))
        return {};
    return std::wstring(local) + L"\\WinampArchiveStream\\keeps.txt";
}

struct ArchiveAdapterPreferences
{
    unsigned memoryMb = 50;
    unsigned cacheLimitMb = 2048;
    bool promptPasswords = true;
    bool askToSavePassword = true;
    std::wstring passwordListPath = ArchiveAdapterDefaultPasswordList();
    std::wstring titleTemplate = L"<filename>";
};

inline ArchiveAdapterPreferences ArchiveAdapterPreferencesLoad()
{
    ArchiveAdapterPreferences settings;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ArchiveStreamPlayer\\Adapters", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return settings;

    auto number = [&](const wchar_t* name, DWORD fallback, DWORD minValue, DWORD maxValue)
    {
        DWORD value = fallback;
        DWORD type = REG_DWORD;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
            type != REG_DWORD || size != sizeof(value))
            return fallback;
        return std::clamp(value, minValue, maxValue);
    };

    auto string = [&](const wchar_t* name) -> std::wstring
    {
        wchar_t value[4096]{};
        DWORD type = REG_SZ;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) != ERROR_SUCCESS ||
            type != REG_SZ || size < sizeof(wchar_t) || size > sizeof(value))
            return {};
        value[(std::min)(static_cast<size_t>(size / sizeof(wchar_t)), std::size(value) - 1)] = L'\0';
        return value;
    };

    settings.memoryMb = number(L"MemoryMB", 50, 1, 512);
    settings.cacheLimitMb = number(L"CacheLimitMB", 2048, 0, 20480);
    settings.promptPasswords = number(L"PromptPasswords", 1, 0, 1) != 0;
    settings.askToSavePassword = number(L"AskToSavePassword", 1, 0, 1) != 0;
    const std::wstring configuredList = string(L"PasswordList");
    if (!configuredList.empty())
        settings.passwordListPath = configuredList;
    const std::wstring format = string(L"TitleTemplate");
    if (format == L"<filename>" || format == L"<archivetype>: <filename>" ||
        format == L"<#>. <filename>" || format == L"<filename> [<filetype>]" ||
        format == L"<archivetype>: <#> - <filename>" ||
        format == L"[<archive>] <#> - <filename>")
        settings.titleTemplate = format;
    RegCloseKey(key);
    return settings;
}

// Keep the format placeholders compatible with the six Winamp/foobar presets.
// The archive entry's original internal path is never modified.
inline std::wstring ArchiveAdapterFormatTitle(const std::wstring& archive,
    const std::wstring& internalPath, unsigned entryNumber, const std::wstring& titleTemplate)
{
    std::wstring filename = internalPath;
    const auto separator = filename.find_last_of(L"/\\");
    if (separator != std::wstring::npos)
        filename.erase(0, separator + 1);
    for (wchar_t& c : filename)
    {
        if (c == L'\r' || c == L'\n' || c == L'\0')
            c = L' ';
    }

    std::wstring extension;
    const auto dot = filename.find_last_of(L'.');
    if (dot != std::wstring::npos)
        extension = filename.substr(dot + 1);

    std::wstring archiveName = archive;
    const auto archiveSep = archiveName.find_last_of(L"/\\");
    if (archiveSep != std::wstring::npos)
        archiveName.erase(0, archiveSep + 1);
    // Split ZIP/7z first volumes use .001; report the underlying container
    // rather than presenting "001" as the archive format in track titles.
    if (archiveName.size() > 4 && archiveName.compare(archiveName.size() - 4, 4, L".001") == 0)
        archiveName.resize(archiveName.size() - 4);
    std::wstring archiveType;
    auto archiveDot = archiveName.find_last_of(L'.');
    if (archiveDot != std::wstring::npos)
    {
        archiveType = archiveName.substr(archiveDot + 1);
        archiveName.resize(archiveDot);
    }
    for (wchar_t& c : archiveType)
        c = static_cast<wchar_t>(towupper(c));

    const auto replace = [](std::wstring& target, const std::wstring& tag, const std::wstring& value)
    {
        size_t pos = 0;
        while ((pos = target.find(tag, pos)) != std::wstring::npos)
        {
            target.replace(pos, tag.length(), value);
            pos += value.length();
        }
    };

    std::wstring result = titleTemplate.empty() ? L"<filename>" : titleTemplate;
    replace(result, L"<filename>", filename);
    replace(result, L"<archivetype>", archiveType);
    replace(result, L"<filetype>", extension);
    replace(result, L"<archive>", archiveName);
    replace(result, L"<#>", std::to_wstring(entryNumber));
    return result;
}

#include <shellapi.h>

// Launches the common settings UI bundled beside each native player DLL.
// Called only from an explicit player UI action, never from a decoder callback.
inline bool ArchiveAdapterOpenSettings(HMODULE module, HWND owner)
{
    if (!module)
        return false;
    wchar_t dll[MAX_PATH * 4]{};
    if (!GetModuleFileNameW(module, dll, static_cast<DWORD>(std::size(dll))))
        return false;
    const wchar_t* slash = wcsrchr(dll, L'\\');
    if (!slash)
        return false;
    const std::wstring directory(dll, slash - dll);
    const std::wstring script = directory + L"\\configure-adapters.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(owner, L"configure-adapters.ps1 is missing next to this plugin. Install the complete Archive Stream Player adapter package.",
            L"Archive Stream Player", MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::wstring arguments = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(owner, L"open", L"powershell.exe", arguments.c_str(), directory.c_str(), SW_SHOWNORMAL));
    return result > 32;
}
