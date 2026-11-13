#pragma once

// One playback and cache policy for every Archive Stream Player adapter.
// Host-specific display/logging options remain in each player's native UI.
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
    unsigned cacheAgeDays = 30;
    bool cacheAutoCleanup = true;
    bool warmSolidArchives = true;
    bool promptPasswords = true;
    bool askToSavePassword = true;
    std::wstring passwordListPath = ArchiveAdapterDefaultPasswordList();
    std::wstring titleTemplate = L"<filename>";
};

inline bool ArchiveAdapterPreferencesConfigured()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ArchiveStreamPlayer\\Adapters", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD kind = 0;
    DWORD length = 0;
    const LONG result = RegQueryValueExW(key, L"MemoryMB", nullptr, &kind, nullptr, &length);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && kind == REG_DWORD && length == sizeof(DWORD);
}

inline std::string ArchiveAdapterWideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string utf8(static_cast<size_t>(bytes), '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr) == bytes ? utf8 : std::string{};
}

inline std::wstring ArchiveAdapterUtf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    const UINT codePage = chars > 0 ? CP_UTF8 : CP_ACP;
    if (chars <= 0)
        chars = MultiByteToWideChar(codePage, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(chars), L'\0');
    return MultiByteToWideChar(codePage, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), chars) == chars ? wide : std::wstring{};
}

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
    settings.cacheAgeDays = number(L"CacheAgeDays", 30, 1, 365);
    settings.cacheAutoCleanup = number(L"CacheAutoCleanup", 1, 0, 1) != 0;
    settings.warmSolidArchives = number(L"WarmSolidArchives", 1, 0, 1) != 0;
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

// Save shared values when any player settings page changes them.  Keep the
// registry key stable so legacy plugins can adopt the same preferences.
inline bool ArchiveAdapterPreferencesSave(const ArchiveAdapterPreferences& settings)
{
    if (settings.memoryMb < 1 || settings.memoryMb > 512 || settings.cacheLimitMb > 20480 ||
        settings.cacheAgeDays < 1 || settings.cacheAgeDays > 365 || settings.passwordListPath.size() >= 4096)
        return false;

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ArchiveStreamPlayer\\Adapters", 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    const auto number = [&](const wchar_t* name, DWORD value)
    {
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    };
    const auto string = [&](const wchar_t* name, const std::wstring& value)
    {
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    };
    const bool saved = number(L"MemoryMB", settings.memoryMb) &&
        number(L"CacheLimitMB", settings.cacheLimitMb) && number(L"CacheAgeDays", settings.cacheAgeDays) &&
        number(L"CacheAutoCleanup", settings.cacheAutoCleanup ? 1 : 0) &&
        number(L"WarmSolidArchives", settings.warmSolidArchives ? 1 : 0) &&
        number(L"PromptPasswords", settings.promptPasswords ? 1 : 0) &&
        number(L"AskToSavePassword", settings.askToSavePassword ? 1 : 0) &&
        string(L"PasswordList", settings.passwordListPath) && string(L"TitleTemplate", settings.titleTemplate);
    RegCloseKey(key);
    return saved;
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
