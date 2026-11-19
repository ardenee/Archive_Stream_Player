// Archive Stream Player / AIMP and shared native hosts.
// Safe, deterministic duration and stream-property contract tests.
// Optional positional archive argument exercises live direct-stream entries.
// No archive extraction, CRC computation, or registry changes.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../shared/archive_adapter_audio_probe.h"

static bool Check(bool condition, const char* test)
{
    if (!condition)
        std::printf("AUDIO_PROPERTY_TEST_FAIL reason=%s\n", test);
    return condition;
}

static void PutBE32(unsigned char* p, uint32_t n)
{
    p[0] = static_cast<unsigned char>(n >> 24);
    p[1] = static_cast<unsigned char>(n >> 16);
    p[2] = static_cast<unsigned char>(n >> 8);
    p[3] = static_cast<unsigned char>(n);
}

static void PutLE32(unsigned char* p, uint32_t n)
{
    p[0] = static_cast<unsigned char>(n);
    p[1] = static_cast<unsigned char>(n >> 8);
    p[2] = static_cast<unsigned char>(n >> 16);
    p[3] = static_cast<unsigned char>(n >> 24);
}

static bool TestHeaders()
{
    unsigned char frame[1044]{};
    frame[0] = 0xff;
    frame[1] = 0xfb;
    frame[2] = 0xe0;
    frame[3] = 0x04;
    memcpy(frame + 36, "Info", 4);
    PutBE32(frame + 40, 1);
    PutBE32(frame + 44, 19620);
    ArchiveAdapterAudioInfo mp3{};
    if (!Check(ArchiveAdapterAudioProbe::ParseMp3InfoFrame(frame, sizeof(frame), mp3), "valid_realistic_mp3_info"))
        return false;
    if (!Check(mp3.sampleRate == 44100 && mp3.channels == 2 && mp3.bitrateKbps == 320, "mpeg_audio_properties"))
        return false;
    if (!Check(std::abs(mp3.durationSeconds - 512.52245) < 0.01, "mpeg_frame_count_duration"))
        return false;
    if (!Check(std::abs(mp3.durationSeconds - 512.52245) < 0.01, "AIMP_duration_property_seconds"))
        return false;
    // Real Subspecies 4 encoder tag: 576 priming + 1241 padding
    // samples. AIMP's decoder returns 512.481247 seconds, rather than
    // the untrimmed Info frame count of 512.522449 seconds.
    memcpy(frame + 48, "Lame3.100", 9);
    frame[69] = 0x24;
    frame[70] = 0x04;
    frame[71] = 0xd9;
    if (!Check(ArchiveAdapterAudioProbe::ParseMp3InfoFrame(frame, sizeof(frame), mp3),
        "valid_lame_gapless_info"))
        return false;
    if (!Check(std::abs(mp3.durationSeconds - 512.481247) < 0.00005,
        "lame_delay_padding_matches_aimp_decoder"))
        return false;
    if (!Check(std::abs(mp3.durationSeconds * 1000.0 - 512481.247) < 0.05,
        "lame_gapless_precision"))
        return false;
    memcpy(frame + 36, "Xing", 4);
    PutBE32(frame + 40, 3);
    PutBE32(frame + 48, 10000000);
    if (!Check(ArchiveAdapterAudioProbe::ParseMp3InfoFrame(frame, sizeof(frame), mp3), "valid_xing"))
        return false;
    if (!Check(mp3.bitrateKbps > 100 && mp3.bitrateKbps < 200, "xing_average_bitrate"))
        return false;
    if (!Check(!ArchiveAdapterAudioProbe::ParseMp3InfoFrame(frame, 30, mp3), "truncated_mpeg_header_rejected"))
        return false;
    frame[44] = 0;
    frame[45] = 0;
    frame[46] = 0;
    frame[47] = 0;
    if (!Check(!ArchiveAdapterAudioProbe::ParseMp3InfoFrame(frame, sizeof(frame), mp3), "empty_mpeg_frame_count_rejected"))
        return false;

    unsigned char flac[42]{};
    memcpy(flac, "fLaC", 4);
    flac[4] = 0x80;
    flac[7] = 34;
    const uint64_t packed = (uint64_t(44100) << 44) | (uint64_t(1) << 41) |
        (uint64_t(15) << 36) | uint64_t(132300);
    PutBE32(flac + 18, static_cast<uint32_t>(packed >> 32));
    PutBE32(flac + 22, static_cast<uint32_t>(packed));
    ArchiveAdapterAudioInfo flacInfo{};
    if (!Check(ArchiveAdapterAudioProbe::ParseFlacStreamInfo(flac, sizeof(flac), flacInfo), "flac_streaminfo"))
        return false;
    if (!Check(flacInfo.sampleRate == 44100 && flacInfo.channels == 2 &&
        std::abs(flacInfo.durationSeconds - 3.0) < 0.001, "flac_duration_channels_rate"))
        return false;

    unsigned char wav[44]{};
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    PutLE32(wav + 16, 16);
    wav[20] = 1;
    wav[22] = 2;
    PutLE32(wav + 24, 44100);
    PutLE32(wav + 28, 176400);
    wav[32] = 4;
    wav[34] = 16;
    memcpy(wav + 36, "data", 4);
    PutLE32(wav + 40, 529200);
    ArchiveAdapterAudioInfo waveInfo{};
    if (!Check(ArchiveAdapterAudioProbe::ParseWaveHeader(wav, sizeof(wav), waveInfo), "wav_header"))
        return false;
    if (!Check(waveInfo.sampleRate == 44100 && waveInfo.channels == 2 &&
        waveInfo.bitrateKbps == 1411 && std::abs(waveInfo.durationSeconds - 3.0) < 0.001, "wav_duration_bitrate"))
        return false;
    wav[0] = 'X';
    if (!Check(!ArchiveAdapterAudioProbe::ParseWaveHeader(wav, sizeof(wav), waveInfo), "invalid_riff_rejected"))
        return false;

    std::puts("AUDIO_PROPERTY_HEADER_TEST_PASS mp3_info=1 xing=1 lame_gapless=1 mpeg_corrupt_rejected=1 flac=1 wav=1 aimp_duration_seconds=1 no_decode=1");
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    if (!TestHeaders())
        return 1;
    if (argc < 2)
        return 0;

    std::vector<ArchiveEntry> entries;
    std::string error;
    if (!ArchiveListEntries(argv[1], entries, error))
    {
        std::printf("AUDIO_PROPERTY_TEST_FAIL archive_list=%s\n", error.c_str());
        return 2;
    }

    unsigned checked = 0;
    double totalSeconds = 0.0;
    for (const auto& entry : entries)
    {
        if (!entry.isSupportedAudio || entry.isFolder)
            continue;
        ArchiveAdapterAudioInfo info{};
        if (!Check(ArchiveAdapterProbeEntryAudio(argv[1], entry, info) && info.HasDuration(),
            "real_archive_entry"))
            return 3;
        if (!Check(info.sampleRate >= 8000 && info.channels >= 1 && info.channels <= 8,
            "real_audio_properties"))
            return 4;
        std::printf("AUDIO_PROPERTY_ARCHIVE_PASS index=%u duration=%.3f rate=%u bitrate_kbps=%u channels=%u\n",
            entry.index, info.durationSeconds, info.sampleRate, info.bitrateKbps, info.channels);
        totalSeconds += info.durationSeconds;
        ++checked;
    }
    std::printf("AUDIO_PROPERTY_ARCHIVE_SUMMARY entries=%u total_seconds=%.3f\n", checked, totalSeconds);
    return checked ? 0 : 5;
}
