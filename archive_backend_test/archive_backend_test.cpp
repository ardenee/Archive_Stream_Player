#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

#include "archive_backend.h"
#include "archive_stream_owner.h"
#include "archive_format.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "archive_reader.h"

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

// Cancellation is signalled from a different thread, but close and I/O stay on
// the owning thread. A token must outlive the open stream.
static int RunCancellationRegression(const wchar_t* archivePath)
{
    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("FAIL: cancellation fixture listing: %s\n", error.c_str());
        return 1;
    }

    const ArchiveEntry* entry = nullptr;
    for (const ArchiveEntry& candidate : entries)
    {
        if (!candidate.isFolder && candidate.size > 64)
        {
            entry = &candidate;
            break;
        }
    }

    if (!entry)
    {
        std::printf("FAIL: cancellation test requires a nonempty entry over 64 bytes\n");
        return 1;
    }

    auto check = [](bool passed, const char* label) -> bool
    {
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", label);
        return passed;
    };

    ArchiveBackendStream stream;
    ArchiveStreamCancellationToken alreadyCancelled;
    alreadyCancelled.RequestCancellation();
    bool opened = ArchiveOpenEntryStream(archivePath, entry->index, stream, error, &alreadyCancelled);
    if (!check(!opened && !stream.impl && stream.readStatus == ArchiveStreamReadStatus::Cancelled && !error.empty(), "reject pre-cancelled stream open"))
    {
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    ArchiveStreamCancellationToken token;
    if (!ArchiveOpenEntryStream(archivePath, entry->index, stream, error, &token))
    {
        std::printf("FAIL: cancellation stream open: %s\n", error.c_str());
        return 1;
    }

    if (!check(stream.readStatus == ArchiveStreamReadStatus::Ready && stream.cancellation == &token, "open retains caller-owned token"))
    {
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    // Request cancellation across threads; the reader observes it via atomics.
    std::thread canceller([&token]() { token.RequestCancellation(); });
    canceller.join();

    unsigned char bytes[64] = {};
    size_t got = ArchiveReadEntryStream(stream, bytes, sizeof(bytes));
    if (!check(got == 0 && stream.readStatus == ArchiveStreamReadStatus::Cancelled && stream.position == 0, "cross-thread cancellation stops read without consuming data"))
    {
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    if (!check(!ArchiveSeekEntryStream(stream, 16) && stream.readStatus == ArchiveStreamReadStatus::Cancelled && stream.position == 0, "cancelled stream rejects seek"))
    {
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    ArchiveCloseEntryStream(stream);
    ArchiveCloseEntryStream(stream);
    if (!check(!stream.impl && !stream.cancellation && stream.readStatus == ArchiveStreamReadStatus::NotOpen, "cancelled stream close resets ownership and status"))
        return 1;

    // A new stream may use a new token without changing the old one's state.
    ArchiveStreamCancellationToken fresh;
    if (!ArchiveOpenEntryStream(archivePath, entry->index, stream, error, &fresh))
    {
        std::printf("FAIL: reopen after cancellation: %s\n", error.c_str());
        return 1;
    }

    got = ArchiveReadEntryStream(stream, bytes, sizeof(bytes));
    if (!check(got > 0 && stream.readStatus == ArchiveStreamReadStatus::Ready, "fresh token permits decoding after cancellation"))
    {
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    ArchiveCloseEntryStream(stream);
    return 0;
}

static int RunLegacyStreamOwnershipRegression(const wchar_t* archivePath)
{
    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("FAIL: legacy fixture listing: %s\n", error.c_str());
        return 1;
    }

    const ArchiveEntry* entry = nullptr;
    for (const ArchiveEntry& candidate : entries)
    {
        if (!candidate.isFolder && candidate.size > 64)
        {
            entry = &candidate;
            break;
        }
    }

    if (!entry)
    {
        std::printf("FAIL: legacy test requires a nonempty entry over 64 bytes\n");
        return 1;
    }

    int required = WideCharToMultiByte(CP_UTF8, 0, archivePath, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1)
        return 1;

    std::string utf8Path(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, archivePath, -1, &utf8Path[0], required, nullptr, nullptr) != required)
        return 1;
    utf8Path.resize(static_cast<size_t>(required - 1));

    ArchiveStreamContext legacy{};
    if (!ArchiveOpenEntryStream(&legacy, utf8Path.c_str(), entry->index))
    {
        std::printf("FAIL: legacy stream initial open\n");
        return 1;
    }

    void* originalHandle = legacy.libarchiveZipStream ? legacy.libarchiveZipStream : legacy.backendStream.impl;
    const bool zipOpen = legacy.libarchiveZipOpen;
    const bool backendOpen = legacy.backendOpen;
    if (!originalHandle || ArchiveOpenEntryStream(&legacy, utf8Path.c_str(), entry->index))
    {
        std::printf("FAIL: legacy double-open was accepted\n");
        ArchiveCloseEntryStream(&legacy);
        return 1;
    }

    std::string preflightError;
    if (ArchiveOpenValidatedEntryStream(&legacy, utf8Path.c_str(), entry->index, 16, &preflightError) || preflightError != "stream_already_open")
    {
        std::printf("FAIL: legacy validated-open erased a live stream\n");
        ArchiveCloseEntryStream(&legacy);
        return 1;
    }

    void* afterHandle = legacy.libarchiveZipStream ? legacy.libarchiveZipStream : legacy.backendStream.impl;
    if (originalHandle != afterHandle || legacy.libarchiveZipOpen != zipOpen || legacy.backendOpen != backendOpen)
    {
        std::printf("FAIL: legacy ownership state changed after rejected opens\n");
        ArchiveCloseEntryStream(&legacy);
        return 1;
    }

    unsigned char buffer[32] = {};
    if (ArchiveReadEntryStream(&legacy, buffer, sizeof(buffer)) == 0)
    {
        std::printf("FAIL: legacy stream unusable after rejected reopen\n");
        ArchiveCloseEntryStream(&legacy);
        return 1;
    }

    ArchiveCloseEntryStream(&legacy);
    ArchiveCloseEntryStream(&legacy);
    if (legacy.libarchiveZipStream || legacy.backendStream.impl || legacy.libarchiveZipOpen || legacy.backendOpen)
    {
        std::printf("FAIL: legacy close did not release handles\n");
        return 1;
    }

    if (!ArchiveOpenEntryStream(&legacy, utf8Path.c_str(), entry->index))
    {
        std::printf("FAIL: legacy reopen after close\n");
        return 1;
    }
    ArchiveCloseEntryStream(&legacy);

    preflightError.clear();
    if (!ArchiveOpenValidatedEntryStream(&legacy, utf8Path.c_str(), entry->index, 16, &preflightError))
    {
        std::printf("FAIL: legacy validated-open failed on a closed stream: %s\n", preflightError.c_str());
        return 1;
    }
    ArchiveCloseEntryStream(&legacy);

    // Overlong paths should return false rather than invoke the CRT's
    // invalid-parameter handler through strcpy_s.
    std::string oversizedPath(MAX_PATH * 4 + 4, 'x');
    if (ArchiveOpenEntryStream(&legacy, oversizedPath.c_str(), entry->index))
    {
        std::printf("FAIL: legacy accepted overlong archive path\n");
        ArchiveCloseEntryStream(&legacy);
        return 1;
    }

    std::printf("PASS: legacy stream rejected double-open, retained handle, closed and reopened\n");
    return 0;
}

static int RunOwnedStreamRegression(const wchar_t* archivePath, bool expectCorrupt)
{
    static_assert(!std::is_copy_constructible_v<ArchiveStreamOwner>);
    static_assert(!std::is_copy_assignable_v<ArchiveStreamOwner>);
    static_assert(std::is_nothrow_move_constructible_v<ArchiveStreamOwner>);
    static_assert(std::is_nothrow_move_assignable_v<ArchiveStreamOwner>);

    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("FAIL: stream owner fixture listing: %s\n", error.c_str());
        return 1;
    }

    const ArchiveEntry* entry = nullptr;
    for (const auto& candidate : entries)
    {
        if (!candidate.isFolder && candidate.size > 64)
        {
            entry = &candidate;
            break;
        }
    }

    if (!entry)
        return 1;

    ArchiveStreamOwner owner;
    unsigned char bytes[4096] = {};
    auto closedRead = ArchiveReadEntryStreamDetailed(owner.RawStream(), bytes, 64);
    if (closedRead.errorKind != ArchiveStreamErrorKind::NotOpen || closedRead.status != ArchiveStreamReadStatus::Error)
    {
        std::printf("FAIL: detailed read did not flag unopened stream\n");
        return 1;
    }

    if (!owner.Open(archivePath, entry->index, error))
    {
        std::printf("FAIL: owned stream open: %s\n", error.c_str());
        return 1;
    }

    const auto original = owner.RawStream().impl;
    ArchiveStreamOwner moved(std::move(owner));
    if (owner.IsOpen() || !moved.IsOpen() || moved.RawStream().impl != original)
    {
        std::printf("FAIL: move construction did not transfer opaque ownership\n");
        return 1;
    }

    ArchiveStreamOwner target;
    target = std::move(moved);
    if (moved.IsOpen() || !target.IsOpen() || target.RawStream().impl != original)
    {
        std::printf("FAIL: move assignment did not transfer opaque ownership\n");
        return 1;
    }

    uint64_t total = 0;
    ArchiveStreamReadResult end{};
    for (;;)
    {
        end = ArchiveReadEntryStreamDetailed(target.RawStream(), bytes, sizeof(bytes));
        total += end.bytesRead;
        if (end.bytesRead == 0)
            break;

        if (total > entry->size)
        {
            std::printf("FAIL: detailed reader exceeded entry size\n");
            return 1;
        }
    }

    if (expectCorrupt)
    {
        if (end.status != ArchiveStreamReadStatus::Error || end.errorKind != ArchiveStreamErrorKind::BackendFailure || end.message.empty())
        {
            std::printf("FAIL: corrupted ZIP error was not propagated; status=%u kind=%u\n", (unsigned)end.status, (unsigned)end.errorKind);
            return 1;
        }

        std::printf("PASS: backend-specific ZIP decompression error: %s\n", end.message.c_str());
    }
    else if (total != entry->size || end.status != ArchiveStreamReadStatus::EndOfEntry || end.errorKind != ArchiveStreamErrorKind::None)
    {
        std::printf("FAIL: detailed read status or byte total incorrect\n");
        return 1;
    }

    if (!target.Seek(0))
    {
        std::printf("FAIL: owned stream rewind after read\n");
        return 1;
    }

    auto invalidBuffer = ArchiveReadEntryStreamDetailed(target.RawStream(), nullptr, 20);
    if (invalidBuffer.errorKind != ArchiveStreamErrorKind::InvalidArgument || invalidBuffer.status != ArchiveStreamReadStatus::Error)
    {
        std::printf("FAIL: null output buffer error not classified\n");
        return 1;
    }

    if (!target.Seek(0))
    {
        std::printf("FAIL: owned stream seek after invalid buffer\n");
        return 1;
    }

    auto noOp = ArchiveReadEntryStreamDetailed(target.RawStream(), bytes, 0);
    if (noOp.bytesRead != 0 || noOp.errorKind != ArchiveStreamErrorKind::None)
    {
        std::printf("FAIL: zero-byte read was incorrectly classified\n");
        return 1;
    }

    target.Cancel();
    auto cancelled = ArchiveReadEntryStreamDetailed(target.RawStream(), bytes, 20);
    if (cancelled.status != ArchiveStreamReadStatus::Cancelled || cancelled.errorKind != ArchiveStreamErrorKind::Cancelled)
    {
        std::printf("FAIL: detailed cancellation status not reported\n");
        return 1;
    }

    target.Close();
    target.Close();
    if (target.IsOpen() || target.Status() != ArchiveStreamReadStatus::NotOpen)
    {
        std::printf("FAIL: owner did not release underlying handle\n");
        return 1;
    }

    if (!target.Open(archivePath, entry->index, error))
    {
        std::printf("FAIL: owner cannot reopen after cancellation: %s\n", error.c_str());
        return 1;
    }

    target.Close();
    std::printf("PASS: move-only ownership, structured status, reset and close\n");
    return 0;
}

static int RunStructuredOperationsRegression(const wchar_t* archivePath)
{
    int failures = 0;
    auto check = [&](bool valid, const char* description)
    {
        std::printf("%s: %s\n", valid ? "PASS" : "FAIL", description);
        if (!valid)
            ++failures;
    };

    ArchiveBackendStream raw;
    auto seekClosed = ArchiveSeekEntryStreamDetailed(raw, 0);
    check(!seekClosed.success && seekClosed.errorKind == ArchiveStreamErrorKind::NotOpen &&
        seekClosed.status == ArchiveStreamReadStatus::Error, "typed seek refuses closed stream");

    auto noPath = ArchiveOpenEntryStreamDetailed(nullptr, 0, raw);
    check(!noPath.success && noPath.errorKind == ArchiveStreamErrorKind::InvalidArgument &&
        noPath.status == ArchiveStreamReadStatus::Error, "typed open reports invalid path");

    auto unsupported = ArchiveOpenEntryStreamDetailed(L"test.unsupported", 0, raw);
    check(!unsupported.success && unsupported.errorKind == ArchiveStreamErrorKind::UnsupportedFormat,
        "typed open reports unknown archive format");

    ArchiveStreamCancellationToken cancelled;
    cancelled.RequestCancellation();
    auto preCancelled = ArchiveOpenEntryStreamDetailed(archivePath, 0, raw, &cancelled);
    check(!preCancelled.success && preCancelled.errorKind == ArchiveStreamErrorKind::Cancelled &&
        preCancelled.status == ArchiveStreamReadStatus::Cancelled, "typed open reports cancellation");
    ArchiveCloseEntryStream(raw);

    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("FAIL: typed-operation fixture list: %s\n", error.c_str());
        return 1;
    }

    const ArchiveEntry* entry = nullptr;
    for (const auto& candidate : entries)
    {
        if (!candidate.isFolder && candidate.size > 64)
        {
            entry = &candidate;
            break;
        }
    }

    if (!entry)
    {
        std::printf("FAIL: typed-operation fixture requires an entry over 64 bytes\n");
        return 1;
    }

    ArchiveStreamOwner owner;
    auto opened = owner.OpenDetailed(archivePath, entry->index);
    check(opened.success && opened.errorKind == ArchiveStreamErrorKind::None &&
        opened.status == ArchiveStreamReadStatus::Ready && owner.IsOpen(),
        "owner typed open succeeds with ready status");

    void* before = owner.RawStream().impl;
    auto duplicate = owner.OpenDetailed(archivePath, entry->index);
    check(!duplicate.success && duplicate.errorKind == ArchiveStreamErrorKind::AlreadyOpen &&
        owner.RawStream().impl == before && owner.IsOpen(),
        "typed double-open preserves the original handle");

    auto outOfBounds = owner.SeekDetailed(owner.Size() + 1);
    check(!outOfBounds.success && outOfBounds.errorKind == ArchiveStreamErrorKind::OutOfRange &&
        outOfBounds.status == ArchiveStreamReadStatus::Error && owner.Tell() == 0,
        "typed out-of-range seek preserves position");

    auto rewind = owner.SeekDetailed(0);
    check(rewind.success && rewind.status == ArchiveStreamReadStatus::Ready && owner.Tell() == 0,
        "valid seek recovers from previous error");

    unsigned char bytes[64] = {};
    auto read = owner.ReadDetailed(bytes, sizeof(bytes));
    check(read.bytesRead == sizeof(bytes) && read.status == ArchiveStreamReadStatus::Ready &&
        read.errorKind == ArchiveStreamErrorKind::None, "owner typed read reports bytes");

    owner.Cancel();
    auto cancelledSeek = owner.SeekDetailed(0);
    check(!cancelledSeek.success && cancelledSeek.errorKind == ArchiveStreamErrorKind::Cancelled &&
        cancelledSeek.status == ArchiveStreamReadStatus::Cancelled, "typed seek reports cancellation");
    owner.Close();

    auto invalidIndex = ArchiveOpenEntryStreamDetailed(archivePath, UINT32_MAX, raw);
    check(!invalidIndex.success && invalidIndex.errorKind == ArchiveStreamErrorKind::BackendFailure &&
        !invalidIndex.message.empty() && !raw.impl, "typed open propagates invalid-index failure");
    ArchiveCloseEntryStream(raw);

    std::printf("Structured stream operation validation: %d failure(s)\n", failures);
    return failures ? 1 : 0;
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

    check(!ArchiveListEntries(static_cast<const wchar_t*>(nullptr), entries, error) && !error.empty(), "list null path");
    check(!ArchiveListEntries(L"", entries, error) && !error.empty(), "list empty path");
    check(!ArchiveExtractEntryToMemory(static_cast<const wchar_t*>(nullptr), 0, data, error) && !error.empty(), "extract memory null path");
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

static int RunPasswordedArchiveFileRegression(const wchar_t* archivePath, const wchar_t* passwordListPath, const wchar_t* indexText, const wchar_t* outputPath)
{
    if (!archivePath || !passwordListPath || !indexText || !outputPath)
        return 1;

    uint32_t index = 0;
    if (!ParseUInt32Arg(indexText, index))
        return 1;

    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, passwordListPath, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return 1;

    std::string passwordPath(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, passwordListPath, -1, passwordPath.data(), len, nullptr, nullptr) != len)
        return 1;

    ArchivePasswordSetListPath(passwordPath.c_str());

    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("PASSWORD_TEST_LIST_FAIL: %s\n", error.c_str());
        return 1;
    }

    const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& e) { return e.index == index && !e.isFolder; });
    if (found == entries.end())
    {
        std::printf("PASSWORD_TEST_INDEX_FAIL: index=%u entries=%zu\n", (unsigned)index, entries.size());
        return 1;
    }

    if (!ArchiveExtractEntryToFile(archivePath, index, outputPath, error))
    {
        std::printf("PASSWORD_TEST_EXTRACT_FAIL: %s\n", error.c_str());
        return 1;
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(outputPath, GetFileExInfoStandard, &attrs))
        return 1;

    const uint64_t extractedSize = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
    if (extractedSize != found->size)
    {
        std::printf("PASSWORD_TEST_SIZE_FAIL: expected=%llu got=%llu\n", (unsigned long long)found->size, (unsigned long long)extractedSize);
        return 1;
    }

    std::printf("PASSWORD_TEST_PASS: index=%u bytes=%llu entries=%zu encrypted=%d\n", (unsigned)index, (unsigned long long)extractedSize, entries.size(), found->isEncrypted ? 1 : 0);
    return 0;
}

static int RunPasswordedArchiveStreamRegression(const wchar_t* archivePath, const wchar_t* passwordListPath, const wchar_t* indexText)
{
    uint32_t index = 0;
    if (!archivePath || !passwordListPath || !indexText || !ParseUInt32Arg(indexText, index))
        return 1;

    const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, passwordListPath, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return 1;
    std::string passwordPath(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, passwordListPath, -1, passwordPath.data(), len, nullptr, nullptr) != len)
        return 1;
    ArchivePasswordSetListPath(passwordPath.c_str());

    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(archivePath, entries, error))
    {
        std::printf("PASSWORD_STREAM_LIST_FAIL: %s\n", error.c_str());
        return 1;
    }
    const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& e) { return e.index == index && !e.isFolder; });
    if (found == entries.end() || found->size > 8 * 1024 * 1024)
        return 1;

    ArchiveBackendStream stream{};
    if (!ArchiveOpenEntryStream(archivePath, index, stream, error))
    {
        std::printf("PASSWORD_STREAM_OPEN_FAIL: %s\n", error.c_str());
        return 1;
    }

    std::vector<unsigned char> decoded;
    unsigned char buffer[8192];
    while (decoded.size() < found->size)
    {
        const size_t wanted = (std::min)(sizeof(buffer), static_cast<size_t>(found->size - decoded.size()));
        const size_t got = ArchiveReadEntryStream(stream, buffer, wanted);
        if (got == 0)
            break;
        decoded.insert(decoded.end(), buffer, buffer + got);
    }
    if (decoded.size() != found->size)
    {
        std::printf("PASSWORD_STREAM_TRUNCATED: expected=%llu got=%zu\n", (unsigned long long)found->size, decoded.size());
        ArchiveCloseEntryStream(stream);
        return 1;
    }

    const uint64_t seekTarget = found->size / 2;
    if (!ArchiveSeekEntryStream(stream, seekTarget))
    {
        std::printf("PASSWORD_STREAM_SEEK_FAIL\n");
        ArchiveCloseEntryStream(stream);
        return 1;
    }
    const size_t nextBytes = (std::min)(sizeof(buffer), decoded.size() - static_cast<size_t>(seekTarget));
    const size_t got = ArchiveReadEntryStream(stream, buffer, nextBytes);
    const bool seekValid = got == nextBytes && std::memcmp(buffer, decoded.data() + seekTarget, nextBytes) == 0;
    ArchiveCloseEntryStream(stream);
    if (!seekValid)
    {
        std::printf("PASSWORD_STREAM_SEEK_MISMATCH\n");
        return 1;
    }

    std::vector<unsigned char> extracted;
    if (!ArchiveExtractEntryToMemory(archivePath, index, extracted, error) || decoded != extracted)
    {
        std::printf("PASSWORD_STREAM_MEMORY_PARITY_FAIL: %s\n", error.c_str());
        return 1;
    }

    std::printf("PASSWORD_STREAM_PASS: index=%u bytes=%zu seek=PASS memory_parity=PASS encrypted=%d\n", (unsigned)index, decoded.size(), found->isEncrypted ? 1 : 0);
    return 0;
}

int wmain(int argc, wchar_t* argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc >= 2 && _wcsicmp(argv[1], L"selftest") == 0)
        return RunApiValidationTests();

    if (argc >= 2 && _wcsicmp(argv[1], L"pwfiletest") == 0)
    {
        if (argc != 6 && argc != 8)
        {
            std::printf("pwfiletest requires archive, password list, index, output, and optionally a second index/output pair\n");
            return 1;
        }

        if (RunPasswordedArchiveFileRegression(argv[2], argv[3], argv[4], argv[5]) != 0)
            return 1;

        if (argc >= 8)
            return RunPasswordedArchiveFileRegression(argv[2], argv[3], argv[6], argv[7]);

        return 0;
    }

    if (argc >= 2 && _wcsicmp(argv[1], L"pwstreamtest") == 0)
    {
        if (argc != 5)
        {
            std::printf("pwstreamtest requires archive, password list and entry index\n");
            return 1;
        }
        return RunPasswordedArchiveStreamRegression(argv[2], argv[3], argv[4]);
    }

    if (argc >= 3 && _wcsicmp(argv[1], L"ziptest") == 0)
        return RunGenericZipStreamRegression(argv[2]);

    if (argc >= 3 && _wcsicmp(argv[1], L"canceltest") == 0)
        return RunCancellationRegression(argv[2]);

    if (argc >= 3 && _wcsicmp(argv[1], L"legacytest") == 0)
        return RunLegacyStreamOwnershipRegression(argv[2]);

    if (argc >= 3 && _wcsicmp(argv[1], L"ownertest") == 0)
        return RunOwnedStreamRegression(argv[2], argc >= 4 && _wcsicmp(argv[3], L"corrupt") == 0);

    if (argc >= 3 && _wcsicmp(argv[1], L"diagtest") == 0)
        return RunStructuredOperationsRegression(argv[2]);

    if (argc < 2)
    {
        std::printf("Usage:\n");
        std::printf("  archive_backend_test.exe selftest\n");
        std::printf("  archive_backend_test.exe pwfiletest archive.rar-or-7z passwords.txt index extracted.file\n");
        std::printf("  archive_backend_test.exe pwstreamtest archive.7z passwords.txt index\n");
        std::printf("  archive_backend_test.exe ziptest archive.zip\n");
        std::printf("  archive_backend_test.exe canceltest archive.zip\n");
        std::printf("  archive_backend_test.exe legacytest archive.zip\n");
        std::printf("  archive_backend_test.exe ownertest archive.zip [corrupt]\n");
        std::printf("  archive_backend_test.exe diagtest archive.zip\n");
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