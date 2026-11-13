// Player-neutral decoded archive streams presented as foobar2000 virtual files.
// The player's existing audio input decoders handle MP3/FLAC/OGG/WAV/etc.
// Unsupported direct stream layouts can use an entry-scoped temporary file.
// Archive content CRCs are never calculated; metadata comes from headers.
#include <foobar2000/SDK/foobar2000.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "archive_format.h"
#include "archive_rar_native_encrypted.h"
#include "archive_stream_owner.h"
#include "foobar_archive_reader.h"
#include "foobar_settings.h"
#include "debug_log.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace
{
    std::wstring Utf8ToWide(const char* value)
    {
        if (!value || !*value)
            return {};

        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
        if (count < 2)
            return {};

        std::wstring output(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, output.data(), count) != count)
            return {};

        output.resize(static_cast<size_t>(count - 1));
        return output;
    }

    std::wstring LocalArchivePath(const char* path, abort_callback& abort)
    {
        abort.check();
        pfc::string8 native;
        filesystem::g_get_native_path(path, native, abort);

        // The backend operates on real native filesystem paths and must not
        // silently treat remote URLs or nested virtual entries as disk files.
        if (std::strstr(native.c_str(), "://"))
            throw exception_io_unsupported_format();

        std::wstring wide = Utf8ToWide(native.c_str());
        if (wide.empty())
            throw exception_io_not_found();

        return wide;
    }

    std::string VirtualEntryPath(const ArchiveEntry& entry)
    {
        // Index makes duplicate internal filenames unambiguous. The final
        // path segment retains the audio extension for foobar2000 decoders.
        const auto slash = entry.path.find_last_of("/\\");
        const std::string name = entry.path.substr(slash == std::string::npos ? 0 : slash + 1);
        return std::to_string(entry.index) + "/" + name;
    }

    bool ParseVirtualEntryPath(const char* virtualPath, uint32_t& index, std::string& name)
    {
        if (!virtualPath)
            return false;

        const char* slash = std::strchr(virtualPath, '/');
        if (!slash || slash == virtualPath || !slash[1])
            return false;

        const auto result = std::from_chars(virtualPath, slash, index);
        if (result.ec != std::errc() || result.ptr != slash)
            return false;

        name.assign(slash + 1);
        return name.find_first_of("/\\") == std::string::npos && name != "." && name != "..";
    }

    ArchiveEntry FindEntry(const wchar_t* path, const char* virtualPath, abort_callback& abort)
    {
        uint32_t index = 0;
        std::string expectedName;
        if (!ParseVirtualEntryPath(virtualPath, index, expectedName))
            throw exception_io_not_found();

        // Native UnRAR header lookup avoids repeatedly opening the bundled
        // libarchive RAR decoder after a prior encrypted entry was decoded.
        // Require the exact index/virtual filename to prevent incorrect routing.
        if (DetectArchiveFormatFromPath(path) == ArchiveFormat::Rar)
        {
            ArchiveEntry nativeEntry;
            std::string nativeError;
            const bool nativeLookupSucceeded = RarNativeLookupEntryMetadata(path, index, nativeEntry, nativeError);
            if (nativeLookupSucceeded && nativeEntry.isEncrypted)
            {
                abort.check();
                if (nativeEntry.isFolder || !nativeEntry.isSupportedAudio || VirtualEntryPath(nativeEntry) != virtualPath)
                    throw exception_io_not_found();

                DebugLogWrite("INFO", "FOOBAR", "FIND_NATIVE_RAR_MATCH", "index=%u | encrypted=%d | bytes=%llu", (unsigned)index, nativeEntry.isEncrypted ? 1 : 0, (unsigned long long)nativeEntry.size);
                return nativeEntry;
            }

            if (!nativeLookupSucceeded)
                DebugLogWrite("WARN", "FOOBAR", "FIND_NATIVE_RAR_FALLBACK", "index=%u | error=%s", (unsigned)index, nativeError.c_str());
        }

        abort.check();
        std::vector<ArchiveEntry> entries;
        std::string error;
        if (!ArchiveListEntries(path, entries, error))
            pfc::throw_exception_with_message<exception_io_data>(error.c_str());

        abort.check();
        for (const auto& entry : entries)
        {
            if (entry.index != index)
                continue;

            if (entry.isFolder || !entry.isSupportedAudio || VirtualEntryPath(entry) != virtualPath)
                throw exception_io_not_found();

            return entry;
        }

        throw exception_io_not_found();
    }

    class ArchiveEntryFile : public file_readonly
    {
    public:
        ArchiveEntryFile(const std::wstring& path, const ArchiveEntry& entry, t_filetimestamp timestamp, abort_callback& abort)
            : path_(path), entry_(entry), timestamp_(timestamp)
        {
            abort.check();
            // libarchive may expose encrypted RAR headers but cannot decode
            // some RAR5 data. Route such entries directly through the native
            // UnRAR extraction fallback; do not first poison a stream state.
            if (entry_.isEncrypted && DetectArchiveFormatFromPath(path_.c_str()) == ArchiveFormat::Rar)
            {
                OpenMemoryOrExtractedFallback("Encrypted RAR uses native UnRAR fallback", abort);
                return;
            }

            // Solid 7z folders have no independent incremental entry stream.
            // Do not ask the backend to open one (or prompt for a password
            // twice); choose memory first, then the temporary-file cache.
            if (entry_.isSolid && DetectArchiveFormatFromPath(path_.c_str()) == ArchiveFormat::SevenZip)
            {
                OpenMemoryOrExtractedFallback("Solid 7z folder is not independently streamable", abort);
                return;
            }

            const auto result = stream_.OpenDetailed(path_.c_str(), entry_.index);
            abort.check();
            if (!result.success)
            {
                // Some valid 7z/solid coder layouts cannot be opened by the
                // incremental decoder. A uniquely named foobar-owned temp
                // file is a deliberate fallback; no content CRC is computed.
                DebugLogWrite("INFO", "FOOBAR", "STREAM_FALLBACK", "index=%u | reason=%s", (unsigned)entry_.index, result.message.c_str());
                OpenMemoryOrExtractedFallback(result.message, abort);
            }
        }

        ~ArchiveEntryFile()
        {
            CloseExtractedFallback();
        }

        t_size read(void* destination, t_size amount, abort_callback& abort) override
        {
            abort.check();
            if (!memoryData_.empty())
            {
                const t_size available = static_cast<t_size>(memoryData_.size() - memoryPosition_);
                const t_size count = (std::min)(amount, available);
                std::memcpy(destination, memoryData_.data() + memoryPosition_, static_cast<size_t>(count));
                memoryPosition_ += static_cast<size_t>(count);
                abort.check();
                return count;
            }
            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                DWORD done = 0;
                const DWORD limit = static_cast<DWORD>((std::min)(amount, static_cast<t_size>((std::numeric_limits<DWORD>::max)())));
                if (!ReadFile(tempFile_, destination, limit, &done, nullptr))
                    exception_io_from_win32(GetLastError());

                abort.check();
                return done;
            }

            // Keep the cancellation notification scope shorter than stream
            // destruction, since the notify callback borrows the stream owner.
            const uint64_t readStart = stream_.Tell();
            ArchiveStreamReadResult result;
            {
                fb2k::abort_notify notifyAbort(abort, [this]() { stream_.Cancel(); });
                result = stream_.ReadDetailed(destination, amount);
            }

            abort.check();
            if (result.errorKind != ArchiveStreamErrorKind::None)
            {
                // If the decoder fails after earlier successful reads, the
                // caller has not received any bytes from this failed call.
                // Rebuild the entry in RAM or disk, then resume at its
                // original offset rather than replaying or skipping data.
                if ((result.errorKind == ArchiveStreamErrorKind::BackendFailure ||
                     result.errorKind == ArchiveStreamErrorKind::TruncatedEntry) && readStart < entry_.size)
                {
                    RecoverFromStreamFailure(result.message, readStart, abort);
                    return read(destination, amount, abort);
                }

                pfc::throw_exception_with_message<exception_io_data>(result.message.c_str());
            }

            return result.bytesRead;
        }

        t_filesize get_size(abort_callback& abort) override
        {
            abort.check();
            return entry_.size;
        }

        t_filesize get_position(abort_callback& abort) override
        {
            abort.check();
            if (!memoryData_.empty())
                return memoryPosition_;            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER zero{}, position{};
                if (!SetFilePointerEx(tempFile_, zero, &position, FILE_CURRENT))
                    exception_io_from_win32(GetLastError());

                return static_cast<t_filesize>(position.QuadPart);
            }

            return stream_.Tell();
        }

        void seek(t_filesize position, abort_callback& abort) override
        {
            abort.check();
            if (position > entry_.size || position > static_cast<t_filesize>((std::numeric_limits<LONGLONG>::max)()))
                throw exception_io_seek_out_of_range();

            if (!memoryData_.empty())
            {
                memoryPosition_ = static_cast<size_t>(position);
                return;
            }
            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER target{};
                target.QuadPart = static_cast<LONGLONG>(position);
                if (!SetFilePointerEx(tempFile_, target, nullptr, FILE_BEGIN))
                    exception_io_from_win32(GetLastError());

                abort.check();
                return;
            }

            ArchiveStreamOperationResult result;
            {
                fb2k::abort_notify notifyAbort(abort, [this]() { stream_.Cancel(); });
                result = stream_.SeekDetailed(position);
            }
            abort.check();
            if (!result.success)
            {
                if (result.errorKind == ArchiveStreamErrorKind::BackendFailure)
                {
                    RecoverFromStreamFailure(result.message, position, abort);
                    return;
                }

                pfc::throw_exception_with_message<exception_io_data>(result.message.c_str());
            }

            abort.check();
        }

        bool can_seek() override { return true; }
        bool get_content_type(pfc::string_base&) override { return false; }
        bool is_remote() override { return false; }

        t_filetimestamp get_timestamp(abort_callback& abort) override
        {
            abort.check();
            return timestamp_;
        }

        void reopen(abort_callback& abort) override
        {
            seek(0, abort);
        }

    private:
        void RecoverFromStreamFailure(const std::string& originalError, uint64_t position, abort_callback& abort)
        {
            abort.check();
            DebugLogWrite("WARN", "FOOBAR", "STREAM_RECOVER", "index=%u | resume_at=%llu | error=%s", (unsigned)entry_.index, (unsigned long long)position, originalError.c_str());
            stream_.Close();
            OpenMemoryOrExtractedFallback(originalError, abort);
            seek(position, abort);
            DebugLogWrite("INFO", "FOOBAR", "STREAM_RECOVER_OK", "index=%u | resume_at=%llu | route=%s", (unsigned)entry_.index, (unsigned long long)position, !memoryData_.empty() ? "memory" : "cache");
        }

        void CloseExtractedFallback() noexcept
        {
            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(tempFile_);
                tempFile_ = INVALID_HANDLE_VALUE;
            }

            if (!tempPath_.empty())
            {
                DeleteFileW(tempPath_.c_str());
                tempPath_.clear();
            }
        }

        void OpenMemoryOrExtractedFallback(const std::string& streamError, abort_callback& abort)
        {
            abort.check();
            const uint64_t memoryLimit = static_cast<uint64_t>(FoobarArchiveSettingsLoad().bufferSizeMb) * 1024 * 1024;
            const bool nativeEncryptedRar = entry_.isEncrypted && DetectArchiveFormatFromPath(path_.c_str()) == ArchiveFormat::Rar;
            if (entry_.size > 0 && entry_.size <= memoryLimit && entry_.size <= SIZE_MAX)
            {
                try
                {
                    std::string error;
                    std::vector<unsigned char> extracted;
                    const bool decoded = nativeEncryptedRar
                        ? RarNativeExtractEncryptedEntryToMemory(path_.c_str(), entry_.index, static_cast<size_t>(memoryLimit), extracted, error)
                        : ArchiveExtractEntryToMemory(path_.c_str(), entry_.index, extracted, error);
                    if (decoded && extracted.size() == entry_.size)
                    {
                        abort.check();
                        memoryData_ = std::move(extracted);
                        memoryPosition_ = 0;
                        DebugLogWrite("INFO", "FOOBAR", "FALLBACK_MEMORY_READY", "index=%u | bytes=%zu | limit=%llu", (unsigned)entry_.index, memoryData_.size(), (unsigned long long)memoryLimit);
                        return;
                    }
                    DebugLogWrite("WARN", "FOOBAR", "FALLBACK_MEMORY_FAIL", "index=%u | expected=%llu | extracted=%zu | error=%s", (unsigned)entry_.index, (unsigned long long)entry_.size, extracted.size(), error.c_str());
                }
                catch (const std::bad_alloc&)
                {
                    DebugLogWrite("WARN", "FOOBAR", "FALLBACK_MEMORY_FAIL", "index=%u | reason=not_enough_memory", (unsigned)entry_.index);
                }
            }
            else
            {
                DebugLogWrite("INFO", "FOOBAR", "FALLBACK_MEMORY_SKIP", "index=%u | bytes=%llu | limit=%llu | native_encrypted_rar=%d", (unsigned)entry_.index, (unsigned long long)entry_.size, (unsigned long long)memoryLimit, nativeEncryptedRar ? 1 : 0);
            }
            abort.check();
            OpenExtractedFallback(streamError, abort);
        }

        void OpenExtractedFallback(const std::string& streamError, abort_callback& abort)
        {
            abort.check();
            char temporaryFolder[MAX_PATH * 4] = {};
            if (!ArchiveBuildPluginDataSubdirPath("Temp", temporaryFolder, sizeof(temporaryFolder)))
                throw exception_io_denied();

            wchar_t wideFolder[MAX_PATH * 4] = {};
            if (!MultiByteToWideChar(CP_ACP, 0, temporaryFolder, -1, wideFolder, _countof(wideFolder)))
                throw exception_io_denied();

            wchar_t temporaryName[MAX_PATH * 4] = {};
            if (!GetTempFileNameW(wideFolder, L"AST", 0, temporaryName))
                exception_io_from_win32(GetLastError());

            tempPath_ = temporaryName;
            try
            {
                std::string error;
                // Native UnRAR can decrypt RAR5 directly. Calling libarchive
                // first for an entry known encrypted is unnecessary, and can
                // destabilize repeated Unicode RAR entry opens in foobar.
                const bool nativeEncryptedRar = entry_.isEncrypted && DetectArchiveFormatFromPath(path_.c_str()) == ArchiveFormat::Rar;
                const bool extracted = nativeEncryptedRar
                    ? RarNativeExtractEncryptedEntryToFile(path_.c_str(), entry_.index, tempPath_.c_str(), error)
                    : ArchiveExtractEntryToFile(path_.c_str(), entry_.index, tempPath_.c_str(), error);
                if (!extracted)
                {
                    abort.check();
                    error = "Cannot stream: " + streamError + "; extraction fallback failed: " + error;
                    pfc::throw_exception_with_message<exception_io_data>(error.c_str());
                }

                abort.check();
                tempFile_ = CreateFileW(tempPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (tempFile_ == INVALID_HANDLE_VALUE)
                    exception_io_from_win32(GetLastError());

                LARGE_INTEGER actualSize{};
                if (!GetFileSizeEx(tempFile_, &actualSize))
                    exception_io_from_win32(GetLastError());

                if (actualSize.QuadPart < 0 || static_cast<uint64_t>(actualSize.QuadPart) != entry_.size)
                    throw exception_io_data_truncation();

                abort.check();
                DebugLogWrite("INFO", "FOOBAR", "FALLBACK_READY", "index=%u | size=%llu", (unsigned)entry_.index, (unsigned long long)entry_.size);
            }
            catch (...)
            {
                CloseExtractedFallback();
                throw;
            }
        }

        std::wstring path_;
        ArchiveEntry entry_;
        t_filetimestamp timestamp_ = filetimestamp_invalid;
        ArchiveStreamOwner stream_;
        std::vector<unsigned char> memoryData_;
        size_t memoryPosition_ = 0;
        HANDLE tempFile_ = INVALID_HANDLE_VALUE;
        std::wstring tempPath_;
    };

    template<ArchiveFormat Format>
    class ArchiveReader : public archive_impl
    {
    public:
        bool is_our_archive(const char* path) override
        {
            if (!path)
                return false;

            // Do not claim nested unpack:// paths. This implementation can
            // currently decode only local filesystem archive containers.
            if (std::strncmp(path, "unpack://", 9) == 0)
                return false;

            return DetectArchiveFormatFromPath(Utf8ToWide(path).c_str()) == Format;
        }

        void list_extensions(pfc::string_base& out) override
        {
            if constexpr (Format == ArchiveFormat::Zip)
                out = "zip,zip.001";
            else if constexpr (Format == ArchiveFormat::SevenZip)
                out = "7z,7z.001";
            else
                out = "rar";
        }

        void archive_list_v5(const char* path, file::ptr readerOptional, callback& output, uint32_t flags, abort_callback& abort) override
        {
            (void)readerOptional;
            const std::wstring native = LocalArchivePath(path, abort);
            if (DetectArchiveFormatFromPath(native.c_str()) != Format)
                throw exception_io_unsupported_format();

            std::vector<ArchiveEntry> entries;
            std::string error;
            if (!ArchiveListEntries(native.c_str(), entries, error))
                pfc::throw_exception_with_message<exception_io_data>(error.c_str());

            t_filestats wholeStats{};
            bool archiveWritable = false;
            filesystem::g_get_stats(path, wholeStats, archiveWritable, abort);
            for (const auto& entry : entries)
            {
                abort.check();
                if (entry.isFolder || !entry.isSupportedAudio)
                    continue;

                const std::string entryName = VirtualEntryPath(entry);
                pfc::string8 virtualUrl;
                make_unpack_path(virtualUrl, path, entryName.c_str());

                t_filestats2 stats = stats2_in_archive(t_filestats{ entry.size, wholeStats.m_timestamp }, t_filestats2::from_legacy(wholeStats));
                file::ptr reader;
                if (flags & archive_v5::flagReaders)
                    reader = fb2k::service_new<ArchiveEntryFile>(native, entry, wholeStats.m_timestamp, abort);

                if (!output.on_entry(virtualUrl.c_str(), stats, reader))
                    return;
            }
        }

    protected:
        const char* get_archive_type() override
        {
            if constexpr (Format == ArchiveFormat::Zip)
                return "astzip";
            else if constexpr (Format == ArchiveFormat::SevenZip)
                return "ast7z";
            else
                return "astrar";
        }

        t_filestats2 get_stats2_in_archive(const char* archivePath, const char* virtualPath, unsigned, abort_callback& abort) override
        {
            const auto native = LocalArchivePath(archivePath, abort);
            const ArchiveEntry entry = FindEntry(native.c_str(), virtualPath, abort);
            t_filestats base{};
            bool archiveWritable = false;
            filesystem::g_get_stats(archivePath, base, archiveWritable, abort);
            return stats2_in_archive(t_filestats{ entry.size, base.m_timestamp }, t_filestats2::from_legacy(base));
        }

        void open_archive(service_ptr_t<file>& output, const char* archivePath, const char* virtualPath, abort_callback& abort) override
        {
            const auto native = LocalArchivePath(archivePath, abort);
            const auto entry = FindEntry(native.c_str(), virtualPath, abort);
            t_filestats base{};
            bool archiveWritable = false;
            filesystem::g_get_stats(archivePath, base, archiveWritable, abort);
            abort.check();
            output = fb2k::service_new<ArchiveEntryFile>(native, entry, base.m_timestamp, abort);
        }
    };

    archive_factory_t<ArchiveReader<ArchiveFormat::Zip>> g_zipArchive;
    archive_factory_t<ArchiveReader<ArchiveFormat::SevenZip>> g_7zArchive;
    archive_factory_t<ArchiveReader<ArchiveFormat::Rar>> g_rarArchive;
}


bool FoobarBuildArchivePlaylistUrls(const char* archivePathUtf8, std::vector<std::string>& urls, std::string& error)
{
    urls.clear();
    error.clear();

    const std::wstring native = Utf8ToWide(archivePathUtf8);
    const ArchiveFormat format = DetectArchiveFormatFromPath(native.c_str());
    if (format == ArchiveFormat::Unknown)
    {
        error = "Unsupported archive type";
        return false;
    }

    std::vector<ArchiveEntry> entries;
    if (!ArchiveListEntries(native.c_str(), entries, error))
        return false;

    const char* type = nullptr;
    switch (format)
    {
    case ArchiveFormat::Zip:
        type = "astzip";
        break;
    case ArchiveFormat::SevenZip:
        type = "ast7z";
        break;
    case ArchiveFormat::Rar:
        type = "astrar";
        break;
    default:
        return false;
    }

    pfc::string8 archiveCanonical;
    try
    {
        filesystem::g_get_canonical_path(archivePathUtf8, archiveCanonical);
    }
    catch (const std::exception& ex)
    {
        error = ex.what();
        return false;
    }

    for (const auto& entry : entries)
    {
        if (entry.isFolder || !entry.isSupportedAudio)
            continue;

        const std::string entryPath = VirtualEntryPath(entry);
        pfc::string8 virtualUrl;
        archive_impl::g_make_unpack_path(virtualUrl, archiveCanonical.c_str(), entryPath.c_str(), type);
        urls.emplace_back(virtualUrl.c_str());
    }

    return true;
}
