#pragma once
#include "archive_types.h"

#include <cstdint>
#include <string>
#include <vector>

bool RarListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
bool RarExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
bool RarExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);
void RarSetPasswordListPath(const char* passwordListPath);
bool RarOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error, const ArchiveStreamCancellationToken* cancellation = nullptr);
size_t RarReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead, std::string* outError = nullptr);
bool RarSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition);
void RarCloseEntryStream(ArchiveBackendStream& stream);