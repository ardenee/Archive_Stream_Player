#include "archive_backend.h"
#include "archive_format.h"
#include "archive_7z_reader.h"
#include "archive_zip_reader.h"
#include "archive_zip_libarchive.h"
#include "archive_rar_reader.h"
#include "debug_log.h"

bool ArchiveListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "SCAN", "LIST_FAIL", "reason=empty_archive_path");
        return false;
    }

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);

    switch (format)
    {
    case ArchiveFormat::SevenZip:
        return SevenZipListEntries(archivePath, entries, error);

    case ArchiveFormat::Zip:
        return ZipListEntries(archivePath, entries, error);

    case ArchiveFormat::Rar:
        return RarListEntries(archivePath, entries, error);

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "SCAN", "LIST_FAIL", "format=%s | reason=unsupported_or_unknown_archive_format", ArchiveFormatToString(format));
        return false;
    }
}

bool ArchiveExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    data.clear();
    error.clear();

    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "index=%u | reason=empty_archive_path", (unsigned int)index);
        return false;
    }

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);

    switch (format)
    {
    case ArchiveFormat::SevenZip:
        return SevenZipExtractEntryToMemory(archivePath, index, data, error);

    case ArchiveFormat::Zip:
        return ZipExtractEntryToMemory(archivePath, index, data, error);

    case ArchiveFormat::Rar:
        return RarExtractEntryToMemory(archivePath, index, data, error);

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "index=%u | format=%s | reason=unsupported_or_unknown_archive_format", (unsigned int)index, ArchiveFormatToString(format));
        return false;
    }
}

bool ArchiveExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    error.clear();

    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "EXTRACT", "FILE_FAIL", "index=%u | reason=empty_archive_path", (unsigned int)index);
        return false;
    }

    if (!outputPath || !*outputPath)
    {
        error = "Output path is empty";
        DebugLogWrite("ERROR", "EXTRACT", "FILE_FAIL", "index=%u | reason=empty_output_path", (unsigned int)index);
        return false;
    }

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);

    switch (format)
    {
    case ArchiveFormat::SevenZip:
        return SevenZipExtractEntryToFile(archivePath, index, outputPath, error);

    case ArchiveFormat::Rar:
        return RarExtractEntryToFile(archivePath, index, outputPath, error);

    case ArchiveFormat::Zip:
        return LibarchiveZipExtractEntryToFile(archivePath, index, outputPath, error);

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "EXTRACT", "FILE_FAIL", "index=%u | format=%s | reason=unsupported_or_unknown_archive_format", (unsigned int)index, ArchiveFormatToString(format));
        return false;
    }
}

bool ArchiveOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error, const ArchiveStreamCancellationToken* cancellation)
{
    error.clear();

    // Do not overwrite a live opaque handle: doing so leaks the backend stream.
    if (stream.impl)
    {
        error = "Archive stream is already open; close it before reopening";
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | reason=stream_already_open", (unsigned int)index);
        return false;
    }

    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Unknown;
    stream.readStatus = ArchiveStreamReadStatus::NotOpen;
    stream.cancellation = nullptr;

    if (cancellation && cancellation->IsCancellationRequested())
    {
        error = "Archive stream open cancelled";
        stream.readStatus = ArchiveStreamReadStatus::Cancelled;
        DebugLogWrite("INFO", "STREAM", "OPEN_CANCELLED", "index=%u | reason=cancellation_requested", (unsigned int)index);
        return false;
    }

    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | reason=empty_archive_path", (unsigned int)index);
        return false;
    }

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);
    stream.format = format;

    switch (format)
    {
    case ArchiveFormat::SevenZip:
        if (!SevenZipOpenEntryStream(archivePath, index, stream, error))
            return false;

        stream.cancellation = cancellation;
        if (cancellation && cancellation->IsCancellationRequested())
        {
            ArchiveCloseEntryStream(stream);
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            error = "Archive stream open cancelled";
            return false;
        }

        stream.readStatus = ArchiveStreamReadStatus::Ready;
        return true;

    case ArchiveFormat::Rar:
        if (!RarOpenEntryStream(archivePath, index, stream, error, cancellation))
            return false;

        stream.cancellation = cancellation;
        if (cancellation && cancellation->IsCancellationRequested())
        {
            ArchiveCloseEntryStream(stream);
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            error = "Archive stream open cancelled";
            return false;
        }

        stream.readStatus = ArchiveStreamReadStatus::Ready;
        return true;

    case ArchiveFormat::Zip:
    {
        void* zipStream = nullptr;
        uint64_t entrySize = 0;

        if (!LibarchiveZipOpenEntryStream(archivePath, index, &zipStream, &entrySize, error, cancellation))
        {
            DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | format=ZIP | reason=libarchive_zip_open_failed | error=%s", (unsigned int)index, error.c_str());
            return false;
        }

        stream.impl = zipStream;
        stream.size = entrySize;
        stream.position = 0;
        stream.cancellation = cancellation;

        if (cancellation && cancellation->IsCancellationRequested())
        {
            ArchiveCloseEntryStream(stream);
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            error = "Archive stream open cancelled";
            return false;
        }

        stream.readStatus = ArchiveStreamReadStatus::Ready;
        return true;
    }

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | format=%s | reason=unsupported_or_unknown_archive_format", (unsigned int)index, ArchiveFormatToString(format));
        return false;
    }
}

ArchiveStreamReadResult ArchiveReadEntryStreamDetailed(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    ArchiveStreamReadResult result;
    result.format = stream.format;
    result.position = stream.position;

    auto finish = [&]() -> ArchiveStreamReadResult
    {
        result.status = stream.readStatus;
        result.position = stream.position;
        return result;
    };

    if (stream.cancellation && stream.cancellation->IsCancellationRequested())
    {
        stream.readStatus = ArchiveStreamReadStatus::Cancelled;
        result.errorKind = ArchiveStreamErrorKind::Cancelled;
        result.message = "Archive stream cancelled";
        return finish();
    }

    if (bytesToRead == 0)
        return finish();

    if (!stream.impl)
    {
        stream.readStatus = ArchiveStreamReadStatus::Error;
        result.errorKind = ArchiveStreamErrorKind::NotOpen;
        result.message = "Archive stream is not open";
        DebugLogWrite("WARN", "STREAM", "READ_FAIL", "format=%s | requested=%zu | reason=not_open", ArchiveFormatToString(stream.format), bytesToRead);
        return finish();
    }

    if (!buffer)
    {
        stream.readStatus = ArchiveStreamReadStatus::Error;
        result.errorKind = ArchiveStreamErrorKind::InvalidArgument;
        result.message = "Archive read destination is null";
        DebugLogWrite("WARN", "STREAM", "READ_FAIL", "format=%s | requested=%zu | reason=null_buffer", ArchiveFormatToString(stream.format), bytesToRead);
        return finish();
    }

    size_t bytesRead = 0;
    std::string backendError;

    switch (stream.format)
    {
    case ArchiveFormat::Zip:
        bytesRead = LibarchiveZipReadEntryStream(stream.impl, buffer, bytesToRead, backendError);
        stream.position += static_cast<uint64_t>(bytesRead);
        break;

    case ArchiveFormat::SevenZip:
        bytesRead = SevenZipReadEntryStream(stream, buffer, bytesToRead);
        break;

    case ArchiveFormat::Rar:
        bytesRead = RarReadEntryStream(stream, buffer, bytesToRead, &backendError);
        break;

    default:
        stream.readStatus = ArchiveStreamReadStatus::Error;
        result.errorKind = ArchiveStreamErrorKind::UnsupportedFormat;
        result.message = "Unsupported archive stream format";
        DebugLogWrite("WARN", "STREAM", "READ_FAIL", "format=%s | requested=%zu | reason=unsupported_stream_format", ArchiveFormatToString(stream.format), bytesToRead);
        return finish();
    }

    if (stream.cancellation && stream.cancellation->IsCancellationRequested())
    {
        stream.readStatus = ArchiveStreamReadStatus::Cancelled;
        result.errorKind = ArchiveStreamErrorKind::Cancelled;
        result.message = "Archive stream cancelled";
        return finish();
    }

    result.bytesRead = bytesRead;

    if (!backendError.empty())
    {
        stream.readStatus = ArchiveStreamReadStatus::Error;
        result.errorKind = ArchiveStreamErrorKind::BackendFailure;
        result.message = std::move(backendError);
        DebugLogWrite("ERROR", "STREAM", "READ_FAIL", "format=%s | position=%llu | requested=%zu | error=%s", ArchiveFormatToString(stream.format), (unsigned long long)stream.position, bytesToRead, result.message.c_str());
    }
    else if (bytesRead > 0)
    {
        stream.readStatus = ArchiveStreamReadStatus::Ready;
    }
    else if (stream.position >= stream.size)
    {
        stream.readStatus = ArchiveStreamReadStatus::EndOfEntry;
    }
    else
    {
        stream.readStatus = ArchiveStreamReadStatus::Error;
        result.errorKind = ArchiveStreamErrorKind::TruncatedEntry;
        result.message = "Unexpected end of archive entry before the advertised size";
        DebugLogWrite("ERROR", "STREAM", "READ_FAIL", "format=%s | position=%llu | expected=%llu | reason=premature_end_of_entry", ArchiveFormatToString(stream.format), (unsigned long long)stream.position, (unsigned long long)stream.size);
    }

    return finish();
}

size_t ArchiveReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    return ArchiveReadEntryStreamDetailed(stream, buffer, bytesToRead).bytesRead;
}

bool ArchiveSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    if (stream.cancellation && stream.cancellation->IsCancellationRequested())
    {
        stream.readStatus = ArchiveStreamReadStatus::Cancelled;
        return false;
    }

    if (!stream.impl)
    {
        DebugLogWrite("ERROR", "STREAM", "SEEK_FAIL", "target=%llu | reason=no_backend_impl", (unsigned long long)absolutePosition);
        return false;
    }

    switch (stream.format)
    {
    case ArchiveFormat::Zip:
    {
        if (absolutePosition > stream.size)
        {
            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=ZIP | target=%llu | entry_size=%llu | reason=beyond_entry", (unsigned long long)absolutePosition, (unsigned long long)stream.size);
            return false;
        }

        std::string error;
        if (!LibarchiveZipSeekEntryStream(stream.impl, absolutePosition, error))
        {
            if (stream.cancellation && stream.cancellation->IsCancellationRequested())
                stream.readStatus = ArchiveStreamReadStatus::Cancelled;

            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=ZIP | target=%llu | error=%s", (unsigned long long)absolutePosition, error.c_str());
            return false;
        }

        if (stream.cancellation && stream.cancellation->IsCancellationRequested())
        {
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            return false;
        }

        stream.position = absolutePosition;
        stream.readStatus = ArchiveStreamReadStatus::Ready;
        return true;
    }

    case ArchiveFormat::SevenZip:
    {
        bool ok = SevenZipSeekEntryStream(stream, absolutePosition);

        if (!ok)
        {
            if (stream.cancellation && stream.cancellation->IsCancellationRequested())
                stream.readStatus = ArchiveStreamReadStatus::Cancelled;

            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=7Z | target=%llu", (unsigned long long)absolutePosition);
        }
        else if (stream.cancellation && stream.cancellation->IsCancellationRequested())
        {
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            return false;
        }
        else
        {
            stream.readStatus = ArchiveStreamReadStatus::Ready;
        }

        return ok;
    }

    case ArchiveFormat::Rar:
    {
        bool ok = RarSeekEntryStream(stream, absolutePosition);

        if (!ok)
        {
            if (stream.cancellation && stream.cancellation->IsCancellationRequested())
                stream.readStatus = ArchiveStreamReadStatus::Cancelled;

            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=RAR | target=%llu", (unsigned long long)absolutePosition);
        }
        else if (stream.cancellation && stream.cancellation->IsCancellationRequested())
        {
            stream.readStatus = ArchiveStreamReadStatus::Cancelled;
            return false;
        }
        else
        {
            stream.readStatus = ArchiveStreamReadStatus::Ready;
        }

        return ok;
    }

    default:
        DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=UNKNOWN | target=%llu", (unsigned long long)absolutePosition);
        return false;
    }
}

void ArchiveCloseEntryStream(ArchiveBackendStream& stream)
{
    if (!stream.impl)
    {
        stream.size = 0;
        stream.position = 0;
        stream.format = ArchiveFormat::Unknown;
        stream.readStatus = ArchiveStreamReadStatus::NotOpen;
        stream.cancellation = nullptr;
        return;
    }

    switch (stream.format)
    {
    case ArchiveFormat::Zip:
        LibarchiveZipCloseEntryStream(stream.impl);
        break;

    case ArchiveFormat::SevenZip:
        SevenZipCloseEntryStream(stream);
        break;

    case ArchiveFormat::Rar:
        RarCloseEntryStream(stream);
        break;

    default:
        break;
    }

    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Unknown;
    stream.readStatus = ArchiveStreamReadStatus::NotOpen;
    stream.cancellation = nullptr;
}