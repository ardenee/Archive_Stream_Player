#define NOMINMAX

#include "archive_passwords.h"

#include "archive_common.h"
#include "debug_log.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>


// -----------------------------------------------------------------------------
// Shared password-list state
//
// The path is process-local and shared by all archive backends. If no explicit
// path is set, ArchivePasswordGetListPath() builds:
//
//     %LOCALAPPDATA%\<selected data folder>\keeps.txt
// -----------------------------------------------------------------------------
static char g_archivePasswordListPath[MAX_PATH * 4] = "";
static ArchivePasswordPromptCallback g_archivePasswordPromptCallback = nullptr;
static ArchivePasswordAcceptedCallback g_archivePasswordAcceptedCallback = nullptr;
static __declspec(thread) bool g_archivePasswordPromptAllowedForCurrentThread = true;

struct ArchiveRuntimePasswordEntry
{
    std::string archivePath;
    std::vector<std::string> passwords;
};

// These vectors are read and updated by independent HTTP playback threads.
// Their lock must never be held across a password dialog or UnRAR decoding.
static std::mutex g_archivePasswordRuntimeMutex;
// Serialize the read/check/append sequence for plaintext passwords within a
// process, so two successful decodes cannot append the same key twice.
static std::mutex g_archivePasswordSaveMutex;
static std::vector<ArchiveRuntimePasswordEntry> g_archiveRuntimePasswords;
static std::vector<std::string> g_archivePasswordCancelledArchives;
static volatile LONG g_archivePasswordPromptActive = 0;

static bool ArchivePasswordStringEquals(const std::string& a, const char* b)
{
    return b && _stricmp(a.c_str(), b) == 0;
}

static ArchiveRuntimePasswordEntry* ArchivePasswordFindRuntimeEntry(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return nullptr;
    }

    for (ArchiveRuntimePasswordEntry& entry : g_archiveRuntimePasswords)
    {
        if (ArchivePasswordStringEquals(entry.archivePath, archivePath))
        {
            return &entry;
        }
    }

    return nullptr;
}

static void ArchivePasswordTrimLine(std::string& line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
    {
        line.pop_back();
    }

    size_t start = 0;

    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
    {
        ++start;
    }

    if (start > 0)
    {
        line.erase(0, start);
    }
}

static bool ArchivePasswordContains(const std::vector<std::string>& passwords, const std::string& password)
{
    return std::find(passwords.begin(), passwords.end(), password) != passwords.end();
}

// Settings UIs pass UTF-8 paths; legacy Winamp's ANSI INI path is also
// accepted for backward compatibility. Windows filesystem operations use
// native Unicode paths so non-ASCII folders are never lost on conversion.
static std::wstring ArchivePasswordPathToWide(const char* path)
{
    if (!path || !*path)
        return {};

    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(codepage, flags, path, -1, nullptr, 0);
    if (count <= 0)
    {
        codepage = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(codepage, flags, path, -1, nullptr, 0);
    }
    if (count <= 0)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(codepage, flags, path, -1, result.data(), count) != count)
        return {};
    result.pop_back();
    return result;
}

// A process-local std::mutex is insufficient when Winamp, foobar2000,
// AIMP and bridge processes share the same plaintext keeps.txt. Use a
// Windows named mutex scoped to the normalized absolute password-list path.
// No additional lock file is written and abandoned mutexes recover on exit.
class ArchivePasswordCrossProcessSaveLock
{
public:
    ~ArchivePasswordCrossProcessSaveLock()
    {
        if (acquired_)
            ReleaseMutex(handle_);
        if (handle_)
            CloseHandle(handle_);
    }

    bool Acquire(const std::wstring& path, std::string& error)
    {
        const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (required == 0 || required > 32768)
        {
            error = "Cannot normalize password list path for write synchronization";
            return false;
        }

        std::wstring normalized(static_cast<size_t>(required), L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), required, normalized.data(), nullptr);
        if (written == 0 || written >= required)
        {
            error = "Cannot resolve password list path for write synchronization";
            return false;
        }
        normalized.resize(written);
        CharLowerBuffW(normalized.data(), written);

        // Stable over process bitness and independent of which user profile
        // supplied the path. Hash Unicode UTF-16 code units as two bytes.
        uint64_t hash = 14695981039346656037ULL;
        for (wchar_t c : normalized)
        {
            const uint16_t unit = static_cast<uint16_t>(c);
            hash = (hash ^ (unit & 0xFF)) * 1099511628211ULL;
            hash = (hash ^ (unit >> 8)) * 1099511628211ULL;
        }
        wchar_t name[96]{};
        if (swprintf_s(name, L"Global\\ArchiveStreamPlayer.PasswordList.%016llX", static_cast<unsigned long long>(hash)) < 0)
        {
            error = "Cannot format password-list synchronization key";
            return false;
        }
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (!handle_)
        {
            error = "Cannot create cross-process password-list mutex";
            return false;
        }
        const DWORD status = WaitForSingleObject(handle_, 15000);
        if (status != WAIT_OBJECT_0 && status != WAIT_ABANDONED)
        {
            error = status == WAIT_TIMEOUT ? "Timed out waiting for another player's password save" : "Password-list synchronization wait failed";
            return false;
        }
        acquired_ = true;
        return true;
    }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

void ArchivePasswordSetListPath(const char* passwordListPath)
{
    if (!passwordListPath)
    {
        passwordListPath = "";
    }

    strcpy_s(g_archivePasswordListPath, sizeof(g_archivePasswordListPath), passwordListPath);
}

const char* ArchivePasswordGetListPath()
{
    if (g_archivePasswordListPath[0])
    {
        return g_archivePasswordListPath;
    }

    char path[MAX_PATH * 4];
    path[0] = '\0';

    if (!ArchiveBuildPluginDataFilePath("keeps.txt", path, sizeof(path)))
    {
        DebugLogWrite("ERROR", "PASSWORD", "PATH_FAIL", "reason=build_default_keep_path_failed");
        return "";
    }

    strcpy_s(g_archivePasswordListPath, sizeof(g_archivePasswordListPath), path);
    return g_archivePasswordListPath;
}

bool ArchivePasswordLoadList(ArchivePasswordList& outList, std::string& error)
{
    outList.passwords.clear();
    error.clear();

    const char* path = ArchivePasswordGetListPath();

    if (!path || !path[0])
    {
        error = "Password list path is empty";
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "reason=empty_path");
        return false;
    }

    const std::wstring widePath = ArchivePasswordPathToWide(path);
    if (widePath.empty())
    {
        error = "Password list path cannot be decoded";
        return false;
    }
    DWORD attrs = GetFileAttributesW(widePath.c_str());

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        DWORD winerr = GetLastError();

        if (winerr == ERROR_FILE_NOT_FOUND || winerr == ERROR_PATH_NOT_FOUND)
        {
            return true;
        }

        error = "Failed to stat password list";
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "path=%s | reason=get_attributes_failed | winerr=%lu", path, winerr);
        return false;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        error = "Password list path is a directory";
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "path=%s | reason=is_directory", path);
        return false;
    }

    std::ifstream file(std::filesystem::path(widePath), std::ios::in | std::ios::binary);

    if (!file)
    {
        error = "Failed to open password list";
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "path=%s | reason=open_failed", path);
        return false;
    }

    std::string line;

    while (std::getline(file, line))
    {
        ArchivePasswordTrimLine(line);

        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        if (!ArchivePasswordContains(outList.passwords, line))
        {
            outList.passwords.push_back(line);
        }
    }

    return true;
}

bool ArchivePasswordAppendIfMissing(const char* password, std::string& error)
{
    error.clear();

    if (!password || !password[0])
    {
        error = "Password is empty";
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "reason=empty_password");
        return false;
    }

    std::lock_guard<std::mutex> guard(g_archivePasswordSaveMutex);
    const char* path = ArchivePasswordGetListPath();

    if (!path || !path[0])
    {
        error = "Password list path is empty";
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "reason=empty_path");
        return false;
    }

    const std::wstring widePath = ArchivePasswordPathToWide(path);
    if (widePath.empty())
    {
        error = "Password list path cannot be decoded";
        return false;
    }

    // Hold across LoadList, duplicate detection and the final append.
    // Lock order: process-local save mutex, then the Windows named mutex.
    ArchivePasswordCrossProcessSaveLock crossProcessLock;
    if (!crossProcessLock.Acquire(widePath, error))
    {
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "reason=cross_process_lock_failed | error=%s", error.c_str());
        return false;
    }

    ArchivePasswordList existing;
    if (!ArchivePasswordLoadList(existing, error))
    {
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "reason=load_existing_failed | error=%s", error.c_str());
        return false;
    }
    if (ArchivePasswordContains(existing.passwords, password))
        return true;

    bool needsLeadingNewline = false;

    FILE* checkFile = nullptr;
    errno_t checkOpenResult = _wfopen_s(&checkFile, widePath.c_str(), L"rb");

    if (checkOpenResult == 0 && checkFile)
    {
        if (fseek(checkFile, 0, SEEK_END) == 0)
        {
            long fileSize = ftell(checkFile);

            if (fileSize > 0 && fseek(checkFile, -1, SEEK_END) == 0)
            {
                int lastChar = fgetc(checkFile);

                if (lastChar != '\n')
                {
                    needsLeadingNewline = true;
                }
            }
        }

        fclose(checkFile);
    }

    FILE* file = nullptr;
    errno_t openResult = _wfopen_s(&file, widePath.c_str(), L"ab");

    if (openResult != 0 || !file)
    {
        error = "Failed to open password list for append";
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "path=%s | reason=open_append_failed | errno=%d", path, (int)openResult);
        return false;
    }

    std::string line;

    if (needsLeadingNewline)
    {
        line += "\r\n";
    }

    line += password;
    line += "\r\n";

    size_t written = fwrite(line.data(), 1, line.size(), file);
    fclose(file);

    if (written != line.size())
    {
        error = "Failed to write password list entry";
        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "path=%s | reason=write_failed | requested=%zu | written=%zu", path, line.size(), written);
        return false;
    }

    return true;
}

void ArchivePasswordSetPromptCallback(ArchivePasswordPromptCallback callback)
{
    g_archivePasswordPromptCallback = callback;
}

bool ArchivePasswordPromptUser(const char* archivePath, const char* entryName, char* outPassword, size_t outPasswordSize, bool* outSavePassword)
{
    if (outPassword && outPasswordSize > 0)
    {
        outPassword[0] = '\0';
    }

    if (outSavePassword)
    {
        *outSavePassword = false;
    }

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SKIP", "archive= | entry=%s | reason=missing_archive_path", entryName ? entryName : "");
        return false;
    }

    if (ArchivePasswordIsArchiveCancelled(archivePath))
    {
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SUPPRESSED", "archive=%s | entry=%s | reason=archive_cancelled", archivePath, entryName ? entryName : "");
        return false;
    }

    if (!g_archivePasswordPromptAllowedForCurrentThread)
    {
        DebugLogWrite("INFO", "PASSWORD", "PROMPT_SKIP", "archive=%s | entry=%s | reason=prompt_disabled_for_thread", archivePath, entryName ? entryName : "");
        return false;
    }

    if (!g_archivePasswordPromptCallback)
    {
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SKIP", "archive=%s | entry=%s | reason=no_prompt_callback", archivePath, entryName ? entryName : "");
        return false;
    }

    if (InterlockedCompareExchange(&g_archivePasswordPromptActive, 1, 0) != 0)
    {
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SKIP", "archive=%s | entry=%s | reason=prompt_already_active", archivePath, entryName ? entryName : "");
        return false;
    }

    bool result = g_archivePasswordPromptCallback(archivePath, entryName, outPassword, outPasswordSize, outSavePassword);

    InterlockedExchange(&g_archivePasswordPromptActive, 0);

    if (!result)
    {
        ArchivePasswordMarkArchiveCancelled(archivePath);
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_CANCELLED", "archive=%s | entry=%s | scope=archive", archivePath, entryName ? entryName : "");
        return false;
    }

    if (!outPassword || !outPassword[0])
    {
        ArchivePasswordMarkArchiveCancelled(archivePath);
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_EMPTY", "archive=%s | entry=%s | scope=archive", archivePath, entryName ? entryName : "");
        return false;
    }

    ArchivePasswordClearArchiveCancelled(archivePath);
    return true;
}

void ArchivePasswordAddRuntimePassword(const char* archivePath, const char* password)
{
    if (!archivePath || !archivePath[0] || !password || !password[0])
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_archivePasswordRuntimeMutex);
    ArchiveRuntimePasswordEntry* entry = ArchivePasswordFindRuntimeEntry(archivePath);

    if (!entry)
    {
        ArchiveRuntimePasswordEntry newEntry;
        newEntry.archivePath = archivePath;
        g_archiveRuntimePasswords.push_back(newEntry);
        entry = &g_archiveRuntimePasswords.back();
    }

    if (!ArchivePasswordContains(entry->passwords, password))
    {
        entry->passwords.push_back(password);
    }
}

void ArchivePasswordGetRuntimePasswords(const char* archivePath, std::vector<std::string>& outPasswords)
{
    outPasswords.clear();

    std::lock_guard<std::mutex> guard(g_archivePasswordRuntimeMutex);
    ArchiveRuntimePasswordEntry* entry = ArchivePasswordFindRuntimeEntry(archivePath);

    if (!entry)
    {
        return;
    }

    outPasswords = entry->passwords;
}

void ArchivePasswordMarkArchiveCancelled(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_archivePasswordRuntimeMutex);
    if (!ArchivePasswordContains(g_archivePasswordCancelledArchives, archivePath))
    {
        g_archivePasswordCancelledArchives.push_back(archivePath);
    }
}

bool ArchivePasswordIsArchiveCancelled(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return false;
    }

    std::lock_guard<std::mutex> guard(g_archivePasswordRuntimeMutex);
    return ArchivePasswordContains(g_archivePasswordCancelledArchives, archivePath);
}

void ArchivePasswordClearArchiveCancelled(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return;
    }

    std::lock_guard<std::mutex> guard(g_archivePasswordRuntimeMutex);
    g_archivePasswordCancelledArchives.erase(
        std::remove_if(
            g_archivePasswordCancelledArchives.begin(),
            g_archivePasswordCancelledArchives.end(),
            [archivePath](const std::string& value)
            {
                return _stricmp(value.c_str(), archivePath) == 0;
            }
        ),
        g_archivePasswordCancelledArchives.end()
    );
}

void ArchivePasswordSetAcceptedCallback(ArchivePasswordAcceptedCallback callback)
{
    g_archivePasswordAcceptedCallback = callback;
}

void ArchivePasswordNotifyAccepted(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return;
    }

    if (g_archivePasswordAcceptedCallback)
    {
        g_archivePasswordAcceptedCallback(archivePath);
    }
}

void ArchivePasswordSetPromptAllowedForCurrentThread(bool allowed)
{
    g_archivePasswordPromptAllowedForCurrentThread = allowed;
}

bool ArchivePasswordGetPromptAllowedForCurrentThread()
{
    return g_archivePasswordPromptAllowedForCurrentThread;
}