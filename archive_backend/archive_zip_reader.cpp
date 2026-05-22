#define NOMINMAX

#include "archive_zip_reader.h"
#include "archive_zip_libarchive.h"
#include "debug_log.h"

// -----------------------------------------------------------------------------
// ZipListEntries
//
// Lists ZIP entries through the libarchive ZIP backend.
//
// ZIP handling is now fully libarchive-backed. The old miniz reader, split-ZIP
// callback reader, and miniz extraction paths have been removed from active
// source. Normal ZIP and split ZIP both use archive_zip_libarchive.cpp.
// -----------------------------------------------------------------------------
bool ZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    return LibarchiveZipListEntries(archivePath, entries, error);
}

// -----------------------------------------------------------------------------
// ZipExtractEntryToMemory
//
// Extracts one ZIP entry into memory through the libarchive ZIP backend.
//
// This is used by small BUFFER routes and by the current ZIP cache path when a
// ZIP entry must be materialized before being written to managed cache.
// -----------------------------------------------------------------------------
bool ZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    return LibarchiveZipExtractEntryToMemory(archivePath, index, data, error);
}