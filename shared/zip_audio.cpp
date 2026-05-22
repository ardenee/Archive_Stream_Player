#include <windows.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "wa_ipc.h"
//#include "plugin_state.h"
#include "archive_common.h"
#include "zip_audio.h"
#include "archive_reader.h"
#include "debug_log.h"
#include "archive_backend.h"

#include "dr_mp3.h"
#include "dr_wav.h"
#include "dr_flac.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

static bool GetAudioDurationMsFromArchiveEntry(const char* archivePath, mz_uint index, mz_uint64 uncompSize, const char* filename, int* outLengthMs);


bool GetSortedAudioEntriesInZip(const char* zipPath, std::vector<ArchiveAudioEntry>& outEntries)
{
    return ArchiveListAudioEntries(zipPath, outEntries);
}

bool FindFirstAudioInZip(const char* zipPath, mz_uint* outIndex, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize)
{
    return ArchiveFindFirstAudioEntry(zipPath, outIndex, outName, outNameSize, outUncompressedSize);
}
/*
bool GetVirtualArchiveEntryInfo(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs)
{
    if (outTitle && outTitleSize > 0)
        outTitle[0] = '\0';

    if (outLengthMs)
        *outLengthMs = -1000;

    char archivePath[MAX_PATH * 4];
    char internalPath[1024];
    mz_uint index = 0;

    archivePath[0] = '\0';
    internalPath[0] = '\0';

    if (!ParseVirtualArchivePath(file, archivePath, sizeof(archivePath), &index, internalPath, sizeof(internalPath)))
        return false;
    
//        For 7z entries, we may not be able to query the archive entry through
//        the old ZIP/miniz path yet, but the virtual playlist path already
//        contains the internal filename after the final pipe.
//
//       Example:
//      Main.7z.archive_stream|0|205-Charlie May - Midnight.flac
    
    if (internalPath[0])
    {
        if (outTitle && outTitleSize > 0)
            CopyBaseName(outTitle, outTitleSize, internalPath);
    }

    char entryName[1024];
    mz_uint64 uncompSize = 0;

    entryName[0] = '\0';

    if (!ArchiveGetEntryInfo(archivePath, index, entryName, sizeof(entryName), &uncompSize))
    {
        
        //   ZIP should normally succeed here.
        //    7z currently fails here because in_archive_stream/archive_reader.cpp
        //    is still ZIP-only. That is okay for title display, because we already
        //    used internalPath above.

            Return true if we at least have an internal filename.
        
        if (!internalPath[0])
            DebugLogWrite("WARN", "VIRTUAL", "ENTRY_INFO_FAIL", "file=%s | archive=%s | index=%u | reason=archive_entry_info_failed_no_internal_name", file ? file : "", archivePath, (unsigned int)index);

        return internalPath[0] != '\0';
    }

    if (outTitle && outTitleSize > 0 && !internalPath[0])
        CopyBaseName(outTitle, outTitleSize, entryName);

    if (outLengthMs)
    {
        int scannedLengthMs = -1000;

        const char* durationName = entryName[0] ? entryName : internalPath;

        if (!GetAudioDurationMsFromArchiveEntry(archivePath, index, uncompSize, durationName, &scannedLengthMs) || scannedLengthMs <= 0)
            scannedLengthMs = -1000;

        *outLengthMs = scannedLengthMs;
    }

    return true;
}
*/

static bool GetAudioDurationMsFromArchiveEntry(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs)
{
    if (!archivePath || !outLengthMs || uncompSize == 0 || !filename)
        return false;

    *outLengthMs = -1000;

    AudioFormat format = GetAudioFormatFromPath(filename);
    const uint64_t maxMetadataBytes = (uint64_t)MAX_METADATA_SCAN_MB * 1024ULL * 1024ULL;

    if (uncompSize > maxMetadataBytes)
        return false;

    std::vector<unsigned char> tempBuffer;
    std::string extractError;

    bool result = false;

    if (!ArchiveExtractEntryToMemory(archivePath, (uint32_t)index, tempBuffer, extractError))
    {
        DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | bytes=%llu | error=%s | reason=extract_to_memory_failed", archivePath, (unsigned int)index, filename, (unsigned long long)uncompSize, extractError.c_str());
    }
    else if (tempBuffer.empty())
    {
        DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "archive=%s | index=%u | entry=%s | bytes=%llu | reason=extract_to_memory_empty", archivePath, (unsigned int)index, filename, (unsigned long long)uncompSize);
    }
    else
    {
        if (format == AUDIO_FORMAT_MP3)
        {
            drmp3 mp3;

            if (drmp3_init_memory(&mp3, tempBuffer.data(), tempBuffer.size(), NULL))
            {
                drmp3_uint64 totalFrames = drmp3_get_pcm_frame_count(&mp3);

                if (mp3.sampleRate > 0 && totalFrames > 0)
                {
                    double seconds = (double)totalFrames / (double)mp3.sampleRate;
                    *outLengthMs = (int)(seconds * 1000.0);
                    result = true;
                }

                drmp3_uninit(&mp3);
            }
            else
            {
                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "decoder=MP3 | archive=%s | index=%u | entry=%s | reason=decoder_init_failed", archivePath, (unsigned int)index, filename);
            }
        }
        else if (format == AUDIO_FORMAT_WAV)
        {
            drwav wav;

            if (drwav_init_memory(&wav, tempBuffer.data(), tempBuffer.size(), NULL))
            {
                if (wav.sampleRate > 0 && wav.totalPCMFrameCount > 0)
                {
                    double seconds = (double)wav.totalPCMFrameCount / (double)wav.sampleRate;
                    *outLengthMs = (int)(seconds * 1000.0);
                    result = true;
                }

                drwav_uninit(&wav);
            }
            else
            {
                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "decoder=WAV | archive=%s | index=%u | entry=%s | reason=decoder_init_failed", archivePath, (unsigned int)index, filename);
            }
        }
        else if (format == AUDIO_FORMAT_FLAC)
        {
            drflac* flac = drflac_open_memory(tempBuffer.data(), tempBuffer.size(), NULL);

            if (flac)
            {
                if (flac->sampleRate > 0 && flac->totalPCMFrameCount > 0)
                {
                    double seconds = (double)flac->totalPCMFrameCount / (double)flac->sampleRate;
                    *outLengthMs = (int)(seconds * 1000.0);
                    result = true;
                }

                drflac_close(flac);
            }
            else
            {
                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "decoder=FLAC | archive=%s | index=%u | entry=%s | reason=decoder_open_failed", archivePath, (unsigned int)index, filename);
            }
        }
        else if (format == AUDIO_FORMAT_OGG)
        {
            int error = 0;
            stb_vorbis* ogg = stb_vorbis_open_memory(tempBuffer.data(), (int)tempBuffer.size(), &error, NULL);

            if (ogg)
            {
                stb_vorbis_info info = stb_vorbis_get_info(ogg);
                unsigned int sampleRate = info.sample_rate;
                unsigned int totalSamples = stb_vorbis_stream_length_in_samples(ogg);

                if (sampleRate > 0 && totalSamples > 0)
                {
                    double seconds = (double)totalSamples / (double)sampleRate;
                    *outLengthMs = (int)(seconds * 1000.0);
                    result = true;
                }

                stb_vorbis_close(ogg);
            }
            else
            {
                DebugLogWrite("WARN", "METADATA", "DURATION_FAIL", "decoder=OGG | archive=%s | index=%u | entry=%s | stb_error=%d | reason=decoder_open_failed", archivePath, (unsigned int)index, filename, error);
            }
        }
    }

    return result;
}

bool RequestZipExpandFromGeneralPlugin(HWND hwndWinamp, const char* zipPath)
{
    if (!zipPath || !zipPath[0])
    {
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "reason=empty_path");
        return false;
    }

    HWND hwnd = FindWindowA(ARCHIVE_STREAM_HIDDEN_CLASS, NULL);

    if (!hwnd)
    {
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "archive=%s | reason=hidden_window_not_found", zipPath);
        return false;
    }

    int playlistIndex = -1;

    if (hwndWinamp)
        playlistIndex = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTPOS);

    char payload[MAX_PATH * 4 + 64];

    sprintf_s(payload, sizeof(payload), "%d|%s", playlistIndex, zipPath);

    DebugLogWrite("INFO", "EXPAND", "REQUEST_SENT", "archive=%s | playlist_index=%d | payload=%s", zipPath, playlistIndex, payload);

    COPYDATASTRUCT cds;
    memset(&cds, 0, sizeof(cds));
    cds.dwData = ARCHIVE_STREAM_COPYDATA_EXPAND_ZIP;
    cds.lpData = payload;
    cds.cbData = (DWORD)strlen(payload) + 1;

    LRESULT result = SendMessageA(hwnd, WM_COPYDATA, 0, (LPARAM)&cds);

    if (result == 0)
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "archive=%s | index=%d | reason=copydata_rejected", zipPath, playlistIndex);

    return result != 0;
}