#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "archive_rar_native_encrypted.h"
#include "archive_passwords.h"
#include "archive_common.h"
#include "debug_log.h"

#include <windows.h>
#include "../third_party/unrar/dll.hpp"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

namespace
{
    std::mutex g_nativeRarExtractionMutex;

    struct NativeRarArchive
    {
        HANDLE handle = nullptr;

        ~NativeRarArchive()
        {
            if (handle)
                RARCloseArchive(handle);
        }
    };

    struct NativeRarTemporaryFile
    {
        std::wstring path;

        ~NativeRarTemporaryFile()
        {
            if (!path.empty())
                DeleteFileW(path.c_str());
        }
    };

    struct NativeRarPasswordContext
    {
        const std::string* password = nullptr;
    };

    int CALLBACK NativeRarPasswordCallback(UINT message, LPARAM userData, LPARAM p1, LPARAM p2)
    {
        const auto* context = reinterpret_cast<const NativeRarPasswordContext*>(userData);
        if (!context || !context->password)
            return -1;

        if (message == UCM_NEEDPASSWORD)
        {
            char* output = reinterpret_cast<char*>(p1);
            const size_t capacity = static_cast<size_t>(p2);
            if (!output || capacity == 0 || context->password->size() + 1 > capacity)
                return -1;

            memcpy(output, context->password->c_str(), context->password->size() + 1);
            return 1;
        }

        if (message == UCM_NEEDPASSWORDW)
        {
            wchar_t* output = reinterpret_cast<wchar_t*>(p1);
            const int capacity = static_cast<int>(p2);
            if (!output || capacity <= 1)
                return -1;

            int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, context->password->c_str(), -1, output, capacity);
            if (count == 0)
                count = MultiByteToWideChar(CP_ACP, 0, context->password->c_str(), -1, output, capacity);

            return count > 0 ? 1 : -1;
        }

        // Multipart volume transitions are not enabled in this fallback until
        // the native callback path has a verified no-prompt lifecycle.
        if (message == UCM_CHANGEVOLUME || message == UCM_CHANGEVOLUMEW)
            return -1;

        return 1;
    }

    bool NativeRarTryOnePassword(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, const std::string& password, std::string& error)
    {
        error.clear();
        if (password.empty())
        {
            error = "Empty password candidate";
            return false;
        }

        NativeRarPasswordContext callbackContext{ &password };
        RAROpenArchiveDataEx open{};
        open.ArcNameW = const_cast<wchar_t*>(archivePath);
        open.OpenMode = RAR_OM_EXTRACT;
        open.Callback = NativeRarPasswordCallback;
        open.UserData = reinterpret_cast<LPARAM>(&callbackContext);

        NativeRarArchive archive;
        archive.handle = RAROpenArchiveEx(&open);
        if (!archive.handle)
        {
            error = "Native UnRAR failed to open archive (code " + std::to_string(open.OpenResult) + ")";
            return false;
        }

        RARSetPassword(archive.handle, const_cast<char*>(password.c_str()));

        uint32_t currentIndex = 0;
        for (;;)
        {
            RARHeaderDataEx header{};
            const int readResult = RARReadHeaderEx(archive.handle, &header);
            if (readResult == ERAR_END_ARCHIVE)
            {
                error = "Selected encrypted RAR entry not found";
                return false;
            }
            if (readResult != ERAR_SUCCESS)
            {
                error = "Native UnRAR header failed (code " + std::to_string(readResult) + ")";
                return false;
            }

            if (currentIndex != index)
            {
                const int skipResult = RARProcessFileW(archive.handle, (header.Flags & RHDF_SOLID) ? RAR_TEST : RAR_SKIP, nullptr, nullptr);
                if (skipResult != ERAR_SUCCESS)
                {
                    error = "Native UnRAR could not advance to selected entry (code " + std::to_string(skipResult) + ")";
                    return false;
                }

                ++currentIndex;
                continue;
            }

            if (header.Flags & RHDF_DIRECTORY)
            {
                error = "Selected RAR entry is a directory";
                return false;
            }

            if (!(header.Flags & RHDF_ENCRYPTED))
            {
                error = "Native fallback is restricted to encrypted RAR entries";
                return false;
            }

            const std::wstring destination(outputPath);
            const size_t slash = destination.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                error = "Native fallback requires an absolute destination path";
                return false;
            }

            const std::wstring parent = destination.substr(0, slash + 1);
            wchar_t tempName[MAX_PATH] = {};
            if (!GetTempFileNameW(parent.c_str(), L"ARE", 0, tempName))
            {
                error = "Native fallback could not allocate temporary file";
                return false;
            }

            NativeRarTemporaryFile staged{ tempName };
            // GetTempFileName creates an empty file. UnRAR needs to create it
            // itself to avoid an interactive overwrite prompt.
            if (!DeleteFileW(staged.path.c_str()))
            {
                error = "Native fallback could not prepare temporary file";
                return false;
            }

            const int extractionResult = RARProcessFileW(archive.handle, RAR_EXTRACT, nullptr, const_cast<wchar_t*>(staged.path.c_str()));
            if (extractionResult != ERAR_SUCCESS)
            {
                error = "Native UnRAR extraction failed (code " + std::to_string(extractionResult) + ")";
                return false;
            }

            WIN32_FILE_ATTRIBUTE_DATA attrs{};
            if (!GetFileAttributesExW(staged.path.c_str(), GetFileExInfoStandard, &attrs))
            {
                error = "Native UnRAR did not produce the expected temporary file";
                return false;
            }

            const uint64_t fileSize = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
            const uint64_t expected = (static_cast<uint64_t>(header.UnpSizeHigh) << 32) | header.UnpSize;
            if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fileSize != expected)
            {
                error = "Native UnRAR output size does not match archive header";
                return false;
            }

            if (!MoveFileExW(staged.path.c_str(), outputPath, MOVEFILE_REPLACE_EXISTING))
            {
                error = "Native UnRAR could not install extracted file";
                return false;
            }

            staged.path.clear();
            return true;
        }
    }
}

bool RarNativeExtractEncryptedEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    error.clear();
    if (!archivePath || !*archivePath || !outputPath || !*outputPath)
    {
        error = "Invalid native encrypted RAR extraction request";
        return false;
    }

    ArchivePasswordList passwordList;
    if (!ArchivePasswordLoadList(passwordList, error))
        return false;

    if (passwordList.passwords.empty())
    {
        error = "No saved RAR passwords are available";
        return false;
    }

    // UnRAR uses process-wide internal state, so serialize its fallback calls.
    std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);

    std::string candidateError;
    for (const std::string& password : passwordList.passwords)
    {
        if (NativeRarTryOnePassword(archivePath, index, outputPath, password, candidateError))
        {
            DebugLogWrite("INFO", "RAR", "NATIVE_ENCRYPTED_FALLBACK_OK", "index=%u | password_candidates=%zu", static_cast<unsigned>(index), passwordList.passwords.size());
            return true;
        }
    }

    error = "Native encrypted RAR extraction failed: " + candidateError;
    DebugLogWrite("WARN", "RAR", "NATIVE_ENCRYPTED_FALLBACK_FAIL", "index=%u | candidates=%zu | error=%s", static_cast<unsigned>(index), passwordList.passwords.size(), error.c_str());
    return false;
}


static bool NativeRarTryListEncryptedHeaders(const wchar_t* archivePath, const std::string& password, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    NativeRarPasswordContext callbackContext{ &password };
    RAROpenArchiveDataEx open{};
    open.ArcNameW = const_cast<wchar_t*>(archivePath);
    open.OpenMode = RAR_OM_LIST;
    open.Callback = NativeRarPasswordCallback;
    open.UserData = reinterpret_cast<LPARAM>(&callbackContext);

    NativeRarArchive archive;
    archive.handle = RAROpenArchiveEx(&open);
    if (!archive.handle)
    {
        error = "Native UnRAR encrypted-header open failed (code " + std::to_string(open.OpenResult) + ")";
        return false;
    }

    if (!(open.Flags & ROADF_ENCHEADERS))
    {
        error = "RAR does not use encrypted headers";
        return false;
    }

    RARSetPassword(archive.handle, const_cast<char*>(password.c_str()));

    uint32_t index = 0;
    for (;;)
    {
        RARHeaderDataEx header{};
        const int readResult = RARReadHeaderEx(archive.handle, &header);
        if (readResult == ERAR_END_ARCHIVE)
            return true;

        if (readResult != ERAR_SUCCESS)
        {
            entries.clear();
            error = "Native UnRAR encrypted-header read failed (code " + std::to_string(readResult) + ")";
            return false;
        }

        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header.FileNameW, -1, nullptr, 0, nullptr, nullptr);
        if (count <= 1)
        {
            entries.clear();
            error = "Native UnRAR cannot convert an encrypted entry name to UTF-8";
            return false;
        }

        std::string utf8(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header.FileNameW, -1, utf8.data(), count, nullptr, nullptr) != count)
        {
            entries.clear();
            error = "Native UnRAR entry filename conversion failed";
            return false;
        }

        utf8.resize(static_cast<size_t>(count - 1));

        ArchiveEntry entry;
        entry.index = index;
        entry.path = utf8;
        entry.size = (static_cast<uint64_t>(header.UnpSizeHigh) << 32) | header.UnpSize;
        entry.isFolder = (header.Flags & RHDF_DIRECTORY) != 0;
        entry.isSupportedAudio = !entry.isFolder && ArchiveIsSupportedAudioPath(entry.path);
        entry.isSolid = (header.Flags & RHDF_SOLID) != 0;
        entry.methodId = header.Method;
        entry.hasMethodId = true;

        // Never derive CRC from decompressed content. A RAR5 CRC is usable
        // only when its header explicitly advertises the CRC32 hash type.
        entry.hasCrc32 = header.HashType == RAR_HASH_CRC32 &&
            !(header.Flags & (RHDF_SPLITBEFORE | RHDF_SPLITAFTER));
        entry.crc32 = entry.hasCrc32 ? header.FileCRC : 0;
        entry.streamCandidate = false;
        entries.push_back(std::move(entry));

        const int skipResult = RARProcessFileW(archive.handle, RAR_SKIP, nullptr, nullptr);
        if (skipResult != ERAR_SUCCESS)
        {
            entries.clear();
            error = "Native UnRAR encrypted-header skip failed (code " + std::to_string(skipResult) + ")";
            return false;
        }

        ++index;
    }
}

bool RarNativeListHeaderEncryptedEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    if (!archivePath || !*archivePath)
    {
        error = "RAR archive path is empty";
        return false;
    }

    ArchivePasswordList passwords;
    if (!ArchivePasswordLoadList(passwords, error))
        return false;

    if (passwords.passwords.empty())
    {
        error = "No saved RAR passwords are available for encrypted headers";
        return false;
    }

    std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);
    for (const std::string& password : passwords.passwords)
    {
        if (!NativeRarTryListEncryptedHeaders(archivePath, password, entries, error))
            continue;

        DebugLogWrite("INFO", "RAR", "NATIVE_ENCRYPTED_HEADER_LIST_OK", "entries=%zu", entries.size());
        return true;
    }

    return false;
}


bool RarNativeHasEncryptedHeaders(const wchar_t* archivePath)
{
    if (!archivePath || !*archivePath)
        return false;

    std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);

    RAROpenArchiveDataEx open{};
    open.ArcNameW = const_cast<wchar_t*>(archivePath);
    open.OpenMode = RAR_OM_LIST;

    NativeRarArchive archive;
    archive.handle = RAROpenArchiveEx(&open);
    return archive.handle && (open.Flags & ROADF_ENCHEADERS) != 0;
}
