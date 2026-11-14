#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "archive_rar_native_encrypted.h"
#include "archive_passwords.h"
#include "archive_common.h"
#include "debug_log.h"

#include <windows.h>
#include "../third_party/unrar/dll.hpp"
#include <cstring>
#include <algorithm>
#include <new>
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
        std::vector<unsigned char>* output = nullptr;
        size_t written = 0;
        bool captureFailed = false;
    };

    int CALLBACK NativeRarPasswordCallback(UINT message, LPARAM userData, LPARAM p1, LPARAM p2)
    {
        const auto* context = reinterpret_cast<const NativeRarPasswordContext*>(userData);
        if ((message == UCM_NEEDPASSWORD || message == UCM_NEEDPASSWORDW) && (!context || !context->password))
            return -1;

        if (message == UCM_PROCESSDATA)
        {
            auto* capture = reinterpret_cast<NativeRarPasswordContext*>(userData);
            if (!capture || !capture->output)
                return 1;

            const auto* chunk = reinterpret_cast<const unsigned char*>(p1);
            if (p2 < 0 || (!chunk && p2 != 0) || capture->written > capture->output->size() ||
                static_cast<size_t>(p2) > capture->output->size() - capture->written)
            {
                capture->captureFailed = true;
                return -1;
            }

            if (p2 != 0)
                memcpy(capture->output->data() + capture->written, chunk, static_cast<size_t>(p2));
            capture->written += static_cast<size_t>(p2);
            return 1;
        }

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

        // Match the existing Winamp UnRAR callbacks: accept a volume only
        // if the requested on-disk file already exists. This prevents any
        // interactive missing-volume retry loop, including RAR_VOL_ASK.
        if (message == UCM_CHANGEVOLUMEW)
        {
            const auto* next = reinterpret_cast<const wchar_t*>(p1);
            if (!next || !*next)
                return -1;

            const DWORD attributes = GetFileAttributesW(next);
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : -1;
        }

        if (message == UCM_CHANGEVOLUME)
        {
            const auto* next = reinterpret_cast<const char*>(p1);
            if (!next || !*next)
                return -1;

            const DWORD attributes = GetFileAttributesA(next);
            return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : -1;
        }

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

    bool NativeRarTryOnePasswordToMemory(const wchar_t* archivePath, uint32_t index, size_t maxBytes, const std::string& password, std::vector<unsigned char>& data, std::string& error)
    {
        data.clear();
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
            if (readResult != ERAR_SUCCESS)
            {
                error = readResult == ERAR_END_ARCHIVE ? "Selected encrypted RAR entry not found" :
                    "Native UnRAR header failed (code " + std::to_string(readResult) + ")";
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

            if ((header.Flags & RHDF_DIRECTORY) || !(header.Flags & RHDF_ENCRYPTED))
            {
                error = "Native memory fallback requires an encrypted file entry";
                return false;
            }

            const uint64_t expected = (static_cast<uint64_t>(header.UnpSizeHigh) << 32) | header.UnpSize;
            if (expected > static_cast<uint64_t>(maxBytes))
            {
                error = "Encrypted RAR entry exceeds memory limit";
                return false;
            }

            std::vector<unsigned char> decoded;
            try
            {
                decoded.resize(static_cast<size_t>(expected));
            }
            catch (const std::bad_alloc&)
            {
                error = "Insufficient memory for encrypted RAR entry";
                return false;
            }

            callbackContext.output = &decoded;
            callbackContext.written = 0;
            callbackContext.captureFailed = false;
            const int decodeResult = RARProcessFileW(archive.handle, RAR_TEST, nullptr, nullptr);
            callbackContext.output = nullptr;
            if (decodeResult != ERAR_SUCCESS || callbackContext.captureFailed || callbackContext.written != decoded.size())
            {
                error = "Native UnRAR memory test failed (code " + std::to_string(decodeResult) + ")";
                return false;
            }

            data = std::move(decoded);
            return true;
        }
    }
    std::string NativeRarArchivePathUtf8(const wchar_t* path)
    {
        const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1)
            return {};
        std::string utf8(static_cast<size_t>(needed), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8.data(), needed, nullptr, nullptr) != needed)
            return {};
        utf8.pop_back();
        return utf8;
    }

    template<typename TryDecode>
    bool NativeRarDecodeWithPasswords(const wchar_t* archivePath, uint32_t index, TryDecode&& decode, std::string& error)
    {
        const std::string archiveUtf8 = NativeRarArchivePathUtf8(archivePath);
        if (archiveUtf8.empty())
        {
            error = "Encrypted RAR archive path could not be converted to UTF-8";
            return false;
        }

        ArchivePasswordList saved;
        if (!ArchivePasswordLoadList(saved, error))
            return false;
        std::vector<std::string> runtime;
        ArchivePasswordGetRuntimePasswords(archiveUtf8.c_str(), runtime);

        // The UnRAR SDK uses process-wide internals. Do not hold a password
        // in the shared plaintext list unless verification succeeded and the
        // user explicitly consented to saving it.
        std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);
        std::string decodeError;
        for (const auto& candidate : runtime)
        {
            if (decode(candidate, decodeError))
                return true;
        }
        for (const auto& candidate : saved.passwords)
        {
            if (decode(candidate, decodeError))
            {
                ArchivePasswordAddRuntimePassword(archiveUtf8.c_str(), candidate.c_str());
                return true;
            }
        }

        // A smaller caller-provided memory bound cannot be fixed by asking
        // for another password. Preserve this hard limit and never show UI.
        if (decodeError.find("exceeds memory limit") != std::string::npos)
        {
            error = decodeError;
            return false;
        }
        if (ArchivePasswordIsArchiveCancelled(archiveUtf8.c_str()))
        {
            error = "Password prompt already cancelled for archive";
            return false;
        }
        if (!ArchivePasswordGetPromptAllowedForCurrentThread())
        {
            error = decodeError.empty() ? "No valid saved RAR password and prompting disabled" : decodeError;
            return false;
        }

        char supplied[256]{};
        bool save = false;
        if (!ArchivePasswordPromptUser(archiveUtf8.c_str(), nullptr, supplied, sizeof(supplied), &save))
        {
            SecureZeroMemory(supplied, sizeof(supplied));
            error = "Password prompt cancelled";
            return false;
        }

        std::string candidate(supplied);
        SecureZeroMemory(supplied, sizeof(supplied));
        const bool decoded = !candidate.empty() && decode(candidate, decodeError);
        if (decoded)
        {
            ArchivePasswordAddRuntimePassword(archiveUtf8.c_str(), candidate.c_str());
            ArchivePasswordNotifyAccepted(archiveUtf8.c_str());
            if (save)
            {
                std::string saveError;
                if (!ArchivePasswordAppendIfMissing(candidate.c_str(), saveError))
                    DebugLogWrite("WARN", "RAR", "VERIFIED_PASSWORD_SAVE_FAIL", "index=%u | reason=%s", static_cast<unsigned>(index), saveError.c_str());
            }
        }
        if (!candidate.empty())
            SecureZeroMemory(candidate.data(), candidate.size());
        if (!decoded)
        {
            error = "Encrypted RAR password did not decode selected entry: " + decodeError;
            return false;
        }
        DebugLogWrite("INFO", "RAR", "NATIVE_ENCRYPTED_PROMPT_DECODE_OK", "index=%u | saved=%d", static_cast<unsigned>(index), save ? 1 : 0);
        return true;
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

    return NativeRarDecodeWithPasswords(archivePath, index,
        [archivePath, index, outputPath](const std::string& password, std::string& candidateError)
        {
            return NativeRarTryOnePassword(archivePath, index, outputPath, password, candidateError);
        }, error);
}

bool RarNativeExtractEncryptedEntryToMemory(const wchar_t* archivePath, uint32_t index, size_t maxBytes, std::vector<unsigned char>& data, std::string& error)
{
    data.clear();
    error.clear();
    if (!archivePath || !*archivePath)
    {
        error = "Invalid native encrypted RAR memory request";
        return false;
    }

    return NativeRarDecodeWithPasswords(archivePath, index,
        [archivePath, index, maxBytes, &data](const std::string& password, std::string& candidateError)
        {
            return NativeRarTryOnePasswordToMemory(archivePath, index, maxBytes, password, data, candidateError);
        }, error);
}


static bool NativeRarTryListEntryHeaders(const wchar_t* archivePath, const std::string& password, bool requireEncryptedHeaders, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    NativeRarPasswordContext callbackContext{ password.empty() ? nullptr : &password };
    RAROpenArchiveDataEx open{};
    open.ArcNameW = const_cast<wchar_t*>(archivePath);
    open.OpenMode = RAR_OM_LIST;
    open.Callback = NativeRarPasswordCallback;
    open.UserData = reinterpret_cast<LPARAM>(&callbackContext);

    NativeRarArchive archive;
    archive.handle = RAROpenArchiveEx(&open);
    if (!archive.handle)
    {
        error = "Native UnRAR header open failed (code " + std::to_string(open.OpenResult) + ")";
        return false;
    }

    if (requireEncryptedHeaders && !(open.Flags & ROADF_ENCHEADERS))
    {
        error = "RAR does not use encrypted headers";
        return false;
    }

    if ((open.Flags & ROADF_ENCHEADERS) && password.empty())
    {
        error = "RAR encrypted headers require a saved password";
        return false;
    }

    if (!password.empty())
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
            error = "Native UnRAR header read failed (code " + std::to_string(readResult) + ")";
            return false;
        }

        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header.FileNameW, -1, nullptr, 0, nullptr, nullptr);
        if (count <= 1)
        {
            entries.clear();
            error = "Native UnRAR cannot convert an entry name to UTF-8";
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
        entry.isEncrypted = (header.Flags & RHDF_ENCRYPTED) != 0;
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
            error = "Native UnRAR header skip failed (code " + std::to_string(skipResult) + ")";
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
        if (!NativeRarTryListEntryHeaders(archivePath, password, true, entries, error))
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


bool RarNativeEnrichMultipartEntryEncryption(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    error.clear();
    if (!archivePath || !*archivePath || entries.empty())
    {
        error = "Cannot enrich empty native RAR entry metadata";
        return false;
    }

    std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);

    NativeRarPasswordContext callbackContext{};
    RAROpenArchiveDataEx open{};
    open.ArcNameW = const_cast<wchar_t*>(archivePath);
    open.OpenMode = RAR_OM_LIST;
    open.Callback = NativeRarPasswordCallback;
    open.UserData = reinterpret_cast<LPARAM>(&callbackContext);

    NativeRarArchive archive;
    archive.handle = RAROpenArchiveEx(&open);
    if (!archive.handle)
    {
        error = "Native RAR metadata open failed (code " + std::to_string(open.OpenResult) + ")";
        return false;
    }

    if (!(open.Flags & ROADF_VOLUME))
        return true;

    // Collect the complete header-only mapping before changing any entries,
    // so an incomplete or differently indexed volume set cannot corrupt the
    // list returned by libarchive.
    std::vector<bool> encrypted;
    encrypted.reserve(entries.size());

    for (size_t index = 0;; ++index)
    {
        RARHeaderDataEx header{};
        const int readResult = RARReadHeaderEx(archive.handle, &header);
        if (readResult == ERAR_END_ARCHIVE)
            break;

        if (readResult != ERAR_SUCCESS || index >= entries.size())
        {
            error = "Native RAR multipart metadata header mismatch (code " + std::to_string(readResult) + ")";
            return false;
        }

        const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header.FileNameW, -1, nullptr, 0, nullptr, nullptr);
        if (required <= 1)
        {
            error = "Native RAR multipart entry has an invalid Unicode name";
            return false;
        }

        std::string nativeName(static_cast<size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, header.FileNameW, -1, nativeName.data(), required, nullptr, nullptr) != required)
        {
            error = "Cannot convert native RAR multipart entry filename";
            return false;
        }
        nativeName.resize(static_cast<size_t>(required - 1));

        std::string libarchiveName = entries[index].path;
        std::replace(nativeName.begin(), nativeName.end(), '\\', '/');
        std::replace(libarchiveName.begin(), libarchiveName.end(), '\\', '/');

        const uint64_t nativeSize = (static_cast<uint64_t>(header.UnpSizeHigh) << 32) | header.UnpSize;
        if (nativeSize != entries[index].size || nativeName != libarchiveName)
        {
            error = "Native and libarchive RAR multipart entry identities differ";
            return false;
        }

        encrypted.push_back((header.Flags & RHDF_ENCRYPTED) != 0);

        const int skipResult = RARProcessFileW(archive.handle, RAR_SKIP, nullptr, nullptr);
        if (skipResult != ERAR_SUCCESS)
        {
            error = "Native RAR multipart metadata cannot advance (code " + std::to_string(skipResult) + ")";
            return false;
        }
    }

    if (encrypted.size() != entries.size())
    {
        error = "Native RAR multipart metadata entry count mismatch";
        return false;
    }

    size_t encryptedCount = 0;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        entries[i].isEncrypted = encrypted[i];
        if (encrypted[i])
            ++encryptedCount;
    }

    DebugLogWrite("INFO", "RAR", "NATIVE_MULTIPART_HEADER_ENCRYPTION", "entries=%zu | encrypted=%zu", entries.size(), encryptedCount);
    return true;
}


bool RarNativeLookupEntryMetadata(const wchar_t* archivePath, uint32_t index, ArchiveEntry& entry, std::string& error)
{
    entry = ArchiveEntry{};
    error.clear();
    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        return false;
    }

    std::vector<ArchiveEntry> entries;

    if (RarNativeHasEncryptedHeaders(archivePath))
    {
        if (!RarNativeListHeaderEncryptedEntries(archivePath, entries, error))
            return false;
    }
    else
    {
        std::lock_guard<std::mutex> guard(g_nativeRarExtractionMutex);
        if (!NativeRarTryListEntryHeaders(archivePath, std::string{}, false, entries, error))
            return false;
    }

    if (static_cast<size_t>(index) >= entries.size() || entries[index].index != index)
    {
        error = "Requested native RAR entry index is missing";
        return false;
    }

    entry = std::move(entries[index]);
    return true;
}
