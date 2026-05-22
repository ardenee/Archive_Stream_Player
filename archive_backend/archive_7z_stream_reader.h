#pragma once

#include "archive_types.h"

#include <cstdint>
#include <string>

bool SevenZipOpenNonSolidEntryStreamImpl( const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error);
size_t SevenZipReadNonSolidEntryStreamImpl(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead);
bool SevenZipSeekNonSolidEntryStreamImpl(ArchiveBackendStream& stream, uint64_t absolutePosition);
void SevenZipCloseNonSolidEntryStreamImpl(ArchiveBackendStream& stream);