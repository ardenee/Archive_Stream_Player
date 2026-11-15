#pragma once

// Winamp's Out_Module::Write contract: at most 8192 bytes per call,
// complete PCM frames only, 0 on success and 1 when not yet accepted.
// This helper is shared by all five native PCM decoder branches.
#include <windows.h>
#include "OUT.H"

template <typename AbortFn, typename AcceptedFn>
inline bool ArchiveStreamWritePcmFrames(Out_Module* output, const void* data, int bytes, int frameBytes, AbortFn shouldAbort, AcceptedFn onAccepted)
{
    if (!output || !output->CanWrite || !output->Write || !data || bytes <= 0 || frameBytes <= 0 || frameBytes > 8192 || bytes % frameBytes != 0)
        return false;

    const char* cursor = static_cast<const char*>(data);
    int remaining = bytes;

    while (remaining > 0)
    {
        if (shouldAbort())
            return false;

        int available = output->CanWrite();
        if (available < frameBytes)
        {
            Sleep(5);
            continue;
        }

        // A host may report a very large or non-frame-aligned CanWrite value.
        // Limit each nonblocking Write to Winamp's documented 8192-byte maximum.
        int count = remaining;
        if (count > available)
            count = available;
        if (count > 8192)
            count = 8192;
        count -= count % frameBytes;

        if (count == 0)
        {
            Sleep(5);
            continue;
        }

        // A nonzero return means NOT accepted. Do not advance or discard bytes.
        if (output->Write(const_cast<char*>(cursor), count) != 0)
        {
            Sleep(5);
            continue;
        }

        onAccepted(count);
        cursor += count;
        remaining -= count;
    }

    return true;
}

template <typename AbortFn>
inline bool ArchiveStreamWritePcmFrames(Out_Module* output, const void* data, int bytes, int frameBytes, AbortFn shouldAbort)
{
    return ArchiveStreamWritePcmFrames(output, data, bytes, frameBytes, shouldAbort, [](int) {});
}
