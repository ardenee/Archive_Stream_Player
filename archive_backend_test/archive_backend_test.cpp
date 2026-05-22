#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <string>
#include <algorithm>

#include "archive_backend.h"
#include "archive_format.h"

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
            break;

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
            "WARNING: streamed byte count does not match listed entry size.\n"
            "Listed:   %llu\n"
            "Streamed: %llu\n",
            static_cast<unsigned long long>(selectedEntry.size),
            totalRead
        );
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

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2)
    {
        std::printf("Usage:\n");
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