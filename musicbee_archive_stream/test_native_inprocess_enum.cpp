#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include "bass.h"

using Enumerate = int (__stdcall *)(const wchar_t*, const wchar_t*);
int wmain(int argc, wchar_t** argv)
{
    if (argc != 5 && argc != 6)
        return 2;
    if (!BASS_Init(0, 44100, 0, nullptr, nullptr))
    {
        printf("QA_BASS_INIT_FAIL=%d\n", BASS_ErrorGetCode());
        return 3;
    }
    HPLUGIN plugin = BASS_PluginLoad(argv[1], BASS_UNICODE);
    if (!plugin)
    {
        printf("QA_BASS_PLUGIN_LOAD_FAIL=%d\n", BASS_ErrorGetCode());
        return 4;
    }
    HMODULE module = GetModuleHandleW(L"bass_archive_stream.dll");
    if (!module)
        return 5;
    const bool allowPcm = argc == 6 && wcscmp(argv[5], L"pcm") == 0;
    const bool allowAudio = argc == 6 && wcscmp(argv[5], L"audio") == 0;
    const bool allowFour = argc == 6 && wcscmp(argv[5], L"four") == 0;
    const bool allowSeven = argc == 6 && wcscmp(argv[5], L"seven") == 0;
    const bool allowBuffered = argc == 6 && wcscmp(argv[5], L"buffered") == 0;
    const bool allowDisk = argc == 6 && wcscmp(argv[5], L"disk") == 0;
    if (argc == 6 && !allowPcm && !allowAudio && !allowFour && !allowSeven && !allowBuffered && !allowDisk)
        return 2;
    const char* exportName = allowDisk ? "ArchiveStreamWriteNativeDiskLastTracksW" :
        allowBuffered ? "ArchiveStreamWriteNativeBufferedTracksW" :
        allowSeven ? "ArchiveStreamWriteNativeSevenCodecsW" :
        allowFour ? "ArchiveStreamWriteNativeFourCodecsW" :
        allowAudio ? "ArchiveStreamWriteNativeAudioTracksW" :
        allowPcm ? "ArchiveStreamWriteNativePcmTracksW" : "ArchiveStreamWriteNativeMp3TracksW";
    auto scan = reinterpret_cast<Enumerate>(GetProcAddress(module, exportName));
    if (!scan)
        return 6;
    const int n = scan(argv[2], argv[3]);
    int expected = _wtoi(argv[4]);
    printf("QA_NATIVE_AUDIO_SCAN mode=%s count=%d expected=%d\n", allowDisk ? "disk" : allowBuffered ? "buffered" : allowSeven ? "seven" : allowFour ? "four" : allowAudio ? "audio" : allowPcm ? "pcm" : "mp3", n, expected);
    if (n != expected)
        return 7;
    int files = 0;
    if (std::filesystem::exists(argv[3]))
    {
        for (const auto& item : std::filesystem::directory_iterator(argv[3]))
        {
            if (item.is_regular_file() && item.path().extension() == L".ast")
                files++;
        }
    }
    printf("QA_NATIVE_MP3_MANIFESTS=%d\n", files);
    if (files != n)
        return 8;
    BASS_PluginFree(plugin);
    BASS_Free();
    puts("QA_NATIVE_IN_PROCESS_ENUM_PASS");
    return 0;
}
