#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <fstream>
#include "bass.h"

int wmain(int argc,wchar_t** argv)
{
    if (argc != 5 && argc != 6 && argc != 7)
        return 2;
    if (!BASS_Init(0,44100,0,nullptr,nullptr))
        return 3;
    HPLUGIN plugin=BASS_PluginLoad(argv[1], BASS_UNICODE);
    if (!plugin)
    {
        printf("BASS_WAV_PLUGIN_LOAD_FAIL=%d\n",BASS_ErrorGetCode());
        return 4;
    }
    HSTREAM stream=BASS_StreamCreateFile(FALSE,argv[2],0,0,BASS_STREAM_DECODE | BASS_UNICODE);
    if (!stream)
    {
        printf("BASS_WAV_STREAM_OPEN_FAIL=%d\n",BASS_ErrorGetCode());
        return 5;
    }
    BASS_CHANNELINFO info{};
    if (!BASS_ChannelGetInfo(stream,&info) || info.chans!=2 || info.freq!=44100)
        return 6;
    std::vector<unsigned char> data(65536);
    QWORD full=BASS_ChannelGetLength(stream,BASS_POS_BYTE);
    DWORD first=BASS_ChannelGetData(stream,data.data(),static_cast<DWORD>(data.size()));
    printf("NATIVE_WAV_PCM_FIRST count=%u len=%llu\n",first,static_cast<unsigned long long>(full));
    if (first!=data.size())
        return 7;
    {
        std::ofstream out(argv[3],std::ios::binary);
        out.write(reinterpret_cast<char*>(data.data()),data.size());
        if (!out.good())
            return 8;
    }
    QWORD target=BASS_ChannelSeconds2Bytes(stream,argc == 7 ? _wtof(argv[6]) : 75.0);
    if (!BASS_ChannelSetPosition(stream,target,BASS_POS_BYTE))
        return 9;
    QWORD position=BASS_ChannelGetPosition(stream,BASS_POS_BYTE);
    printf("NATIVE_WAV_NATIVE_SEEK requested=%llu actual=%llu\n",static_cast<unsigned long long>(target),static_cast<unsigned long long>(position));
    if (target!=position)
        return 10;
    DWORD after=BASS_ChannelGetData(stream,data.data(),static_cast<DWORD>(data.size()));
    if (after!=data.size())
        return 11;
    {
        std::ofstream out(argv[4],std::ios::binary);
        out.write(reinterpret_cast<char*>(data.data()),data.size());
        if (!out.good())
            return 12;
    }
    if (argc >= 6)
    {
        if (!BASS_ChannelSetPosition(stream, 0, BASS_POS_BYTE))
            return 13;
        std::ofstream fullOutput(argv[5], std::ios::binary);
        if (!fullOutput)
            return 14;
        unsigned long long total = 0;
        while (total < full)
        {
            const DWORD count = BASS_ChannelGetData(stream, data.data(), static_cast<DWORD>(data.size()));
            if (count == static_cast<DWORD>(-1) || !count || count > data.size())
            {
                printf("NATIVE_FULL_PCM_FAILED offset=%llu bass_error=%d count=%u\n", total, BASS_ErrorGetCode(), count);
                return 15;
            }
            fullOutput.write(reinterpret_cast<const char*>(data.data()), count);
            if (!fullOutput.good())
                return 16;
            total += count;
        }
        fullOutput.close();
        if (total != full)
            return 17;
        printf("NATIVE_FULL_PCM_STREAM_COMPLETE bytes=%llu\n", total);
    }
    BASS_StreamFree(stream);
    BASS_PluginFree(plugin);
    BASS_Free();
    puts("NATIVE_WAV_BASS_PCM_OUTPUT_SEEK_PASS");
    return 0;
}
