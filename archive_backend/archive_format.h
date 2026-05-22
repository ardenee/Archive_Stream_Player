#pragma once
#include "archive_types.h"

ArchiveFormat DetectArchiveFormatFromPath(const wchar_t* archivePath);
const char* ArchiveFormatToString(ArchiveFormat format);