#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <string>

#include "archive_common.h"
#include "plugin_state.h"
#include "winamp_bridge_protocol.h"
#include "playback.h"
#include "archive_reader.h"
#include "mf_audio_decoder.h"
#include "mf_support.h"
#include "debug_log.h"

#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

static size_t StreamingMp3Read(void* userData, void* bufferOut, size_t bytesToRead)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx || !bufferOut || bytesToRead == 0)
        return 0;

    return ArchiveReadEntryStream(ctx, bufferOut, bytesToRead);
}

static size_t StreamingWavRead(void* userData, void* bufferOut, size_t bytesToRead)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx || !bufferOut || bytesToRead == 0)
        return 0;

    return ArchiveReadEntryStream(ctx, bufferOut, bytesToRead);
}

static drwav_bool32 StreamingWavSeek(void* userData, int offset, drwav_seek_origin origin)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx)
        return DRWAV_FALSE;

    // dr_wav seek origin values normally match:
    // 0 = start
    // 1 = current
    if ((int)origin == 0)
    {
        if (offset < 0)
            return DRWAV_FALSE;

        mz_uint64 target = (mz_uint64)offset;

        if (target < ctx->position)
            return DRWAV_FALSE;

        offset = (int)(target - ctx->position);
    }
    else if ((int)origin == 1)
    {
        if (offset < 0)
            return DRWAV_FALSE;
    }
    else
    {
        return DRWAV_FALSE;
    }

    unsigned char scratch[4096];
    int remaining = offset;

    while (remaining > 0)
    {
        size_t toRead = remaining > (int)sizeof(scratch) ? sizeof(scratch) : (size_t)remaining;
        size_t bytesRead = StreamingWavRead(ctx, scratch, toRead);

        if (bytesRead == 0)
            return DRWAV_FALSE;

        remaining -= (int)bytesRead;
    }

    return DRWAV_TRUE;
}

static drwav_bool32 StreamingWavTell(void* userData, drwav_int64* pCursor)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx || !pCursor)
        return DRWAV_FALSE;

    *pCursor = (drwav_int64)ctx->position;
    return DRWAV_TRUE;
}

static size_t StreamingFlacRead(void* userData, void* bufferOut, size_t bytesToRead)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx || !bufferOut || bytesToRead == 0)
        return 0;

    return ArchiveReadEntryStream(ctx, bufferOut, bytesToRead);
}

static drflac_bool32 StreamingFlacTell(void* userData, drflac_int64* pCursor)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx || !pCursor)
        return DRFLAC_FALSE;

    *pCursor = (drflac_int64)ctx->position;
    return DRFLAC_TRUE;
}

static drflac_bool32 StreamingFlacSeek(void* userData, int offset, drflac_seek_origin origin)
{
    ArchiveStreamContext* ctx = (ArchiveStreamContext*)userData;

    if (!ctx)
        return DRFLAC_FALSE;

    // Same idea as WAV streaming:
    // allow forward-only seek while the decoder parses metadata.
    // 0 = start, 1 = current in the dr_libs seek-origin convention.
    if ((int)origin == 0)
    {
        if (offset < 0)
            return DRFLAC_FALSE;

        mz_uint64 target = (mz_uint64)offset;

        if (target < ctx->position)
            return DRFLAC_FALSE;

        offset = (int)(target - ctx->position);
    }
    else if ((int)origin == 1)
    {
        if (offset < 0)
            return DRFLAC_FALSE;
    }
    else
    {
        return DRFLAC_FALSE;
    }

    unsigned char scratch[4096];
    int remaining = offset;

    while (remaining > 0)
    {
        size_t toRead = remaining > (int)sizeof(scratch) ? sizeof(scratch) : (size_t)remaining;
        size_t bytesRead = StreamingFlacRead(ctx, scratch, toRead);

        if (bytesRead == 0)
            return DRFLAC_FALSE;

        remaining -= (int)bytesRead;
    }

    return DRFLAC_TRUE;
}

static bool OpenStreamingMp3Context(ArchiveStreamContext* ctx, const char* zipPath, mz_uint zipIndex)
{
    if (!ctx)
    {
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (InterlockedCompareExchange(&g_prevalidatedStreamReady, 0, 0) != 0)
    {
        if (zipPath && zipPath[0] && _stricmp(zipPath, g_currentArchivePath) == 0 && zipIndex == g_currentArchiveIndex)
        {
            *ctx = g_prevalidatedStream;
            memset(&g_prevalidatedStream, 0, sizeof(g_prevalidatedStream));
            InterlockedExchange(&g_prevalidatedStreamReady, 0);

            DebugLogWrite("INFO", "STREAM", "PREVALIDATED_STREAM_TAKE", "archive=%s | index=%u", zipPath, (unsigned int)zipIndex);
            return true;
        }

        DebugLogWrite("WARN", "STREAM", "PREVALIDATED_STREAM_MISMATCH", "requested_archive=%s | requested_index=%u | current_archive=%s | current_index=%u", zipPath ? zipPath : "", (unsigned int)zipIndex, g_currentArchivePath, (unsigned int)g_currentArchiveIndex);
    }

    return ArchiveOpenEntryStream(ctx, zipPath, zipIndex);
}

static void CloseStreamingMp3Context(ArchiveStreamContext* ctx)
{
    ArchiveCloseEntryStream(ctx);
}

struct OggPushStream
{
    ArchiveStreamContext archive;
    stb_vorbis* vorbis;
    std::vector<unsigned char> buffer;
    bool archiveOpen;
    bool eof;
    int channels;
    int sampleRate;
};

static bool FillOggPushBuffer(OggPushStream* ctx, size_t minBytes)
{
    if (!ctx || ctx->eof)
        return false;

    unsigned char temp[32768];

    while (ctx->buffer.size() < minBytes)
    {
        size_t bytesRead = ArchiveReadEntryStream(&ctx->archive, temp, sizeof(temp));

        if (bytesRead == 0)
        {
            ctx->eof = true;
            break;
        }

        ctx->buffer.insert(ctx->buffer.end(), temp, temp + bytesRead);
    }

    return !ctx->buffer.empty();
}

static bool RestartStreamingMp3AtMs(drmp3* mp3, ArchiveStreamContext* streamCtx, LONG seekMs)
{
    if (!mp3 || !streamCtx)
        return false;

    if (seekMs < 0)
        seekMs = 0;

    // Close old decoder/context.
    drmp3_uninit(mp3);
    CloseStreamingMp3Context(streamCtx);

    memset(mp3, 0, sizeof(*mp3));
    memset(streamCtx, 0, sizeof(*streamCtx));

    if (!OpenStreamingMp3Context(streamCtx, g_currentArchivePath, g_currentArchiveIndex))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=MP3 | archive=%s | index=%u | seek_ms=%ld | reason=open_stream_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    if (!drmp3_init(mp3, StreamingMp3Read, NULL, NULL, NULL, streamCtx, NULL))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=MP3 | archive=%s | index=%u | seek_ms=%ld | reason=decoder_init_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        CloseStreamingMp3Context(streamCtx);
        return false;
    }

    if (seekMs == 0)
        return true;

    // Slow seek: decode and discard until target frame.
    if (mp3->sampleRate == 0)
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=MP3 | archive=%s | index=%u | seek_ms=%ld | reason=zero_sample_rate", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    drmp3_uint64 targetFrame = ((drmp3_uint64)seekMs * (drmp3_uint64)mp3->sampleRate) / 1000;
    const drmp3_uint64 framesPerChunk = 1152 * 16;
    const size_t maxSamples = (size_t)framesPerChunk * mp3->channels;

    short* discard = (short*)malloc(maxSamples * sizeof(short));

    if (!discard)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=MP3 | seek_ms=%ld | samples=%zu | reason=seek_discard_buffer", seekMs, maxSamples);
        return false;
    }

    drmp3_uint64 framesSkipped = 0;

    while (framesSkipped < targetFrame && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        drmp3_uint64 framesWanted = targetFrame - framesSkipped;

        if (framesWanted > framesPerChunk)
            framesWanted = framesPerChunk;

        drmp3_uint64 framesRead = drmp3_read_pcm_frames_s16(mp3, framesWanted, discard);

        if (framesRead == 0)
            break;

        framesSkipped += framesRead;
    }

    free(discard);

    if (framesSkipped == 0 && seekMs != 0)
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=MP3 | archive=%s | index=%u | seek_ms=%ld | reason=no_frames_skipped", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);

    return framesSkipped > 0 || seekMs == 0;
}

static bool RestartStreamingWavAtMs(drwav* wav, ArchiveStreamContext* streamCtx, LONG seekMs)
{
    if (!wav || !streamCtx)
        return false;

    if (seekMs < 0)
        seekMs = 0;

    drwav_uninit(wav);
    CloseStreamingMp3Context(streamCtx);

    memset(wav, 0, sizeof(*wav));
    memset(streamCtx, 0, sizeof(*streamCtx));

    if (!OpenStreamingMp3Context(streamCtx, g_currentArchivePath, g_currentArchiveIndex))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=WAV | archive=%s | index=%u | seek_ms=%ld | reason=open_stream_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    if (!drwav_init(wav, StreamingWavRead, StreamingWavSeek, StreamingWavTell, streamCtx, NULL))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=WAV | archive=%s | index=%u | seek_ms=%ld | reason=decoder_init_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        CloseStreamingMp3Context(streamCtx);
        return false;
    }

    if (seekMs == 0)
        return true;

    if (wav->sampleRate == 0)
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=WAV | archive=%s | index=%u | seek_ms=%ld | reason=zero_sample_rate", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    drwav_uint64 targetFrame = ((drwav_uint64)seekMs * (drwav_uint64)wav->sampleRate) / 1000;
    const drwav_uint64 framesPerChunk = 4096;
    const size_t maxSamples = (size_t)framesPerChunk * wav->channels;

    short* discard = (short*)malloc(maxSamples * sizeof(short));

    if (!discard)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=WAV | seek_ms=%ld | samples=%zu | reason=seek_discard_buffer", seekMs, maxSamples);
        return false;
    }

    drwav_uint64 framesSkipped = 0;

    while (framesSkipped < targetFrame && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        drwav_uint64 framesWanted = targetFrame - framesSkipped;

        if (framesWanted > framesPerChunk)
            framesWanted = framesPerChunk;

        drwav_uint64 framesRead = drwav_read_pcm_frames_s16(wav, framesWanted, discard);

        if (framesRead == 0)
            break;

        framesSkipped += framesRead;
    }

    free(discard);

    if (framesSkipped == 0 && seekMs != 0)
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=WAV | archive=%s | index=%u | seek_ms=%ld | reason=no_frames_skipped", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);

    return framesSkipped > 0 || seekMs == 0;
}

static bool RestartStreamingFlacAtMs(drflac** flac, ArchiveStreamContext* streamCtx, LONG seekMs)
{
    if (!flac || !streamCtx)
        return false;

    if (seekMs < 0)
        seekMs = 0;

    if (*flac)
    {
        drflac_close(*flac);
        *flac = NULL;
    }

    CloseStreamingMp3Context(streamCtx);
    memset(streamCtx, 0, sizeof(*streamCtx));

    if (!OpenStreamingMp3Context(streamCtx, g_currentArchivePath, g_currentArchiveIndex))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=FLAC | archive=%s | index=%u | seek_ms=%ld | reason=open_stream_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    *flac = drflac_open(StreamingFlacRead, StreamingFlacSeek, StreamingFlacTell, streamCtx, NULL);

    if (!*flac)
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=FLAC | archive=%s | index=%u | seek_ms=%ld | reason=decoder_open_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        CloseStreamingMp3Context(streamCtx);
        return false;
    }

    if (seekMs == 0)
        return true;

    if ((*flac)->sampleRate == 0)
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=FLAC | archive=%s | index=%u | seek_ms=%ld | reason=zero_sample_rate", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    drflac_uint64 targetFrame = ((drflac_uint64)seekMs * (drflac_uint64)(*flac)->sampleRate) / 1000;
    const drflac_uint64 framesPerChunk = 4096;
    const size_t maxSamples = (size_t)framesPerChunk * (*flac)->channels;

    short* discard = (short*)malloc(maxSamples * sizeof(short));

    if (!discard)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=FLAC | seek_ms=%ld | samples=%zu | reason=seek_discard_buffer", seekMs, maxSamples);
        return false;
    }

    drflac_uint64 framesSkipped = 0;

    while (framesSkipped < targetFrame && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        drflac_uint64 framesWanted = targetFrame - framesSkipped;

        if (framesWanted > framesPerChunk)
            framesWanted = framesPerChunk;

        drflac_uint64 framesRead = drflac_read_pcm_frames_s16(*flac, framesWanted, discard);

        if (framesRead == 0)
            break;

        framesSkipped += framesRead;
    }

    free(discard);

    if (framesSkipped == 0 && seekMs != 0)
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=FLAC | archive=%s | index=%u | seek_ms=%ld | reason=no_frames_skipped", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);

    return framesSkipped > 0 || seekMs == 0;
}

static void CloseOggPushStream(OggPushStream* ctx)
{
    if (!ctx)
        return;

    if (ctx->vorbis)
    {
        stb_vorbis_close(ctx->vorbis);
        ctx->vorbis = NULL;
    }

    CloseStreamingMp3Context(&ctx->archive);

    ctx->buffer.clear();
    ctx->archiveOpen = false;
    ctx->eof = false;
    ctx->channels = 0;
    ctx->sampleRate = 0;
}

static bool OpenOggPushStream(OggPushStream* ctx, const char* zipPath, mz_uint zipIndex)
{
    if (!ctx || !zipPath || !zipPath[0])
    {
        DebugLogWrite("ERROR", "OGG", "PUSH_OPEN_FAIL", "archive=%s | index=%u | reason=invalid_input", zipPath ? zipPath : "", (unsigned int)zipIndex);
        return false;
    }

    memset(ctx, 0, sizeof(*ctx));

    if (!OpenStreamingMp3Context(&ctx->archive, zipPath, zipIndex))
    {
        DebugLogWrite("WARN", "OGG", "PUSH_OPEN_FAIL", "archive=%s | index=%u | reason=open_archive_stream_failed", zipPath, (unsigned int)zipIndex);
        return false;
    }

    ctx->archiveOpen = true;
    ctx->eof = false;

    // Need enough data for Vorbis headers.
    if (!FillOggPushBuffer(ctx, 65536))
    {
        DebugLogWrite("WARN", "OGG", "PUSH_OPEN_FAIL", "archive=%s | index=%u | reason=initial_buffer_fill_failed", zipPath, (unsigned int)zipIndex);
        CloseOggPushStream(ctx);
        return false;
    }

    int bytesConsumed = 0;
    int error = 0;

    ctx->vorbis = stb_vorbis_open_pushdata(ctx->buffer.data(), (int)ctx->buffer.size(), &bytesConsumed, &error, NULL);

    if (!ctx->vorbis)
    {
        DebugLogWrite("WARN", "OGG", "PUSH_OPEN_FAIL", "archive=%s | index=%u | stb_error=%d | buffered=%zu | reason=open_pushdata_failed", zipPath, (unsigned int)zipIndex, error, ctx->buffer.size());
        CloseOggPushStream(ctx);
        return false;
    }

    if (bytesConsumed > 0)
        ctx->buffer.erase(ctx->buffer.begin(), ctx->buffer.begin() + bytesConsumed);

    stb_vorbis_info info = stb_vorbis_get_info(ctx->vorbis);

    ctx->channels = info.channels;
    ctx->sampleRate = info.sample_rate;

    if (ctx->channels <= 0 || ctx->sampleRate <= 0)
    {
        DebugLogWrite("WARN", "OGG", "PUSH_OPEN_FAIL", "archive=%s | index=%u | channels=%d | sample_rate=%d | reason=invalid_stream_info", zipPath, (unsigned int)zipIndex, ctx->channels, ctx->sampleRate);
        CloseOggPushStream(ctx);
        return false;
    }

    return true;
}

static short FloatToS16(float v)
{
    if (v > 1.0f)
        v = 1.0f;

    if (v < -1.0f)
        v = -1.0f;

    return (short)(v * 32767.0f);
}

static int ReadOggPushFrameS16(OggPushStream* ctx, short* pcmOut, int maxFrames)
{
    if (!ctx || !ctx->vorbis || !pcmOut || maxFrames <= 0)
        return 0;

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        if (ctx->buffer.empty() && !ctx->eof)
            FillOggPushBuffer(ctx, 32768);

        if (ctx->buffer.empty() && ctx->eof)
            return 0;

        int channels = 0;
        float** output = NULL;
        int samples = 0;

        int bytesUsed = stb_vorbis_decode_frame_pushdata(ctx->vorbis, ctx->buffer.data(), (int)ctx->buffer.size(), &channels, &output, &samples);

        if (bytesUsed > 0)
            ctx->buffer.erase(ctx->buffer.begin(), ctx->buffer.begin() + bytesUsed);

        if (samples > 0 && output && channels > 0)
        {
            if (samples > maxFrames)
                samples = maxFrames;

            for (int i = 0; i < samples; ++i)
            {
                for (int ch = 0; ch < ctx->channels; ++ch)
                {
                    float sample = 0.0f;

                    if (ch < channels)
                        sample = output[ch][i];

                    pcmOut[(i * ctx->channels) + ch] = FloatToS16(sample);
                }
            }

            return samples;
        }

        if (ctx->eof && bytesUsed == 0)
            return 0;

        if (!ctx->eof)
        {
            size_t before = ctx->buffer.size();
            FillOggPushBuffer(ctx, before + 32768);

            if (ctx->buffer.size() == before && !ctx->eof)
                Sleep(1);
        }
    }

    return 0;
}

static bool RestartStreamingOggAtMs(OggPushStream* ctx, LONG seekMs)
{
    if (!ctx)
        return false;

    if (seekMs < 0)
        seekMs = 0;

    CloseOggPushStream(ctx);

    if (!OpenOggPushStream(ctx, g_currentArchivePath, g_currentArchiveIndex))
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=OGG | archive=%s | index=%u | seek_ms=%ld | reason=open_push_stream_failed", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    if (seekMs == 0)
        return true;

    if (ctx->sampleRate <= 0)
    {
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=OGG | archive=%s | index=%u | seek_ms=%ld | reason=zero_sample_rate", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);
        return false;
    }

    unsigned int targetSample = (unsigned int)(((unsigned long long)seekMs * (unsigned long long)ctx->sampleRate) / 1000ULL);
    const int framesPerChunk = 4096;
    const int maxSamples = framesPerChunk * ctx->channels;

    short* discard = (short*)malloc(maxSamples * sizeof(short));

    if (!discard)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=OGG | seek_ms=%ld | samples=%d | reason=seek_discard_buffer", seekMs, maxSamples);
        return false;
    }

    unsigned int skipped = 0;

    while (skipped < targetSample && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        int framesWanted = (int)(targetSample - skipped);

        if (framesWanted > framesPerChunk)
            framesWanted = framesPerChunk;

        int framesRead = ReadOggPushFrameS16(ctx, discard, framesWanted);

        if (framesRead <= 0)
            break;

        skipped += (unsigned int)framesRead;
    }

    free(discard);

    if (skipped == 0 && seekMs != 0)
        DebugLogWrite("WARN", "PLAYBACK", "SEEK_RESTART_FAIL", "decoder=OGG | archive=%s | index=%u | seek_ms=%ld | reason=no_frames_skipped", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, seekMs);

    return skipped > 0 || seekMs == 0;
}

bool TestDecodeWavFromMemory(const unsigned char* wavData, size_t wavDataSize, unsigned int* outChannels, unsigned int* outSampleRate, drwav_uint64* outTotalPCMFrames, drwav_uint64* outDecodedTestFrames)
{
    if (!wavData || wavDataSize == 0)
        return false;

    if (outChannels)
        *outChannels = 0;

    if (outSampleRate)
        *outSampleRate = 0;

    if (outTotalPCMFrames)
        *outTotalPCMFrames = 0;

    if (outDecodedTestFrames)
        *outDecodedTestFrames = 0;

    drwav wav;

    if (!drwav_init_memory(&wav, wavData, wavDataSize, NULL))
        return false;

    if (outChannels)
        *outChannels = wav.channels;

    if (outSampleRate)
        *outSampleRate = wav.sampleRate;

    if (outTotalPCMFrames)
        *outTotalPCMFrames = wav.totalPCMFrameCount;

    const drwav_uint64 framesToRead = 1024;
    const size_t maxSamples = (size_t)framesToRead * wav.channels;

    float* pcmTestBuffer = (float*)malloc(maxSamples * sizeof(float));

    if (!pcmTestBuffer)
    {
        drwav_uninit(&wav);
        return false;
    }

    drwav_uint64 framesDecoded = drwav_read_pcm_frames_f32(&wav, framesToRead, pcmTestBuffer);

    free(pcmTestBuffer);
    drwav_uninit(&wav);

    if (outDecodedTestFrames)
        *outDecodedTestFrames = framesDecoded;

    return framesDecoded > 0;
}

bool TestDecodeOggFromMemory(const unsigned char* oggData, size_t oggDataSize, unsigned int* outChannels, unsigned int* outSampleRate, unsigned int* outTotalSamples, unsigned int* outDecodedTestSamples)
{
    if (!oggData || oggDataSize == 0)
        return false;

    if (outChannels)
        *outChannels = 0;

    if (outSampleRate)
        *outSampleRate = 0;

    if (outTotalSamples)
        *outTotalSamples = 0;

    if (outDecodedTestSamples)
        *outDecodedTestSamples = 0;

    int error = 0;
    stb_vorbis* ogg = stb_vorbis_open_memory(oggData, (int)oggDataSize, &error, NULL);

    if (!ogg)
        return false;

    stb_vorbis_info info = stb_vorbis_get_info(ogg);

    if (outChannels)
        *outChannels = info.channels;

    if (outSampleRate)
        *outSampleRate = info.sample_rate;

    unsigned int totalSamples = stb_vorbis_stream_length_in_samples(ogg);

    if (outTotalSamples)
        *outTotalSamples = totalSamples;

    const int maxFrames = 1024;
    const int maxSamples = maxFrames * info.channels;

    short* pcmTestBuffer = (short*)malloc(maxSamples * sizeof(short));

    if (!pcmTestBuffer)
    {
        stb_vorbis_close(ogg);
        return false;
    }

    int framesDecoded = stb_vorbis_get_samples_short_interleaved(ogg, info.channels, pcmTestBuffer, maxSamples);

    free(pcmTestBuffer);
    stb_vorbis_close(ogg);

    if (outDecodedTestSamples)
        *outDecodedTestSamples = (unsigned int)framesDecoded;

    return framesDecoded > 0;
}

bool TestDecodeFlacFromMemory(const unsigned char* flacData, size_t flacDataSize, unsigned int* outChannels, unsigned int* outSampleRate, drflac_uint64* outTotalPCMFrames, drflac_uint64* outDecodedTestFrames)
{
    if (!flacData || flacDataSize == 0)
        return false;

    if (outChannels)
        *outChannels = 0;

    if (outSampleRate)
        *outSampleRate = 0;

    if (outTotalPCMFrames)
        *outTotalPCMFrames = 0;

    if (outDecodedTestFrames)
        *outDecodedTestFrames = 0;

    drflac* flac = drflac_open_memory(flacData, flacDataSize, NULL);

    if (!flac)
        return false;

    if (outChannels)
        *outChannels = flac->channels;

    if (outSampleRate)
        *outSampleRate = flac->sampleRate;

    if (outTotalPCMFrames)
        *outTotalPCMFrames = flac->totalPCMFrameCount;

    const drflac_uint64 framesToRead = 1024;
    const size_t maxSamples = (size_t)framesToRead * flac->channels;

    short* pcmTestBuffer = (short*)malloc(maxSamples * sizeof(short));

    if (!pcmTestBuffer)
    {
        drflac_close(flac);
        return false;
    }

    drflac_uint64 framesDecoded = drflac_read_pcm_frames_s16(flac, framesToRead, pcmTestBuffer);

    free(pcmTestBuffer);
    drflac_close(flac);

    if (outDecodedTestFrames)
        *outDecodedTestFrames = framesDecoded;

    return framesDecoded > 0;
}

bool TestDecodeMp3FromMemory(const unsigned char* mp3Data, size_t mp3DataSize, unsigned int* outChannels, unsigned int* outSampleRate, drmp3_uint64* outTotalPCMFrames, drmp3_uint64* outDecodedTestFrames)
{
    if (!mp3Data || mp3DataSize == 0)
        return false;

    if (outChannels)
        *outChannels = 0;

    if (outSampleRate)
        *outSampleRate = 0;

    if (outTotalPCMFrames)
        *outTotalPCMFrames = 0;

    if (outDecodedTestFrames)
        *outDecodedTestFrames = 0;

    drmp3 mp3;

    if (!drmp3_init_memory(&mp3, mp3Data, mp3DataSize, NULL))
        return false;

    if (outChannels)
        *outChannels = mp3.channels;

    if (outSampleRate)
        *outSampleRate = mp3.sampleRate;

    drmp3_uint64 totalFrames = drmp3_get_pcm_frame_count(&mp3);

    if (outTotalPCMFrames)
        *outTotalPCMFrames = totalFrames;

    float pcmTestBuffer[4096];
    drmp3_uint64 framesDecoded = drmp3_read_pcm_frames_f32(&mp3, 1024, pcmTestBuffer);

    if (outDecodedTestFrames)
        *outDecodedTestFrames = framesDecoded;

    drmp3_uninit(&mp3);

    return framesDecoded > 0;
}

DWORD WINAPI PlaybackThreadProc(LPVOID)
{
    bool endedNaturally = false;
    InterlockedExchange(&g_isPlaying, 1);

    AudioFormat format = (AudioFormat)InterlockedCompareExchange(&g_currentAudioFormat, AUDIO_FORMAT_UNKNOWN, AUDIO_FORMAT_UNKNOWN);
    ArchivePlaybackMode playbackModeAtEntry = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);
    DebugLogWrite("INFO", "PLAYBACK", "THREAD_ENTER", "archive=%s | index=%u | format=%d | mode=%ld | cache=%s | size=%zu", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, (int)format, (long)playbackModeAtEntry, g_currentCachePath, g_currentAudioSize);

    if (MfIsNativeMediaFormat(format))
    {
        ArchivePlaybackMode playbackMode = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);

        MfAudioDecoder* mf = nullptr;
        bool mfOk = false;

        if (playbackMode == ARCHIVE_PLAYBACK_CACHE_FILE)
            mfOk = MfOpenCacheFileDecoder(g_currentCachePath, format, &mf);
        else
            mfOk = MfOpenArchiveDecoder(g_currentArchivePath, g_currentArchiveIndex, (mz_uint64)g_currentAudioSize, format, &mf);

        if (!mfOk || !mf)
        {
            DebugLogWrite("ERROR", "MF", "OPEN_FAIL", "mode=%ld | archive=%s | index=%u | cache=%s | format=%d", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, (int)format);

            const HRESULT startupHr = MfGetStartupResult();

            if (FAILED(startupHr))
            {
                char message[640];
                sprintf_s(message, sizeof(message), "Media Foundation could not be started.\n\nM4A, AAC and WMA playback require Windows Media Foundation.\nOn Windows N, install the Microsoft Media Feature Pack.\n\nHRESULT: 0x%08lX", (unsigned long)startupHr);
                MessageBoxA(g_mod.hMainWindow, message, "Archive Stream - Media Foundation Required", MB_OK | MB_ICONWARNING);
            }
            else
            {
                MessageBoxA(g_mod.hMainWindow, "Media Foundation could not open this archived audio file.\n\nThis file or codec may not be supported by the installed Windows Media Foundation components.", "Archive Stream - Media Foundation Decode Error", MB_OK | MB_ICONWARNING);
            }

            if (mf)
                MfCloseDecoder(mf);

            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        int sampleRate = MfGetSampleRate(mf);
        int channels = MfGetChannels(mf);
        int bitsPerSample = MfGetBitsPerSample(mf);
        int lengthMs = MfGetLengthMs(mf);

        if (bitsPerSample <= 0)
            bitsPerSample = 16;

        if (lengthMs > 0)
            g_currentLengthMs = lengthMs;

        g_currentSampleRate = sampleRate;
        g_currentChannels = channels;

        if (!g_mod.outMod || sampleRate <= 0 || channels <= 0)
        {
            DebugLogWrite("ERROR", "MF", "INVALID_OUTPUT_FORMAT", "sample_rate=%d | channels=%d | bits=%d | length_ms=%d", sampleRate, channels, bitsPerSample, lengthMs);

            MfCloseDecoder(mf);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        int openResult = g_mod.outMod->Open(sampleRate, channels, bitsPerSample, -1, -1);

        if (openResult < 0)
        {
            DebugLogWrite("ERROR", "OUTPUT", "OPEN_FAIL", "decoder=MF | sample_rate=%d | channels=%d | bits=%d | result=%d", sampleRate, channels, bitsPerSample, openResult);

            MfCloseDecoder(mf);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        g_mod.outMod->SetVolume(-666);
        g_mod.outMod->SetPan(0);

        if (g_mod.SetInfo)
            g_mod.SetInfo(g_currentBitrateKbps, sampleRate / 1000, channels, 1);

        InterlockedExchange(&g_outputTimeBaseMs, 0);
        InterlockedExchange(&g_trackStartTick, GetTickCount());

        const size_t pcmBufferBytes = 64 * 1024;
        unsigned char* pcm = (unsigned char*)malloc(pcmBufferBytes);

        if (!pcm)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=MF | bytes=%zu", pcmBufferBytes);

            if (g_mod.outMod)
                g_mod.outMod->Close();

            MfCloseDecoder(mf);
            InterlockedExchange(&g_isPlaying, 0);

            return 0;
        }

        endedNaturally = false;

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
        {
            if (InterlockedCompareExchange(&g_seekRequested, 0, 0) != 0)
            {
                LONG seekMs = InterlockedCompareExchange(&g_seekTargetMs, 0, 0);

                if (seekMs < 0)
                    seekMs = 0;

                if (g_currentLengthMs > 0 && seekMs > g_currentLengthMs)
                    seekMs = g_currentLengthMs;

                InterlockedExchange(&g_seekRequested, 0);

                if (MfSeekMs(mf, seekMs))
                {
                    if (g_mod.outMod)
                        g_mod.outMod->Flush(seekMs);

                    InterlockedExchange(&g_outputTimeBaseMs, seekMs);
                    InterlockedExchange(&g_trackStartTick, GetTickCount());
                }
                else
                {
                    DebugLogWrite("WARN", "PLAYBACK", "SEEK_FAIL", "decoder=MF | seek_ms=%ld", seekMs);
                }
            }

            size_t bytesRead = MfReadPcm(mf, pcm, pcmBufferBytes);

            if (bytesRead == 0)
            {
                endedNaturally = true;
                break;
            }

            int bytesToWrite = (int)bytesRead;
            char* writePtr = (char*)pcm;

            while (bytesToWrite > 0 &&
                InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_seekRequested, 0, 0) == 0)
            {
                int canWrite = g_mod.outMod->CanWrite();

                if (canWrite <= 0)
                {
                    Sleep(5);
                    continue;
                }

                int thisWrite = bytesToWrite;

                if (thisWrite > canWrite)
                    thisWrite = canWrite;

                g_mod.outMod->Write(writePtr, thisWrite);

                writePtr += thisWrite;
                bytesToWrite -= thisWrite;
            }
        }

        free(pcm);

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.outMod && g_mod.outMod->IsPlaying())
            Sleep(10);

        if (g_mod.outMod)
            g_mod.outMod->Close();

        MfCloseDecoder(mf);
        InterlockedExchange(&g_isPlaying, 0);

        DebugLogWrite("INFO", "PLAYBACK", "THREAD_EXIT", "decoder=MP3 | archive=%s | index=%u | cache=%s | ended_naturally=%d | stop_requested=%ld", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, endedNaturally ? 1 : 0, InterlockedCompareExchange(&g_stopRequested, 0, 0));

        if (endedNaturally && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.hMainWindow)
            PostMessage(g_mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);

        return 0;
    }

    if (format == AUDIO_FORMAT_WAV)
    {
        ArchivePlaybackMode playbackMode = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);

        bool usingStreaming = playbackMode == ARCHIVE_PLAYBACK_STREAM;
        bool usingCacheFile = playbackMode == ARCHIVE_PLAYBACK_CACHE_FILE;

        ArchiveStreamContext streamCtx;
        memset(&streamCtx, 0, sizeof(streamCtx));

        drwav wav;
        memset(&wav, 0, sizeof(wav));

        bool wavOk = false;

        if (usingStreaming)
        {
            if (OpenStreamingMp3Context(&streamCtx, g_currentArchivePath, g_currentArchiveIndex))
                wavOk = drwav_init(&wav, StreamingWavRead, StreamingWavSeek, StreamingWavTell, &streamCtx, NULL) ? true : false;
        }
        else if (usingCacheFile)
        {
            wavOk = drwav_init_file(&wav, g_currentCachePath, NULL) ? true : false;
        }
        else
        {
            wavOk = drwav_init_memory(&wav, g_audioBuffer, g_currentAudioSize, NULL) ? true : false;
        }

        if (!wavOk)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "DECODER_OPEN_FAIL", "decoder=WAV | mode=%ld | archive=%s | index=%u | cache=%s | size=%zu", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, g_currentAudioSize);

            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        const unsigned int channels = wav.channels;
        const unsigned int sampleRate = wav.sampleRate;
        const int bitsPerSample = 16;

        if (!g_mod.outMod)
        {
            DebugLogWrite("ERROR", "OUTPUT", "MISSING", "decoder=WAV");

            drwav_uninit(&wav);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        int openResult = g_mod.outMod->Open(sampleRate, channels, bitsPerSample, -1, -1);

        if (openResult < 0)
        {
            DebugLogWrite("ERROR", "OUTPUT", "OPEN_FAIL", "decoder=WAV | sample_rate=%u | channels=%u | bits=%d | result=%d", sampleRate, channels, bitsPerSample, openResult);

            drwav_uninit(&wav);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        g_mod.outMod->SetVolume(-666);
        g_mod.outMod->SetPan(0);

        if (g_mod.SetInfo)
            g_mod.SetInfo(g_currentBitrateKbps, sampleRate / 1000, channels, 1);

        InterlockedExchange(&g_outputTimeBaseMs, 0);
        InterlockedExchange(&g_trackStartTick, GetTickCount());

        const drwav_uint64 framesPerChunk = 4096;
        const size_t maxSamples = (size_t)framesPerChunk * channels;

        short* pcm = (short*)malloc(maxSamples * sizeof(short));

        if (!pcm)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=WAV | samples=%zu", maxSamples);

            if (g_mod.outMod)
                g_mod.outMod->Close();

            drwav_uninit(&wav);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        bool endedNaturally = false;

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
        {
            if (InterlockedCompareExchange(&g_seekRequested, 0, 0) != 0)
            {
                LONG seekMs = InterlockedCompareExchange(&g_seekTargetMs, 0, 0);

                if (seekMs < 0)
                    seekMs = 0;

                if (g_currentLengthMs > 0 && seekMs > g_currentLengthMs)
                    seekMs = g_currentLengthMs;

                InterlockedExchange(&g_seekRequested, 0);

                bool seekOk = false;

                if (usingStreaming)
                    seekOk = RestartStreamingWavAtMs(&wav, &streamCtx, seekMs);
                else
                {
                    drwav_uint64 targetFrame = ((drwav_uint64)seekMs * (drwav_uint64)sampleRate) / 1000;
                    seekOk = drwav_seek_to_pcm_frame(&wav, targetFrame) ? true : false;
                }

                if (seekOk)
                {
                    if (g_mod.outMod)
                        g_mod.outMod->Flush(seekMs);

                    InterlockedExchange(&g_outputTimeBaseMs, seekMs);
                    InterlockedExchange(&g_trackStartTick, GetTickCount());
                    DebugLogWrite("INFO", "PLAYBACK", "SEEK_APPLIED", "decoder=WAV | seek_ms=%ld | playback_mode=%ld | archive=%s | index=%u", seekMs, InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex);
                }
                else
                {
                    DebugLogWrite("WARN", "PLAYBACK", "SEEK_FAIL", "decoder=WAV | mode=%ld | archive=%s | index=%u | cache=%s | seek_ms=%ld", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, seekMs);
                }
            }

            drwav_uint64 framesRead = drwav_read_pcm_frames_s16(&wav, framesPerChunk, pcm);

            if (framesRead == 0)
            {
                endedNaturally = true;
                break;
            }

            int bytesToWrite = (int)(framesRead * channels * sizeof(short));
            char* writePtr = (char*)pcm;

            while (bytesToWrite > 0 &&
                InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_seekRequested, 0, 0) == 0)
            {
                int canWrite = g_mod.outMod->CanWrite();

                if (canWrite <= 0)
                {
                    Sleep(5);
                    continue;
                }

                int thisWrite = bytesToWrite;

                if (thisWrite > canWrite)
                    thisWrite = canWrite;

                g_mod.outMod->Write(writePtr, thisWrite);

                writePtr += thisWrite;
                bytesToWrite -= thisWrite;
            }
        }

        free(pcm);

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.outMod && g_mod.outMod->IsPlaying())
            Sleep(10);

        if (g_mod.outMod)
            g_mod.outMod->Close();

        drwav_uninit(&wav);
        CloseStreamingMp3Context(&streamCtx);

        InterlockedExchange(&g_isPlaying, 0);

        if (endedNaturally && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.hMainWindow)
            PostMessage(g_mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);

        return 0;
    }

    if (format == AUDIO_FORMAT_FLAC)
    {
        ArchivePlaybackMode playbackMode = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);

        bool usingStreaming = playbackMode == ARCHIVE_PLAYBACK_STREAM;
        bool usingCacheFile = playbackMode == ARCHIVE_PLAYBACK_CACHE_FILE;

        ArchiveStreamContext streamCtx;
        memset(&streamCtx, 0, sizeof(streamCtx));

        drflac* flac = NULL;

        if (usingStreaming)
        {
            if (OpenStreamingMp3Context(&streamCtx, g_currentArchivePath, g_currentArchiveIndex))
                flac = drflac_open(StreamingFlacRead, StreamingFlacSeek, StreamingFlacTell, &streamCtx, NULL);
        }
        else if (usingCacheFile)
        {
            flac = drflac_open_file(g_currentCachePath, NULL);
        }
        else
        {
            flac = drflac_open_memory(g_audioBuffer, g_currentAudioSize, NULL);
        }

        if (!flac)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "DECODER_OPEN_FAIL", "decoder=FLAC | mode=%ld | archive=%s | index=%u | cache=%s | size=%zu", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, g_currentAudioSize);

            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        const unsigned int channels = flac->channels;
        const unsigned int sampleRate = flac->sampleRate;
        const int bitsPerSample = 16;

        if (!g_mod.outMod)
        {
            DebugLogWrite("ERROR", "OUTPUT", "MISSING", "decoder=FLAC");

            drflac_close(flac);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        int openResult = g_mod.outMod->Open(sampleRate, channels, bitsPerSample, -1, -1);

        if (openResult < 0)
        {
            DebugLogWrite("ERROR", "OUTPUT", "OPEN_FAIL", "decoder=FLAC | sample_rate=%u | channels=%u | bits=%d | result=%d", sampleRate, channels, bitsPerSample, openResult);

            drflac_close(flac);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        g_mod.outMod->SetVolume(-666);
        g_mod.outMod->SetPan(0);

        if (g_mod.SetInfo)
            g_mod.SetInfo(g_currentBitrateKbps, sampleRate / 1000, channels, 1);

        InterlockedExchange(&g_outputTimeBaseMs, 0);
        InterlockedExchange(&g_trackStartTick, GetTickCount());

        const drflac_uint64 framesPerChunk = 4096;
        const size_t maxSamples = (size_t)framesPerChunk * channels;

        short* pcm = (short*)malloc(maxSamples * sizeof(short));

        if (!pcm)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=FLAC | samples=%zu", maxSamples);

            if (g_mod.outMod)
                g_mod.outMod->Close();

            drflac_close(flac);
            CloseStreamingMp3Context(&streamCtx);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        bool endedNaturally = false;

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
        {
            if (InterlockedCompareExchange(&g_seekRequested, 0, 0) != 0)
            {
                LONG seekMs = InterlockedCompareExchange(&g_seekTargetMs, 0, 0);

                if (seekMs < 0)
                    seekMs = 0;

                if (g_currentLengthMs > 0 && seekMs > g_currentLengthMs)
                    seekMs = g_currentLengthMs;

                InterlockedExchange(&g_seekRequested, 0);

                bool seekOk = false;

                if (usingStreaming)
                    seekOk = RestartStreamingFlacAtMs(&flac, &streamCtx, seekMs);
                else
                {
                    drflac_uint64 targetFrame = ((drflac_uint64)seekMs * (drflac_uint64)sampleRate) / 1000;
                    seekOk = drflac_seek_to_pcm_frame(flac, targetFrame) ? true : false;
                }

                if (seekOk)
                {
                    if (g_mod.outMod)
                        g_mod.outMod->Flush(seekMs);

                    InterlockedExchange(&g_outputTimeBaseMs, seekMs);
                    InterlockedExchange(&g_trackStartTick, GetTickCount());
                }
                else
                {
                    DebugLogWrite("WARN", "PLAYBACK", "SEEK_FAIL", "decoder=FLAC | mode=%ld | archive=%s | index=%u | cache=%s | seek_ms=%ld", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, seekMs);
                }
            }

            drflac_uint64 framesRead = drflac_read_pcm_frames_s16(flac, framesPerChunk, pcm);

            if (framesRead == 0)
            {
                endedNaturally = true;
                break;
            }

            int bytesToWrite = (int)(framesRead * channels * sizeof(short));
            char* writePtr = (char*)pcm;

            while (bytesToWrite > 0 &&
                InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_seekRequested, 0, 0) == 0)
            {
                int canWrite = g_mod.outMod->CanWrite();

                if (canWrite <= 0)
                {
                    Sleep(5);
                    continue;
                }

                int thisWrite = bytesToWrite;

                if (thisWrite > canWrite)
                    thisWrite = canWrite;

                g_mod.outMod->Write(writePtr, thisWrite);

                writePtr += thisWrite;
                bytesToWrite -= thisWrite;
            }
        }

        free(pcm);

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.outMod && g_mod.outMod->IsPlaying())
            Sleep(10);

        if (g_mod.outMod)
            g_mod.outMod->Close();

        drflac_close(flac);
        CloseStreamingMp3Context(&streamCtx);

        InterlockedExchange(&g_isPlaying, 0);

        if (endedNaturally && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.hMainWindow)
            PostMessage(g_mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);

        return 0;
    }

    if (format == AUDIO_FORMAT_OGG)
    {
        ArchivePlaybackMode playbackMode = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);

        bool usingStreaming = playbackMode == ARCHIVE_PLAYBACK_STREAM;
        bool usingCacheFile = playbackMode == ARCHIVE_PLAYBACK_CACHE_FILE;

        stb_vorbis* ogg = NULL;
        OggPushStream oggStream;
        memset(&oggStream, 0, sizeof(oggStream));

        int channels = 0;
        int sampleRate = 0;
        bool oggOk = false;

        if (usingStreaming)
        {
            if (OpenOggPushStream(&oggStream, g_currentArchivePath, g_currentArchiveIndex))
            {
                channels = oggStream.channels;
                sampleRate = oggStream.sampleRate;
                oggOk = true;
            }
        }
        else if (usingCacheFile)
        {
            int error = 0;
            ogg = stb_vorbis_open_filename(g_currentCachePath, &error, NULL);

            if (ogg)
            {
                stb_vorbis_info info = stb_vorbis_get_info(ogg);
                channels = info.channels;
                sampleRate = info.sample_rate;
                oggOk = true;
            }
        }
        else
        {
            int error = 0;
            ogg = stb_vorbis_open_memory(g_audioBuffer, (int)g_currentAudioSize, &error, NULL);

            if (ogg)
            {
                stb_vorbis_info info = stb_vorbis_get_info(ogg);
                channels = info.channels;
                sampleRate = info.sample_rate;
                oggOk = true;
            }
        }

        if (!oggOk)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "DECODER_OPEN_FAIL", "decoder=OGG | mode=%ld | archive=%s | index=%u | cache=%s | size=%zu", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, g_currentAudioSize);

            if (ogg)
                stb_vorbis_close(ogg);

            CloseOggPushStream(&oggStream);

            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        const int bitsPerSample = 16;

        if (!g_mod.outMod)
        {
            DebugLogWrite("ERROR", "OUTPUT", "MISSING", "decoder=OGG");

            if (ogg)
                stb_vorbis_close(ogg);

            CloseOggPushStream(&oggStream);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        int openResult = g_mod.outMod->Open(sampleRate, channels, bitsPerSample, -1, -1);

        if (openResult < 0)
        {
            DebugLogWrite("ERROR", "OUTPUT", "OPEN_FAIL", "decoder=OGG | sample_rate=%d | channels=%d | bits=%d | result=%d", sampleRate, channels, bitsPerSample, openResult);

            if (ogg)
                stb_vorbis_close(ogg);

            CloseOggPushStream(&oggStream);
            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        g_mod.outMod->SetVolume(-666);
        g_mod.outMod->SetPan(0);

        if (g_mod.SetInfo)
            g_mod.SetInfo(g_currentBitrateKbps, sampleRate / 1000, channels, 1);

        InterlockedExchange(&g_outputTimeBaseMs, 0);
        InterlockedExchange(&g_trackStartTick, GetTickCount());

        const int framesPerChunk = 4096;
        const int maxSamples = framesPerChunk * channels;

        short* pcm = (short*)malloc(maxSamples * sizeof(short));

        if (!pcm)
        {
            DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=OGG | samples=%d", maxSamples);

            if (g_mod.outMod)
                g_mod.outMod->Close();

            if (ogg)
                stb_vorbis_close(ogg);

            CloseOggPushStream(&oggStream);

            InterlockedExchange(&g_isPlaying, 0);
            return 0;
        }

        bool endedNaturally = false;

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
        {
            if (InterlockedCompareExchange(&g_seekRequested, 0, 0) != 0)
            {
                LONG seekMs = InterlockedCompareExchange(&g_seekTargetMs, 0, 0);

                if (seekMs < 0)
                    seekMs = 0;

                if (g_currentLengthMs > 0 && seekMs > g_currentLengthMs)
                    seekMs = g_currentLengthMs;

                InterlockedExchange(&g_seekRequested, 0);

                bool seekOk = false;

                if (usingStreaming)
                    seekOk = RestartStreamingOggAtMs(&oggStream, seekMs);
                else
                {
                    unsigned int targetSample = sampleRate > 0 ? (unsigned int)(((unsigned long long)seekMs * (unsigned long long)sampleRate) / 1000ULL) : 0;
                    seekOk = stb_vorbis_seek(ogg, targetSample) ? true : false;
                }

                if (seekOk)
                {
                    if (g_mod.outMod)
                        g_mod.outMod->Flush(seekMs);

                    InterlockedExchange(&g_outputTimeBaseMs, seekMs);
                    InterlockedExchange(&g_trackStartTick, GetTickCount());
                }
                else
                {
                    DebugLogWrite("WARN", "PLAYBACK", "SEEK_FAIL", "decoder=OGG | mode=%ld | archive=%s | index=%u | cache=%s | seek_ms=%ld", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, seekMs);
                }
            }

            int framesRead = 0;

            if (usingStreaming)
                framesRead = ReadOggPushFrameS16(&oggStream, pcm, framesPerChunk);
            else
                framesRead = stb_vorbis_get_samples_short_interleaved(ogg, channels, pcm, maxSamples);

            if (framesRead == 0)
            {
                endedNaturally = true;
                break;
            }

            int bytesToWrite = framesRead * channels * sizeof(short);
            char* writePtr = (char*)pcm;

            while (bytesToWrite > 0 &&
                InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
                InterlockedCompareExchange(&g_seekRequested, 0, 0) == 0)
            {
                int canWrite = g_mod.outMod->CanWrite();

                if (canWrite <= 0)
                {
                    Sleep(5);
                    continue;
                }

                int thisWrite = bytesToWrite;

                if (thisWrite > canWrite)
                    thisWrite = canWrite;

                g_mod.outMod->Write(writePtr, thisWrite);

                writePtr += thisWrite;
                bytesToWrite -= thisWrite;
            }
        }

        free(pcm);

        while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.outMod && g_mod.outMod->IsPlaying())
            Sleep(10);

        if (g_mod.outMod)
            g_mod.outMod->Close();

        if (ogg)
            stb_vorbis_close(ogg);

        CloseOggPushStream(&oggStream);

        InterlockedExchange(&g_isPlaying, 0);

        if (endedNaturally && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.hMainWindow)
            PostMessage(g_mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);

        return 0;
    }

    // MP3

    drmp3 mp3;
    memset(&mp3, 0, sizeof(mp3));

    ArchiveStreamContext streamCtx;
    memset(&streamCtx, 0, sizeof(streamCtx));

    ArchivePlaybackMode playbackMode = (ArchivePlaybackMode)InterlockedCompareExchange(&g_playbackMode, 0, 0);

    bool usingStreaming = playbackMode == ARCHIVE_PLAYBACK_STREAM;
    bool usingCacheFile = playbackMode == ARCHIVE_PLAYBACK_CACHE_FILE;

    bool mp3Ok = false;

    if (usingStreaming)
    {
        if (OpenStreamingMp3Context(&streamCtx, g_currentArchivePath, g_currentArchiveIndex))
            mp3Ok = drmp3_init(&mp3, StreamingMp3Read, NULL, NULL, NULL, &streamCtx, NULL) ? true : false;
    }
    else if (usingCacheFile)
    {
        mp3Ok = drmp3_init_file(&mp3, g_currentCachePath, NULL) ? true : false;
    }
    else
    {
        mp3Ok = drmp3_init_memory(&mp3, g_audioBuffer, g_currentAudioSize, NULL) ? true : false;
    }

    DebugLogWrite("INFO", "PLAYBACK", "DECODER_OPEN_OK", "decoder=MP3 | mode=%ld | streaming=%d | cache_file=%d | archive=%s | index=%u | cache=%s | sample_rate=%u | channels=%u", InterlockedCompareExchange(&g_playbackMode, 0, 0), usingStreaming ? 1 : 0, usingCacheFile ? 1 : 0, g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, (unsigned int)mp3.sampleRate, (unsigned int)mp3.channels);

    if (!mp3Ok)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "DECODER_OPEN_FAIL", "decoder=MP3 | mode=%ld | archive=%s | index=%u | cache=%s | size=%zu", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, g_currentAudioSize);

        CloseStreamingMp3Context(&streamCtx);
        InterlockedExchange(&g_isPlaying, 0);
        return 0;
    }

    const unsigned int channels = mp3.channels;
    const unsigned int sampleRate = mp3.sampleRate;
    const int bitsPerSample = 16;

    if (!g_mod.outMod)
    {
        DebugLogWrite("ERROR", "OUTPUT", "MISSING", "decoder=MP3");

        drmp3_uninit(&mp3);
        CloseStreamingMp3Context(&streamCtx);
        InterlockedExchange(&g_isPlaying, 0);
        return 0;
    }

    // Open Winamp's currently selected output plugin.
    // bufferlenms/prebufferms are Winamp output buffer settings.
    DebugLogWrite("INFO", "OUTPUT", "OPEN_REQUEST", "decoder=MP3 | archive=%s | index=%u | cache=%s | sample_rate=%u | channels=%u | bits=%d", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, sampleRate, channels, bitsPerSample);

    int openResult = g_mod.outMod->Open(sampleRate, channels, bitsPerSample, -1, -1);

    if (openResult >= 0)
    {
        DebugLogWrite("INFO", "OUTPUT", "OPEN_OK", "decoder=MP3 | archive=%s | index=%u | cache=%s | sample_rate=%u | channels=%u | bits=%d | result=%d", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, sampleRate, channels, bitsPerSample, openResult);
    }

    if (openResult < 0)
    {
        DebugLogWrite("ERROR", "OUTPUT", "OPEN_FAIL", "decoder=MP3 | sample_rate=%u | channels=%u | bits=%d | result=%d", sampleRate, channels, bitsPerSample, openResult);

        drmp3_uninit(&mp3);
        CloseStreamingMp3Context(&streamCtx);
        InterlockedExchange(&g_isPlaying, 0);
        return 0;
    }

    g_mod.outMod->SetVolume(-666); // -666 means use current Winamp volume.
    g_mod.outMod->SetPan(0);

    InterlockedExchange(&g_outputTimeBaseMs, 0);
    InterlockedExchange(&g_trackStartTick, GetTickCount());

    if (g_mod.SetInfo)
        g_mod.SetInfo(g_currentBitrateKbps, sampleRate / 1000, channels, 1);

    const drmp3_uint64 framesPerChunk = 1152 * 8;
    const size_t maxSamples = (size_t)framesPerChunk * channels;
    bool firstOutputWriteLogged = false;
    short* pcm = (short*)malloc(maxSamples * sizeof(short));

    if (!pcm)
    {
        DebugLogWrite("ERROR", "PLAYBACK", "ALLOC_FAIL", "decoder=MP3 | samples=%zu", maxSamples);

        if (g_mod.outMod)
            g_mod.outMod->Close();

        drmp3_uninit(&mp3);
        CloseStreamingMp3Context(&streamCtx);
        InterlockedExchange(&g_isPlaying, 0);
        return 0;
    }

    endedNaturally = false;

    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0)
    {
        if (InterlockedCompareExchange(&g_seekRequested, 0, 0) != 0)
        {
            LONG seekMs = InterlockedCompareExchange(&g_seekTargetMs, 0, 0);

            if (seekMs < 0)
                seekMs = 0;

            if (g_currentLengthMs > 0 && seekMs > g_currentLengthMs)
                seekMs = g_currentLengthMs;

            // Clear request before doing work so it cannot loop forever on failure.
            InterlockedExchange(&g_seekRequested, 0);

            bool seekOk = false;

            if (usingStreaming)
                seekOk = RestartStreamingMp3AtMs(&mp3, &streamCtx, seekMs);
            else
            {
                drmp3_uint64 targetFrame = 0;

                if (mp3.sampleRate > 0)
                    targetFrame = ((drmp3_uint64)seekMs * (drmp3_uint64)mp3.sampleRate) / 1000;

                seekOk = drmp3_seek_to_pcm_frame(&mp3, targetFrame) ? true : false;
            }

            if (seekOk)
            {
                if (g_mod.outMod)
                    g_mod.outMod->Flush(seekMs);

                InterlockedExchange(&g_outputTimeBaseMs, seekMs);
                InterlockedExchange(&g_trackStartTick, GetTickCount());
            }
            else
            {
                DebugLogWrite("WARN", "PLAYBACK", "SEEK_FAIL", "decoder=MP3 | mode=%ld | archive=%s | index=%u | cache=%s | seek_ms=%ld", InterlockedCompareExchange(&g_playbackMode, 0, 0), g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, seekMs);
            }
        }

        drmp3_uint64 framesRead = drmp3_read_pcm_frames_s16(&mp3, framesPerChunk, pcm);

        if (framesRead == 0)
        {
            endedNaturally = true;
            break;
        }

        int bytesToWrite = (int)(framesRead * channels * sizeof(short));
        char* writePtr = (char*)pcm;

        while (bytesToWrite > 0 &&
            InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 &&
            InterlockedCompareExchange(&g_seekRequested, 0, 0) == 0)
        {
            int canWrite = g_mod.outMod->CanWrite();

            if (canWrite <= 0)
            {
                Sleep(5);
                continue;
            }

            int thisWrite = bytesToWrite;

            if (thisWrite > canWrite)
                thisWrite = canWrite;

            if (!firstOutputWriteLogged)
            {
                DebugLogWrite("INFO", "OUTPUT", "FIRST_WRITE", "decoder=MP3 | archive=%s | index=%u | cache=%s | bytes=%d", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, thisWrite);
                firstOutputWriteLogged = true;
            }

            g_mod.outMod->Write(writePtr, thisWrite);

            writePtr += thisWrite;
            bytesToWrite -= thisWrite;
        }
    }

    free(pcm);

    // Let output finish any already-buffered audio unless Stop() requested.
    while (InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.outMod->IsPlaying())
        Sleep(10);

    if (g_mod.outMod)
        g_mod.outMod->Close();

    drmp3_uninit(&mp3);
    CloseStreamingMp3Context(&streamCtx);

    InterlockedExchange(&g_isPlaying, 0);

    DebugLogWrite("INFO", "PLAYBACK", "THREAD_EXIT", "decoder=MP3 | archive=%s | index=%u | cache=%s | ended_naturally=%d | stop_requested=%ld", g_currentArchivePath, (unsigned int)g_currentArchiveIndex, g_currentCachePath, endedNaturally ? 1 : 0, InterlockedCompareExchange(&g_stopRequested, 0, 0));

    if (endedNaturally && InterlockedCompareExchange(&g_stopRequested, 0, 0) == 0 && g_mod.hMainWindow)
        PostMessage(g_mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);

    return 0;
}

void Pause()
{
    if (g_mod.outMod)
        g_mod.outMod->Pause(1);
}

void UnPause()
{
    if (g_mod.outMod)
        g_mod.outMod->Pause(0);
}

int IsPaused()
{
    if (g_mod.outMod)
        return g_mod.outMod->IsPlaying() == 3;

    return 0;
}

void Stop()
{
    InterlockedExchange(&g_stopRequested, 1);
    InterlockedExchange(&g_seekRequested, 0);
    InterlockedExchange(&g_seekTargetMs, 0);

    if (g_playThread)
    {
        DWORD waitResult = WaitForSingleObject(g_playThread, 5000);

        if (waitResult == WAIT_OBJECT_0)
        {
            CloseHandle(g_playThread);
            g_playThread = NULL;
        }
        else
        {
            DebugLogWrite("WARN", "PLAYBACK", "STOP_TIMEOUT", "wait_ms=5000 | reason=playback_thread_did_not_exit");

            // Do not close the handle or outMod if the thread did not exit.
            // The playback thread still owns output cleanup.
            return;
        }
    }

    InterlockedExchange(&g_isPlaying, 0);
}

int GetLength()
{
    return g_currentLengthMs;
}

int GetOutputTime()
{
    if (InterlockedCompareExchange(&g_isPlaying, 0, 0) == 0)
        return 0;

    LONG baseMs = InterlockedCompareExchange(&g_outputTimeBaseMs, 0, 0);
    LONG startTick = InterlockedCompareExchange(&g_trackStartTick, 0, 0);

    DWORD now = GetTickCount();
    int elapsed = baseMs + (int)(now - (DWORD)startTick);

    if (g_currentLengthMs > 0 && elapsed > g_currentLengthMs)
        elapsed = g_currentLengthMs;

    return elapsed;
}

void SetOutputTime(int time_in_ms)
{
    DebugLogWrite("INFO", "PLAYBACK", "SEEK_REQUEST", "requested_ms=%d | playing=%ld | current_ms=%d", time_in_ms, InterlockedCompareExchange(&g_isPlaying, 0, 0), GetOutputTime());
    if (time_in_ms < 0)
        time_in_ms = 0;

    if (g_currentLengthMs > 0 && time_in_ms > g_currentLengthMs)
        time_in_ms = g_currentLengthMs;

    InterlockedExchange(&g_seekTargetMs, time_in_ms);
    InterlockedExchange(&g_seekRequested, 1);
}

void SetVolume(int volume)
{
    if (g_mod.outMod)
        g_mod.outMod->SetVolume(volume);
}

void SetPan(int pan)
{
    if (g_mod.outMod)
        g_mod.outMod->SetPan(pan);
}

void EQSet(int on, char data[10], int preamp)
{
    // Equalizer handling can be added later.
}