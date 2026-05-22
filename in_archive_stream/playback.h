#pragma once

#include <windows.h>
#include <cstddef>

#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"

DWORD WINAPI PlaybackThreadProc(LPVOID);

void Pause();
void UnPause();
int IsPaused();
void Stop();

int GetLength();
int GetOutputTime();
void SetOutputTime(int time_in_ms);

void SetVolume(int volume);
void SetPan(int pan);
void EQSet(int on, char data[10], int preamp);

bool TestDecodeMp3FromMemory(
    const unsigned char* mp3Data,
    size_t mp3DataSize,
    unsigned int* outChannels,
    unsigned int* outSampleRate,
    drmp3_uint64* outTotalPCMFrames,
    drmp3_uint64* outDecodedTestFrames
);

bool TestDecodeWavFromMemory(
    const unsigned char* wavData,
    size_t wavDataSize,
    unsigned int* outChannels,
    unsigned int* outSampleRate,
    drwav_uint64* outTotalPCMFrames,
    drwav_uint64* outDecodedTestFrames
);

bool TestDecodeFlacFromMemory(
    const unsigned char* flacData,
    size_t flacDataSize,
    unsigned int* outChannels,
    unsigned int* outSampleRate,
    drflac_uint64* outTotalPCMFrames,
    drflac_uint64* outDecodedTestFrames
);

bool TestDecodeOggFromMemory(
    const unsigned char* oggData,
    size_t oggDataSize,
    unsigned int* outChannels,
    unsigned int* outSampleRate,
    unsigned int* outTotalSamples,
    unsigned int* outDecodedTestSamples
);