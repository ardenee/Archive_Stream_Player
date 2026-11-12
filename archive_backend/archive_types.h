#pragma once
#include <atomic>
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
    bool isEncrypted = false; // Header-provided entry encryption flag, not inferred from reading content.

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

enum class ArchiveStreamReadStatus : uint8_t
{
    NotOpen = 0,
    Ready,
    EndOfEntry,
    Error,
    Cancelled
};

// Caller-owned, thread-safe signal. A token must outlive every stream that
// observes it. Request cancellation from any thread; join in-flight reads
// before closing or reopening the stream (those operations are not reentrant).
struct ArchiveStreamCancellationToken
{
    void RequestCancellation() noexcept { requested.store(true, std::memory_order_release); }
    bool IsCancellationRequested() const noexcept { return requested.load(std::memory_order_acquire); }
    void Reset() noexcept { requested.store(false, std::memory_order_release); }

private:
    std::atomic<bool> requested{ false };
};

struct ArchiveBackendStream
{
    void* impl = nullptr;
    uint64_t size = 0;
    uint64_t position = 0;
    ArchiveFormat format = ArchiveFormat::Unknown;

    // Read result state: a zero-byte read is EOF only when status is EndOfEntry.
    // Other reader backends cannot always report a detailed error string yet.
    ArchiveStreamReadStatus readStatus = ArchiveStreamReadStatus::NotOpen;
    const ArchiveStreamCancellationToken* cancellation = nullptr;
};

