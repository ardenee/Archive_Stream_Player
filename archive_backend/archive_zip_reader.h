#pragma once
#include "archive_types.h"

#include <cstdint>
#include <string>
#include <vector>

bool ZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
bool ZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);