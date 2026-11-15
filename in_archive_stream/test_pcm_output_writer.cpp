// Standalone Winamp output contract regression (no sound device or host).
// Build with VS x86/x64 compiler, including third_party/winamp.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include "pcm_output_writer.h"

struct MockOutputState
{
    std::vector<unsigned char> accepted;
    int frameBytes = 0;
    int canWriteCalls = 0;
    int writes = 0;
    int refused = 0;
    int maximumWrite = 0;
    bool contractViolated = false;
};

static MockOutputState* g_state = nullptr;

static int MockCanWrite()
{
    MockOutputState& state = *g_state;
    ++state.canWriteCalls;
    static const int capacities[] = {0, 1, 3, 2047, 8191, 8192, 8193, 32767, 16384, 509, 6};
    return capacities[(state.canWriteCalls - 1) % (sizeof(capacities) / sizeof(capacities[0]))];
}

static int MockWrite(char* bytes, int length)
{
    MockOutputState& state = *g_state;
    ++state.writes;
    if (length > 8192 || length <= 0 || length % state.frameBytes != 0)
    {
        state.contractViolated = true;
        return 1;
    }
    if (length > state.maximumWrite)
        state.maximumWrite = length;

    // The output plugin is allowed to reject an entire block even when
    // CanWrite had reported capacity. Caller must retry the SAME samples.
    if (state.writes % 5 == 0)
    {
        ++state.refused;
        return 1;
    }

    state.accepted.insert(state.accepted.end(),
                          reinterpret_cast<unsigned char*>(bytes),
                          reinterpret_cast<unsigned char*>(bytes) + length);
    return 0;
}

static bool TestLosslessPcm(int bytesPerFrame)
{
    MockOutputState state;
    state.frameBytes = bytesPerFrame;
    g_state = &state;

    Out_Module output = {};
    output.CanWrite = MockCanWrite;
    output.Write = MockWrite;

    std::vector<unsigned char> source(23011 * (size_t)bytesPerFrame);
    std::uint32_t randomValue = 0xFA379CC9u;
    for (unsigned char& sample : source)
    {
        randomValue = randomValue * 1664525u + 1013904223u;
        sample = (unsigned char)(randomValue >> 24);
    }

    int acceptedBytes = 0;
    int acceptedBlocks = 0;
    const bool result = ArchiveStreamWritePcmFrames(&output, source.data(), (int)source.size(), bytesPerFrame,
        []() { return false; }, [&](int count)
        {
            acceptedBytes += count;
            ++acceptedBlocks;
        });

    if (!result || state.contractViolated || state.refused == 0 ||
        state.accepted != source || acceptedBytes != (int)source.size() ||
        acceptedBlocks <= 1 || state.maximumWrite > 8192)
    {
        std::printf("PCM_OUTPUT_LOSSLESS_FAIL frame=%d accepted=%zu source=%zu refused=%d blocks=%d max=%d\n",
                    bytesPerFrame, state.accepted.size(), source.size(), state.refused, acceptedBlocks, state.maximumWrite);
        return false;
    }

    std::printf("PCM_OUTPUT_LOSSLESS_PASS frame=%d pcm=%zu blocks=%d rejected_and_retried=%d max_write=%d\n",
                bytesPerFrame, source.size(), acceptedBlocks, state.refused, state.maximumWrite);
    return true;
}

static bool TestAbortAndInvalid()
{
    MockOutputState state;
    state.frameBytes = 4;
    g_state = &state;

    Out_Module output = {};
    output.CanWrite = MockCanWrite;
    output.Write = MockWrite;
    std::vector<unsigned char> data(18000 * 4, 0xA7);

    const bool aborted = ArchiveStreamWritePcmFrames(&output, data.data(), (int)data.size(), 4,
        [&state]() { return state.accepted.size() >= 4096; });
    if (aborted || state.accepted.empty() || state.accepted.size() >= data.size() || state.contractViolated)
        return false;

    // A partial PCM frame must be rejected rather than emitted.
    state.accepted.clear();
    if (ArchiveStreamWritePcmFrames(&output, data.data(), 4095, 4, []() { return false; }))
        return false;
    if (!state.accepted.empty())
        return false;

    std::puts("PCM_OUTPUT_ABORT_AND_PARTIAL_FRAME_REJECT_PASS");
    return true;
}

int main()
{
    bool passed = true;
    for (int bytesPerFrame : {2, 4, 6, 8})
        passed = TestLosslessPcm(bytesPerFrame) && passed;
    passed = TestAbortAndInvalid() && passed;
    if (!passed)
        return 1;
    std::puts("WINAMP_OUTPUT_PCM_CONTRACT_ALL_PASS");
    return 0;
}
