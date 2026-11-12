#pragma once

#include "archive_backend.h"
#include <memory>
#include <utility>

// Move-only owner of exactly one generic ZIP/7z/RAR stream. The legacy
// ArchiveBackendStream ABI remains unchanged for existing Winamp callers.
// This wrapper is for new adapters, background workers and test utilities.
//
// Reads, seeks and Close() must run on the owning I/O thread. Cancel() can be
// called from another thread only while the owner remains alive. A cancelled
// token cannot be reset while I/O is in flight.
class ArchiveStreamOwner final
{
public:
    ArchiveStreamOwner() = default;
    ~ArchiveStreamOwner() { Close(); }

    ArchiveStreamOwner(const ArchiveStreamOwner&) = delete;
    ArchiveStreamOwner& operator=(const ArchiveStreamOwner&) = delete;

    ArchiveStreamOwner(ArchiveStreamOwner&& other) noexcept
        : stream_(other.stream_), token_(std::move(other.token_))
    {
        other.stream_ = ArchiveBackendStream{};
    }

    ArchiveStreamOwner& operator=(ArchiveStreamOwner&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            stream_ = other.stream_;
            token_ = std::move(other.token_);
            other.stream_ = ArchiveBackendStream{};
        }

        return *this;
    }

    bool Open(const wchar_t* path, uint32_t index, std::string& error)
    {
        if (stream_.impl)
        {
            error = "ArchiveStreamOwner already owns an open stream";
            return false;
        }

        if (!token_)
            token_ = std::make_unique<ArchiveStreamCancellationToken>();

        token_->Reset();
        return ArchiveOpenEntryStream(path, index, stream_, error, token_.get());
    }

    ArchiveStreamOperationResult OpenDetailed(const wchar_t* path, uint32_t index)
    {
        if (stream_.impl)
        {
            ArchiveStreamOperationResult result;
            result.format = stream_.format;
            result.position = stream_.position;
            result.status = stream_.readStatus;
            result.errorKind = ArchiveStreamErrorKind::AlreadyOpen;
            result.message = "ArchiveStreamOwner already owns an open stream";
            return result;
        }

        if (!token_)
            token_ = std::make_unique<ArchiveStreamCancellationToken>();

        token_->Reset();
        return ArchiveOpenEntryStreamDetailed(path, index, stream_, token_.get());
    }

    ArchiveStreamReadResult ReadDetailed(void* output, size_t length)
    {
        return ArchiveReadEntryStreamDetailed(stream_, output, length);
    }

    ArchiveStreamOperationResult SeekDetailed(uint64_t position)
    {
        return ArchiveSeekEntryStreamDetailed(stream_, position);
    }

    void Cancel() noexcept
    {
        if (token_)
            token_->RequestCancellation();
    }

    void Close() noexcept
    {
        ArchiveCloseEntryStream(stream_);
    }

    bool IsOpen() const noexcept { return stream_.impl != nullptr; }
    uint64_t Size() const noexcept { return stream_.size; }
    uint64_t Tell() const noexcept { return stream_.position; }
    ArchiveFormat Format() const noexcept { return stream_.format; }
    ArchiveStreamReadStatus Status() const noexcept { return stream_.readStatus; }

    // Useful to an adapter that has a separate stop/UI thread.
    // Valid until the owner is destroyed or moved.
    ArchiveStreamCancellationToken* CancellationToken() noexcept { return token_.get(); }

    size_t Read(void* output, size_t length) { return ArchiveReadEntryStream(stream_, output, length); }
    bool Seek(uint64_t position) { return ArchiveSeekEntryStream(stream_, position); }

    ArchiveBackendStream& RawStream() noexcept { return stream_; }
    const ArchiveBackendStream& RawStream() const noexcept { return stream_; }

private:
    ArchiveBackendStream stream_{};
    std::unique_ptr<ArchiveStreamCancellationToken> token_;
};
