// Player-neutral decoded archive streams presented as foobar2000 virtual files.
// The player's existing audio input decoders handle MP3/FLAC/OGG/WAV/etc.
// No audio payloads are extracted to temporary files or CRCs recalculated.
#include <foobar2000/SDK/foobar2000.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "archive_format.h"
#include "archive_stream_owner.h"
#include "foobar_archive_reader.h"
#include "debug_log.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <cstring>
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
        ArchiveEntryFile(const std::wstring& path, const ArchiveEntry& entry, t_filetimestamp timestamp)
            : path_(path), entry_(entry), timestamp_(timestamp)
        {
            const auto result = stream_.OpenDetailed(path_.c_str(), entry_.index);
            if (!result.success)
            {
                // Some valid 7z/solid coder layouts cannot be opened by the
                // incremental decoder. A uniquely named foobar-owned temp
                // file is a deliberate fallback; no content CRC is computed.
                DebugLogWrite("INFO", "FOOBAR", "STREAM_FALLBACK", "index=%u | reason=%s", (unsigned)entry_.index, result.message.c_str());
                OpenExtractedFallback(result.message);
            }
        }

        ~ArchiveEntryFile()
        {
            CloseExtractedFallback();
        }

        t_size read(void* destination, t_size amount, abort_callback& abort) override
        {
            abort.check();
            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                DWORD done = 0;
                const DWORD limit = static_cast<DWORD>((std::min)(amount, static_cast<t_size>((std::numeric_limits<DWORD>::max)())));
                if (!ReadFile(tempFile_, destination, limit, &done, nullptr))
                    exception_io_from_win32(GetLastError());

                abort.check();
                return done;
            }

            const auto result = stream_.ReadDetailed(destination, amount);
            if (result.errorKind != ArchiveStreamErrorKind::None)
                pfc::throw_exception_with_message<exception_io_data>(result.message.c_str());

            abort.check();
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
            if (tempFile_ != INVALID_HANDLE_VALUE)
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

            if (tempFile_ != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER target{};
                target.QuadPart = static_cast<LONGLONG>(position);
                if (!SetFilePointerEx(tempFile_, target, nullptr, FILE_BEGIN))
                    exception_io_from_win32(GetLastError());

                abort.check();
                return;
            }

            const auto result = stream_.SeekDetailed(position);
            if (!result.success)
                pfc::throw_exception_with_message<exception_io_data>(result.message.c_str());

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

        void OpenExtractedFallback(const std::string& streamError)
        {
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
                if (!ArchiveExtractEntryToFile(path_.c_str(), entry_.index, tempPath_.c_str(), error))
                {
                    error = "Cannot stream: " + streamError + "; extraction fallback failed: " + error;
                    pfc::throw_exception_with_message<exception_io_data>(error.c_str());
                }

                tempFile_ = CreateFileW(tempPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (tempFile_ == INVALID_HANDLE_VALUE)
                    exception_io_from_win32(GetLastError());

                LARGE_INTEGER actualSize{};
                if (!GetFileSizeEx(tempFile_, &actualSize))
                    exception_io_from_win32(GetLastError());

                if (actualSize.QuadPart < 0 || static_cast<uint64_t>(actualSize.QuadPart) != entry_.size)
                    throw exception_io_data_truncation();

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
                    reader = fb2k::service_new<ArchiveEntryFile>(native, entry, wholeStats.m_timestamp);

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
            output = fb2k::service_new<ArchiveEntryFile>(native, entry, base.m_timestamp);
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
