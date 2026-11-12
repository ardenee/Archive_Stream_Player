#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "archive_types.h"

bool ArchiveListEntries(const wchar_t* archivePath,std::vector<ArchiveEntry>& entries,std::string& error);
bool ArchiveExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
bool ArchiveExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);
// ZIP, 7z and RAR entry streams share the same opaque open/read/seek/close API.
// ZIP seeks are decoded-byte seeks (backwards seeks reopen and re-decompress).
// The caller must close each successfully opened stream exactly once; close is
// idempotent. Consult stream.readStatus after zero-byte reads: EndOfEntry
// means normal EOF; Error means an invalid request, a backend-reported read
// failure, or premature termination before the expected entry size.
bool ArchiveOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error);
size_t ArchiveReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead);
bool ArchiveSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition);
void ArchiveCloseEntryStream(ArchiveBackendStream& stream);
bool ArchiveEntryCanStream(const char* archivePath, unsigned int index);

