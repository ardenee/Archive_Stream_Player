#pragma once

#include <windows.h>
#include "archive_common.h"

struct MfAudioDecoder;

bool MfIsNativeMediaFormat(AudioFormat format);
const wchar_t* MfMimeTypeForFormat(AudioFormat format);
const wchar_t* MfVirtualUrlExtensionForFormat(AudioFormat format);
bool MfOpenArchiveDecoder(const char* archivePath, mz_uint index, mz_uint64 length, AudioFormat format, HWND hwndParent, MfAudioDecoder** outDecoder);
bool MfOpenCacheFileDecoder(const char* cachePath, AudioFormat format, HWND hwndParent, MfAudioDecoder** outDecoder);
size_t MfReadPcm(MfAudioDecoder* decoder, void* outBuffer, size_t outBufferBytes);
bool MfSeekMs(MfAudioDecoder* decoder, int positionMs);
void MfCloseDecoder(MfAudioDecoder* decoder);
int MfGetSampleRate(MfAudioDecoder* decoder);
int MfGetChannels(MfAudioDecoder* decoder);
int MfGetBitsPerSample(MfAudioDecoder* decoder);
int MfGetLengthMs(MfAudioDecoder* decoder);