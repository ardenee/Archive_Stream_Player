#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "archive_types.h"

// Narrow fallback for encrypted RAR entries which libarchive cannot decode.
// Uses the configured backend password list and the already-linked UnRAR SDK.
// Only the selected entry is written, to a caller-owned destination path.
bool RarNativeExtractEncryptedEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);

// Fallback listing when RAR5 filename encryption makes libarchive's listing empty.
// RAR CRC metadata is copied only from native archive headers, never recalculated.
bool RarNativeEnrichMultipartEntryEncryption(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
// Header-only lookup for foobar virtual files. Matches their entry index and
// filename without repeatedly invoking libarchive after decoder lifecycles.
bool RarNativeLookupEntryMetadata(const wchar_t* archivePath, uint32_t index, ArchiveEntry& entry, std::string& error);
bool RarNativeHasEncryptedHeaders(const wchar_t* archivePath);
bool RarNativeListHeaderEncryptedEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
