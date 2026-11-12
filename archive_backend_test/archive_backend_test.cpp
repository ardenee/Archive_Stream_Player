#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

#include "archive_backend.h"
#include "archive_format.h"
#include "archive_common.h"

static bool ParseUInt32Arg(const wchar_t* text, uint32_t& outValue)
{
    if (!text || !*text)
        return false;

    wchar_t* end = nullptr;
    unsigned long value = std::wcstoul(text, &end, 10);

    if (end == text || *end != L'\0')
        return false;

    if (value > 0xFFFFFFFFUL)
        return false;

    outValue = static_cast<uint32_t>(value);
    return true;
}

static void PrintFirstBytes(const std::vector<unsigned char>& data, size_t maxBytes = 16)
{
    size_t count = (std::min)(maxBytes, data.size());

    std::printf("First %zu byte(s):", count);

    for (size_t i = 0; i < count; ++i)
        std::printf(" %02X", static_cast<unsigned>(data[i]));

    std::printf("\n");
}

static bool StreamEntryTest(const wchar_t* archivePath, uint32_t selectedIndex, const ArchiveEntry& selectedEntry, std::string& error)
{
    ArchiveBackendStream stream;

    if (!ArchiveOpenEntryStream(archivePath, selectedIndex, stream, error))
        return false;

    std::printf("Stream opened successfully.\n");
    std::printf("Stream size: %llu byte(s)\n", static_cast<unsigned long long>(stream.size));

    unsigned char buffer[64 * 1024];
    std::vector<unsigned char> firstBytes;

    unsigned long long totalRead = 0;

    while (true)
    {
        size_t got = ArchiveReadEntryStream(stream, buffer, sizeof(buffer));

        if (got == 0)
        {
            if (stream.readStatus != ArchiveStreamReadStatus::EndOfEntry)
            {
                error = "Backend reported an error or unexpected status at end of stream";
                ArchiveCloseEntryStream(stream);
                return false;
            }

            break;
        }

        if (firstBytes.size() < 16)
        {
            size_t needed = 16 - firstBytes.size();
            size_t copyNow = got < needed ? got : needed;
            firstBytes.insert(firstBytes.end(), buffer, buffer + copyNow);
        }

        totalRead += got;
    }

    std::printf("Stream read size: %llu byte(s)\n", totalRead);

    if (totalRead != selectedEntry.size)
    {
        std::printf(
            "ERROR: streamed byte count does not match listed entry size.\n"
            "Listed:   %llu\n"
            "Streamed: %llu\n",
            static_cast<unsigned long long>(selectedEntry.size),
            totalRead
        );
        error = "Streamed byte count differs from the expected entry size";
        ArchiveCloseEntryStream(stream);
        return false;
    }

    if (!firstBytes.empty())
        PrintFirstBytes(firstBytes);

    if (selectedEntry.size > 32)
    {
        if (ArchiveSeekEntryStream(stream, 16))
        {
            unsigned char seekBuffer[16];
            size_t got = ArchiveReadEntryStream(stream, seekBuffer, sizeof(seekBuffer));

            std::printf("Seek test: seek to 16, read %zu byte(s):", got);

            for (size_t i = 0; i < got; ++i)
                std::printf(" %02X", static_cast<unsigned>(seekBuffer[i]));

            std::printf("\n");
        }
        else
        {
            std::printf("Seek test failed.\n");
        }
    }

    ArchiveCloseEntryStream(stream);

    std::printf("\nStream test succeeded.\n");
    return true;
}

// Check generic ZIP decoded bytes against independent memory extraction.
// This test is deliberately fixture-sized; it never computes content CRCs.
static bool VerifyGenericZipStream(const wchar_t* archivePath, const ArchiveEntry& entry, std::string& error)
{
    error.clear();

    if (entry.isFolder)
        return true;

    if (entry.size > 1024 * 1024)
    {
        error = "ZIP regression fixture exceeds the 1 MiB comparison limit";
        return false;
    }

    std::vector<unsigned char> reference;

    // The memory-extraction API currently rejects empty files; an empty
    // entry is still a valid stream and must immediately report normal EOF.
    if (entry.size > 0 && !ArchiveExtractEntryToMemory(archivePath, entry.index, reference, error))
        return false;

    if (reference.size() != entry.size)
    {
        error = "ZIP reference extraction size differs from archive metadata";
        return false;
    }

    ArchiveBackendStream stream;
    if (!ArchiveOpenEntryStream(archivePath, entry.index, stream, error))
        return false;

    auto fail = [&](const char* reason) -> bool
    {
        error = reason;
        ArchiveCloseEntryStream(stream);
        return false;
    };

    if (!stream.impl || stream.format != ArchiveFormat::Zip || stream.size != entry.size || stream.position != 0 || stream.readStatus != ArchiveStreamReadStatus::Ready)
        return fail("Generic ZIP open returned invalid stream state");

    void* originalHandle = stream.impl;
    std::string reopenError;
    if (ArchiveOpenEntryStream(archivePath, entry.index, stream, reopenError) || stream.impl != originalHandle || reopenError.empty())
        return fail("Generic ZIP permitted reopening a live stream");

    std::vector<unsigned char> actual;
    unsigned char block[4096];
    for (;;)
    {
        size_t got = ArchiveReadEntryStream(stream, block, sizeof(block));
        if (got == 0)
            break;

        if (got > reference.size() - actual.size())
            return fail("Generic ZIP decoded more data than listed");

        actual.insert(actual.end(), block, block + got);
    }

    if (actual != reference || stream.position != entry.size || stream.readStatus != ArchiveStreamReadStatus::EndOfEntry)
        return fail("Generic ZIP sequential decoding differs from reference bytes or did not end normally");

    if (ArchiveReadEntryStream(stream, block, sizeof(block)) != 0 || stream.readStatus != ArchiveStreamReadStatus::EndOfEntry)
        return fail("Generic ZIP read beyond EOF returned data or incorrect status");

    if (ArchiveSeekEntryStream(stream, entry.size + 1) || stream.position != entry.size)
        return fail("Generic ZIP seek beyond EOF succeeded or changed position");

    if (!ArchiveSeekEntryStream(stream, 0) || stream.position != 0 || stream.readStatus != ArchiveStreamReadStatus::Ready)
        return fail("Generic ZIP rewind from EOF failed");

    size_t prefixCount = reference.size() < 16 ? reference.size() : 16;
    size_t gotPrefix = ArchiveReadEntryStream(stream, block, prefixCount);
    if (gotPrefix != prefixCount || (prefixCount && std::memcmp(block, reference.data(), prefixCount) != 0))
        return fail("Generic ZIP rewind read differs from reference bytes");

    if (reference.size() > 64)
    {
        if (!ArchiveSeekEntryStream(stream, 32) || stream.position != 32)
            return fail("Generic ZIP forward seek to byte 32 failed");

        size_t got = ArchiveReadEntryStream(stream, block, 32);
        if (got != 32 || std::memcmp(block, reference.data() + 32, 32) != 0)
            return fail("Generic ZIP forward-seek read differs from reference bytes");

        if (!ArchiveSeekEntryStream(stream, 16) || stream.position != 16)
            return fail("Generic ZIP backward seek to byte 16 failed");

        got = ArchiveReadEntryStream(stream, block, 16);
        if (got != 16 || std::memcmp(block, reference.data() + 16, 16) != 0)
            return fail("Generic ZIP backward-seek read differs from reference bytes");
    }

    ArchiveCloseEntryStream(stream);
    ArchiveCloseEntryStream(stream);

    if (stream.impl || stream.size != 0 || stream.position != 0 || stream.format != ArchiveFormat::Unknown || stream.readStatus != ArchiveStreamReadStatus::NotOpen)
    {
        error = "Generic ZIP close did not reset stream state";
        return false;
    }

    if (!ArchiveOpenEntryStream(archivePath, entry.index, stream, error))
        return false;

    ArchiveCloseEntryStream(stream);
    std::printf("PASS: generic ZIP stream index %u (%s), %llu bytes\n", (unsigned int)entry.index, entry.path.c_str(), (unsigned long long)entry.size);
    return true;
}

static int RunGenericZipStreamRegression(const wchar_t* archivePath)
{
    if (DetectArchiveFormatFromPath(archivePath) != ArchiveFormat::Zip)
    {
        std::printf("FAIL: ziptest requires a ZIP or ZIP.001 archive\n");
        return 1;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("FAIL: ZIP listing: %s\n", error.c_str());
        return 1;
    }

    unsigned tested = 0;
    for (const ArchiveEntry& entry : entries)
    {
        if (entry.isFolder)
            continue;

        if (!VerifyGenericZipStream(archivePath, entry, error))
        {
            std::printf("FAIL: generic ZIP stream index %u (%s): %s\n", (unsigned int)entry.index, entry.path.c_str(), error.c_str());
            return 1;
        }

        ++tested;
    }

    if (tested == 0)
    {
        std::printf("FAIL: no ZIP files were tested\n");
        return 1;
    }

    ArchiveBackendStream missing;
    if (ArchiveOpenEntryStream(archivePath, UINT32_MAX, missing, error) || error.empty() || missing.impl)
    {
        ArchiveCloseEntryStream(missing);
        std::printf("FAIL: generic ZIP did not reject an invalid entry index\n");
        return 1;
    }

    std::printf("PASS: ZIP stream regression %u entries; invalid index rejected\n", tested);
    return 0;
}

static int RunApiValidationTests()
{
    int failures = 0;

    auto check = [&failures](bool passed, const char* testName)
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", testName);
        if (!passed)
            ++failures;
    };

    std::vector<ArchiveEntry> entries;
    std::vector<unsigned char> data;
    std::string error;

    check(!ArchiveListEntries(nullptr, entries, error) && !error.empty(), "list null path");
    check(!ArchiveListEntries(L"", entries, error) && !error.empty(), "list empty path");
    check(!ArchiveExtractEntryToMemory(nullptr, 0, data, error) && !error.empty(), "extract memory null path");
    check(!ArchiveExtractEntryToFile(nullptr, 0, L"unused.bin", error) && !error.empty(), "extract file null path");
    check(!ArchiveExtractEntryToFile(L"unused.7z", 0, nullptr, error) && !error.empty(), "extract file null output");

    ArchiveBackendStream stream;
    check(!ArchiveOpenEntryStream(nullptr, 0, stream, error) && !error.empty() && !stream.impl, "stream null path");
    unsigned char byte = 0;
    check(ArchiveReadEntryStream(stream, &byte, 1) == 0 && stream.readStatus == ArchiveStreamReadStatus::Error, "read on closed stream sets error status");
    ArchiveCloseEntryStream(stream);
    check(stream.readStatus == ArchiveStreamReadStatus::NotOpen, "closing inactive stream resets status");

    stream.impl = &stream; // Simulate a live opaque handle without opening an archive.
    check(!ArchiveOpenEntryStream(L"unused.7z", 0, stream, error) && !error.empty() && stream.impl == &stream, "reject reopening live stream");
    stream.impl = nullptr;

    // Folder name is controlled by the adapter, never constructed from archive data.
    check(ArchiveSetDataDirectoryName(nullptr), "generic directory default");
    check(!ArchiveSetDataDirectoryName(""), "reject empty directory name");
    check(!ArchiveSetDataDirectoryName("../bad"), "reject directory traversal");
    check(!ArchiveSetDataDirectoryName("bad\\path"), "reject nested directory");
    check(!ArchiveSetDataDirectoryName("folder:name"), "reject invalid directory character");
    check(!ArchiveSetDataDirectoryName("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"), "reject overlong directory name");

    char path[MAX_PATH * 4] = {};
    check(ArchiveGetPluginDataRoot(path, sizeof(path)) && std::string(path).find("\\ArchiveStreamPlayer") != std::string::npos, "generic data root");
    check(ArchiveSetDataDirectoryName("WinampArchiveStream"), "select legacy Winamp data folder");
    check(ArchiveGetPluginDataRoot(path, sizeof(path)) && std::string(path).find("\\WinampArchiveStream") != std::string::npos, "preserve Winamp data root");
    check(ArchiveBuildPluginDataFilePath("in_archive_stream.ini", path, sizeof(path)) && std::string(path).find("\\WinampArchiveStream\\in_archive_stream.ini") != std::string::npos, "preserve Winamp config path");

    char shortPath[8] = {};
    check(!ArchiveGetPluginDataRoot(shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject truncated data root");
    check(!ArchiveBuildPluginDataFilePath("in_archive_stream.ini", shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject truncated data file path");
    check(!ArchiveBuildPluginDataSubdirPath("Cache", shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject truncated data subdirectory path");
    strcpy_s(shortPath, sizeof(shortPath), "stale");
    check(!ArchiveBuildPluginDataFilePath("../escape", shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject escaping data filename and clear output");
    strcpy_s(shortPath, sizeof(shortPath), "stale");
    check(!ArchiveBuildPluginDataSubdirPath("..", shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject parent data directory and clear output");
    strcpy_s(shortPath, sizeof(shortPath), "stale");
    check(!ArchiveBuildPluginDataFilePath(nullptr, shortPath, sizeof(shortPath)) && shortPath[0] == '\0', "reject null data filename and clear output");
    check(ArchiveSetDataDirectoryName(nullptr), "reset directory to generic default");

    std::printf("\nAPI validation: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc >= 2 && _wcsicmp(argv[1], L"selftest") == 0)
        return RunApiValidationTests();

    if (argc >= 3 && _wcsicmp(argv[1], L"ziptest") == 0)
        return RunGenericZipStreamRegression(argv[2]);

    if (argc < 2)
    {
        std::printf("Usage:\n");
        std::printf("  archive_backend_test.exe selftest\n");
        std::printf("  archive_backend_test.exe ziptest archive.zip\n");
        std::printf("  archive_backend_test.exe archive.7z\n");
        std::printf("  archive_backend_test.exe archive.7z index\n");
        std::printf("  archive_backend_test.exe archive.7z index stream\n");
        std::printf("  archive_backend_test.exe archive.rar index file output_path\n");
        std::printf("\n");
        std::printf("Examples:\n");
        std::printf("  archive_backend_test.exe Main.7z\n");
        std::printf("  archive_backend_test.exe Main.7z 1\n");
        std::printf("  archive_backend_test.exe Main.7z 0 stream\n");
        std::printf("  archive_backend_test.exe Main.rar 0 file out.ext\n");
        return 1;
    }

    const wchar_t* archivePath = argv[1];

    ArchiveFormat format = DetectArchiveFormatFromPath(archivePath);

    std::printf("Detected format: %s\n", ArchiveFormatToString(format));

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("ArchiveListEntries failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("Archive opened successfully.\n");
    std::printf("Files/items: %zu\n\n", entries.size());

    std::printf("%-8s %-10s %-14s %-8s %-8s %-10s %-8s %-8s %-8s %-10s %-8s %s\n", "Index", "Audio", "Size", "Folder", "Solid", "CRC", "HasCRC", "Block", "InBlock", "Method", "Stream", "Path");
    std::printf("-----------------------------------------------------------------------------------------------------------------------------------\n");

    for (const ArchiveEntry& entry : entries)
    {
        char methodText[32];

        if (entry.hasMethodId)
            sprintf_s(methodText, sizeof(methodText), "%08X", entry.methodId);
        else
            strcpy_s(methodText, sizeof(methodText), "-");

        std::printf("%-8u %-10s %-14llu %-8s %-8s %08X   %-8s %-8u %-8u %-10s %-8s %s\n", (unsigned int)entry.index, entry.isSupportedAudio ? "YES" : "", (unsigned long long)entry.size, entry.isFolder ? "YES" : "NO", entry.isSolid ? "YES" : "NO", (unsigned int)entry.crc32, entry.hasCrc32 ? "YES" : "NO", (unsigned int)entry.folderIndex, (unsigned int)entry.filesInFolder, methodText, entry.streamCandidate ? "YES" : "NO", entry.path.c_str());
    }

    if (argc >= 3)
    {
        uint32_t selectedIndex = 0;

        if (!ParseUInt32Arg(argv[2], selectedIndex))
        {
            std::printf("\nInvalid file index argument: expected a numeric index.\n");
            return 1;
        }

        const ArchiveEntry* selectedEntry = nullptr;

        for (const ArchiveEntry& entry : entries)
        {
            if (entry.index == selectedIndex)
            {
                selectedEntry = &entry;
                break;
            }
        }

        if (!selectedEntry)
        {
            std::printf("\nInvalid file index: %u\n", static_cast<unsigned>(selectedIndex));
            return 1;
        }

        bool streamMode = false;
        bool fileMode = false;

        if (argc >= 4 && _wcsicmp(argv[3], L"stream") == 0)
            streamMode = true;

        if (argc >= 5 && _wcsicmp(argv[3], L"file") == 0)
            fileMode = true;

        std::printf("\n%s index %u: %s\n", streamMode ? "Streaming" : "Extracting", static_cast<unsigned>(selectedIndex), selectedEntry->path.c_str());
        std::printf("Expected size: %llu byte(s)\n", static_cast<unsigned long long>(selectedEntry->size));

        if (streamMode)
        {
            if (!StreamEntryTest(archivePath, selectedIndex, *selectedEntry, error))
            {
                std::printf("ArchiveOpen/ReadEntryStream failed: %s\n", error.c_str());
                return 1;
            }
        }
        else if (fileMode)
        {
            const wchar_t* outputPath = argv[4];

            if (!ArchiveExtractEntryToFile(archivePath, selectedIndex, outputPath, error))
            {
                std::printf("ArchiveExtractEntryToFile failed: %s\n", error.c_str());
                return 1;
            }

            WIN32_FILE_ATTRIBUTE_DATA fad;

            if (GetFileAttributesExW(outputPath, GetFileExInfoStandard, &fad))
            {
                ULARGE_INTEGER size;
                size.HighPart = fad.nFileSizeHigh;
                size.LowPart = fad.nFileSizeLow;

                std::printf("Extracted file size: %llu byte(s)\n", static_cast<unsigned long long>(size.QuadPart));
            }

            std::printf("\nFile extraction test succeeded.\n");
        }
        else
        {
            std::vector<unsigned char> extractedData;

            if (!ArchiveExtractEntryToMemory(archivePath, selectedIndex, extractedData, error))
            {
                std::printf("ArchiveExtractEntryToMemory failed: %s\n", error.c_str());
                return 1;
            }

            std::printf("Extracted size: %zu byte(s)\n", extractedData.size());
            PrintFirstBytes(extractedData);
            std::printf("\nMemory extraction test succeeded.\n");
        }
    }

    return 0;
}