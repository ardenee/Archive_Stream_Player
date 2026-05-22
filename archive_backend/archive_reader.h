#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>

#include "archive_types.h"
#include "archive_common.h"
#include "archive_metadata.h"

bool ArchiveFindFirstAudioEntry(const char* archivePath, mz_uint* outIndex, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize);

bool ArchiveGetEntryInfoEx(const char* archivePath, uint32_t index, char* outName, size_t outNameSize, uint64_t* outUncompressedSize, uint32_t* outCrc32, bool* outHasCrc32);
bool ArchiveGetEntrySolidInfo(const char* archivePath, mz_uint index, bool* outIsSolid, mz_uint32* outFolderIndex, mz_uint32* outFilesInFolder);
bool CharPathToWidePath(const char* inPath, wchar_t* outPath, size_t outPathSize);
bool WidePathToCharPath(const wchar_t* inPath, char* outPath, size_t outPathSize);
bool IsSupportedArchivePath(const char* path);
bool ArchiveListEntries(const char* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
bool ArchiveListAudioEntries(const char* archivePath, std::vector<ArchiveAudioEntry>& entries);
bool ArchiveGetEntryInfo(const char* archivePath, uint32_t index, char* outName, size_t outNameSize, uint64_t* outUncompressedSize);
bool ArchiveGetEntryInfoByPath(const char* archivePath, const char* internalPath, char* outName, size_t outNameSize, uint64_t* outUncompressedSize, uint32_t* outIndex);
bool ArchiveExtractEntryToMemory(const char* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
void ArchiveSetRarPasswordListPath(const char* passwordListPath);
ArchiveStreamCapability ArchiveGetEntryStreamCapability(const char* archivePath, mz_uint index);
bool ArchiveOpenEntryStream(ArchiveStreamContext* ctx, const char* archivePath, mz_uint index);
size_t ArchiveReadEntryStream(ArchiveStreamContext* ctx, void* bufferOut, size_t bytesToRead);
bool ArchiveOpenValidatedEntryStream(ArchiveStreamContext* ctx, const char* archivePath, mz_uint index, size_t probeBytes, std::string* outError);
bool ArchiveValidateEntryStreamRead(const char* archivePath, mz_uint index, size_t probeBytes, std::string* outError);
bool ArchiveSeekEntryStream(ArchiveStreamContext* ctx, mz_uint64 absolutePosition);
void ArchiveCloseEntryStream(ArchiveStreamContext* ctx);
bool IsSevenZipArchivePath(const char* path);
bool IsRarArchivePath(const char* path);
bool IsZipArchivePath(const char* path);