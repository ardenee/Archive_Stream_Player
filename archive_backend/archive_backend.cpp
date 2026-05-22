#include "archive_backend.h"
#include "archive_format.h"
#include "archive_7z_reader.h"
#include "archive_zip_reader.h"
#include "archive_rar_reader.h"
#include "debug_log.h"

bool ArchiveListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

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
        /*
            ZIP large-entry playback already uses the existing streaming path.
            This generic file extraction path is currently only needed for
            non-streaming cache fallback archive types.
        */
        error = "ZIP extract-to-file is not used by this backend path";
        DebugLogWrite("WARN", "EXTRACT", "FILE_FAIL", "index=%u | format=ZIP | reason=zip_extract_to_file_not_used_by_backend_path", (unsigned int)index);
        return false;

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "EXTRACT", "FILE_FAIL", "index=%u | format=%s | reason=unsupported_or_unknown_archive_format", (unsigned int)index, ArchiveFormatToString(format));
        return false;
    }
}

bool ArchiveOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error)
{
    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Unknown;
    error.clear();

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);
    stream.format = format;

    switch (format)
    {
    case ArchiveFormat::SevenZip:
        return SevenZipOpenEntryStream(archivePath, index, stream, error);

    case ArchiveFormat::Rar:
        return RarOpenEntryStream(archivePath, index, stream, error);

    case ArchiveFormat::Zip:
        error = "ZIP streaming is handled by the Winamp input plugin ZIP path";
        DebugLogWrite("WARN", "STREAM", "OPEN_FAIL", "index=%u | format=ZIP | reason=zip_streaming_handled_by_input_plugin", (unsigned int)index);
        return false;

    case ArchiveFormat::Unknown:
    default:
        error = "Unsupported or unknown archive format";
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | format=%s | reason=unsupported_or_unknown_archive_format", (unsigned int)index, ArchiveFormatToString(format));
        return false;
    }
}

size_t ArchiveReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    if (!stream.impl || !buffer || bytesToRead == 0)
    {
        if (bytesToRead > 0)
            DebugLogWrite("WARN", "STREAM", "READ_FAIL", "format=%s | requested=%zu | reason=invalid_input", ArchiveFormatToString(stream.format), bytesToRead);

        return 0;
    }

    switch (stream.format)
    {
    case ArchiveFormat::SevenZip:
        return SevenZipReadEntryStream(stream, buffer, bytesToRead);

    case ArchiveFormat::Rar:
        return RarReadEntryStream(stream, buffer, bytesToRead);

    default:
        DebugLogWrite("WARN", "STREAM", "READ_FAIL", "format=%s | requested=%zu | reason=unsupported_stream_format", ArchiveFormatToString(stream.format), bytesToRead);
        return 0;
    }
}

bool ArchiveSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    if (!stream.impl)
    {
        DebugLogWrite("ERROR", "STREAM", "SEEK_FAIL", "target=%llu | reason=no_backend_impl", (unsigned long long)absolutePosition);
        return false;
    }

    switch (stream.format)
    {
    case ArchiveFormat::SevenZip:
    {
        bool ok = SevenZipSeekEntryStream(stream, absolutePosition);

        if (!ok)
            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=7Z | target=%llu", (unsigned long long)absolutePosition);

        return ok;
    }

    case ArchiveFormat::Rar:
    {
        bool ok = RarSeekEntryStream(stream, absolutePosition);

        if (!ok)
            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "format=RAR | target=%llu", (unsigned long long)absolutePosition);

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
        return;

    switch (stream.format)
    {
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
}