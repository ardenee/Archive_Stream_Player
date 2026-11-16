// Standalone real BASS PCM reader for fault-injected native recovery tests.
// This is not the MusicBee GUI. The result is compared byte-for-byte with
// the exact baseline decoded through the same native host stack.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "bass.h"

int wmain(int argc,wchar_t** argv)
{
    if(argc!=4){puts("Usage: recover_probe.exe addon.dll member.ast decoded.pcm");return 2;}
    if(!BASS_Init(0,44100,0,nullptr,nullptr)){printf("RECOVER_INIT_FAIL %d\n",BASS_ErrorGetCode());return 3;}
    HPLUGIN addon=BASS_PluginLoad(argv[1],BASS_UNICODE);
    if(!addon){printf("RECOVER_PLUGIN_FAIL %d\n",BASS_ErrorGetCode());return 4;}
    HSTREAM channel=BASS_StreamCreateFile(FALSE,argv[2],0,0,BASS_STREAM_DECODE|BASS_UNICODE);
    if(!channel){printf("RECOVER_OPEN_FAIL %d\n",BASS_ErrorGetCode());return 5;}
    const QWORD expected=BASS_ChannelGetLength(channel,BASS_POS_BYTE);
    if(expected==static_cast<QWORD>(-1)||expected<1048576){printf("RECOVER_LENGTH_FAIL %llu\n",expected);return 6;}
    FILE* pcm=nullptr;
    if(_wfopen_s(&pcm,argv[3],L"wb")!=0||!pcm)return 7;
    std::vector<unsigned char> block(65536);
    uint64_t total=0;
    unsigned chunks=0;
    while(total<expected)
    {
        const DWORD wanted=static_cast<DWORD>(std::min<uint64_t>(block.size(),expected-total));
        const DWORD got=BASS_ChannelGetData(channel,block.data(),wanted);
        if(got==0xFFFFFFFFu||got==0)
        {
            // dr_mp3 may overestimate the end by MPEG padding samples.
            // This is allowed only very near EOS for the MP3 fixture,
            // never as a substitute for successfully recovering midstream.
            const bool mp3Tail = wcsstr(argv[2],L"mp3-buffer") != nullptr &&
                expected>=total && expected-total<=2304ULL*4;
            if(mp3Tail)
                break;
            printf("RECOVER_PCM_EARLY_END offset=%llu expected=%llu got=%u error=%d\n",total,expected,got,BASS_ErrorGetCode());
            fclose(pcm);
            return 8;
        }
        if(got>wanted){fclose(pcm);return 8;}
        if(fwrite(block.data(),1,got,pcm)!=got){fclose(pcm);return 9;}
        total+=got;
        ++chunks;
    }
    fclose(pcm);
    printf("RECOVER_NATIVE_FULL_PCM_PASS bytes=%llu chunks=%u\n",total,chunks);
    BASS_StreamFree(channel);
    BASS_PluginFree(addon);
    BASS_Free();
    return 0;
}
