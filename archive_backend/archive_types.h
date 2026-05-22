#pragma once
#include <cstdint>
#include <string>

enum class ArchiveFormat
{
    Unknown,
    Zip,
    SevenZip,
    Rar
};

struct ArchiveEntry
{
    uint32_t index = 0;
    std::string path;
    uint64_t size = 0;
    bool isFolder = false;
    bool isSupportedAudio = false;

    uint32_t crc32 = 0;
    bool hasCrc32 = false;

    bool isSolid = false;
    uint32_t folderIndex = 0xFFFFFFFF;
    uint32_t filesInFolder = 0;

    // 7z primary coder method metadata.
    // ZIP leaves this as 0.
    uint32_t methodId = 0;
    bool hasMethodId = false;
    bool streamCandidate = false;
};

struct ArchiveBackendStream
{
    void* impl = nullptr;
    uint64_t size = 0;
    uint64_t position = 0;
    ArchiveFormat format = ArchiveFormat::Unknown;
};

