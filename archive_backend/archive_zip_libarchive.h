#pragma once

#include <string>
#include <vector>

#include "archive_backend.h"

// -----------------------------------------------------------------------------
// LibarchiveZipListEntries
//
// Lists ZIP entries through libarchive instead of miniz.
//
// Supports normal ZIP and first-volume split ZIP archives by presenting
// .zip.001/.zip.002/etc. as one logical seekable input stream through
// libarchive callbacks.
// -----------------------------------------------------------------------------
bool LibarchiveZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);

// -----------------------------------------------------------------------------
// LibarchiveZipExtractEntryToMemory
//
// Extracts one ZIP entry into memory through libarchive.
//
// This is LA-ZIP-2. It keeps the outer backend API unchanged while moving ZIP
// extraction away from miniz. Normal ZIP and split ZIP both use the same
// libarchive callback source.
// -----------------------------------------------------------------------------
bool LibarchiveZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);

// -----------------------------------------------------------------------------
// LibarchiveZipOpenEntryStream
//
// Opens one ZIP entry for decoded byte streaming through libarchive.
//
// Normal ZIP and split ZIP use the same callback-backed archive source. The
// returned opaque stream handle owns the libarchive reader and source handles,
// and must be closed with LibarchiveZipCloseEntryStream().
// -----------------------------------------------------------------------------
bool LibarchiveZipOpenEntryStream(const wchar_t* archivePath, uint32_t index, void** outStream, uint64_t* outEntrySize, std::string& error);

// -----------------------------------------------------------------------------
// LibarchiveZipReadEntryStream
//
// Reads decoded bytes from an open libarchive ZIP entry stream.
// -----------------------------------------------------------------------------
size_t LibarchiveZipReadEntryStream(void* stream, void* buffer, size_t bytesToRead, std::string& error);

// -----------------------------------------------------------------------------
// LibarchiveZipSeekEntryStream
//
// Seeks inside a libarchive ZIP entry stream.
//
// libarchive streams decoded entry data forward. Forward seeks discard bytes.
// Backward seeks reopen the archive and discard forward to the target position.
// -----------------------------------------------------------------------------
bool LibarchiveZipSeekEntryStream(void* stream, uint64_t absolutePosition, std::string& error);

// -----------------------------------------------------------------------------
// LibarchiveZipCloseEntryStream
//
// Closes an opaque libarchive ZIP entry stream.
// -----------------------------------------------------------------------------
void LibarchiveZipCloseEntryStream(void* stream);

// -----------------------------------------------------------------------------
// LibarchiveZipExtractEntryToFile
//
// Extracts one ZIP entry directly to a file through libarchive.
//
// This is used by cache-building paths so large ZIP entries can be written
// without first holding the whole extracted file in memory.
// -----------------------------------------------------------------------------
bool LibarchiveZipExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);

// -----------------------------------------------------------------------------
