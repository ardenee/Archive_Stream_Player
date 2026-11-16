#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "archive_reader.h"
#include "mf_audio_decoder.h"
#include "debug_log.h"

int main(int argc,char** argv)
{
    if(argc!=6)
    {
        puts("usage: mf_midstream.exe original codec archive route log");
        return 2;
    }
    const AudioFormat format=GetAudioFormatFromPath(argv[1]);
    if(!MfIsNativeMediaFormat(format))
        return 3;
    const bool blocked=strcmp(argv[4],"blocked")==0;
    const bool cache=strcmp(argv[4],"cache")==0;
    if(!cache && !blocked && strcmp(argv[4],"buffer")!=0)
        return 4;
    DebugLogConfigure(true,argv[5],true);
    if(cache||blocked)
        _putenv_s("AST_MF_TEST_MEMORY_MB","1");
    else
        _putenv_s("AST_MF_TEST_MEMORY_MB","");
    _putenv_s("AST_MF_TEST_CACHE_DISABLED",blocked?"1":"");
    _putenv_s("AST_MF_TEST_STREAM_FAIL_AFTER","");

    char name[1024]{};
    uint64_t size=0;
    if(!ArchiveGetEntryInfo(argv[3],0,name,sizeof(name),&size)||!size)
    {
        puts("MF_MIDSTREAM_ENTRY_INFO_FAIL");
        return 5;
    }
    MfAudioDecoder* local=nullptr;
    MfAudioDecoder* native=nullptr;
    if(!MfOpenCacheFileDecoder(argv[1],format,&local)||!local)
    {
        puts("MF_MIDSTREAM_LOCAL_OPEN_FAIL");
        return 6;
    }
    if(!MfOpenArchiveDecoder(argv[3],0,size,format,&native)||!native)
    {
        puts("MF_MIDSTREAM_ARCHIVE_OPEN_FAIL");
        MfCloseDecoder(local);
        return 7;
    }
    const int sampleRate=MfGetSampleRate(local);
    const int channels=MfGetChannels(local);
    const bool supported=sampleRate>=8000&&sampleRate<=384000&&
        (channels==1||channels==2)&&
        sampleRate==MfGetSampleRate(native)&&channels==MfGetChannels(native)&&
        MfGetBitsPerSample(native)==16&&MfGetBitsPerSample(local)==16;
    if(!supported)
    {
        puts("MF_MIDSTREAM_FORMAT_MISMATCH");
        return 8;
    }
    unsigned char a[32768]{};
    unsigned char b[32768]{};
    size_t n=MfReadPcm(local,a,sizeof(a));
    size_t m=MfReadPcm(native,b,sizeof(b));
    if(n!=m||!n||memcmp(a,b,n)!=0)
    {
        printf("MF_MIDSTREAM_FIRST_PCM_MISMATCH sizes=%zu/%zu\n",n,m);
        return 9;
    }
    // Arm fault injection *after* actual PCM has been emitted. The compiled
    // test-only byte stream reads this environment variable on each read.
    _putenv_s("AST_MF_TEST_STREAM_FAIL_AFTER","4096");
    uint64_t total=n;
    unsigned chunks=1;
    bool matching=true;
    while(total<1024ULL*1024*1024)
    {
        n=MfReadPcm(local,a,sizeof(a));
        m=MfReadPcm(native,b,sizeof(b));
        if(n!=m||(n&&memcmp(a,b,n)!=0))
        {
            printf("MF_MIDSTREAM_PCM_MISMATCH codec=%s offset=%llu sizes=%zu/%zu\n",
                argv[2],static_cast<unsigned long long>(total),n,m);
            matching=false;
            break;
        }
        if(!n)
            break;
        total+=n;
        ++chunks;
    }
    _putenv_s("AST_MF_TEST_STREAM_FAIL_AFTER","");
    bool seeked=matching && MfSeekMs(local,75000) && MfSeekMs(native,75000);
    if(seeked)
    {
        n=MfReadPcm(local,a,sizeof(a));
        m=MfReadPcm(native,b,sizeof(b));
        seeked=n==m&&n>0&&memcmp(a,b,n)==0;
    }
    MfCloseDecoder(native);
    MfCloseDecoder(local);
    DebugLogShutdown();
    if(blocked)
    {
        if(matching)
        {
            puts("MF_MIDSTREAM_DISABLED_CACHE_UNEXPECTED_SUCCESS");
            return 12;
        }
        printf("MF_MIDSTREAM_BLOCKED_RECOVERY_EXPECTED_EARLY_END_PASS codec=%s decoded_before_failure=%llu\n",
            argv[2],static_cast<unsigned long long>(total));
        return 0;
    }
    if(!matching||!seeked||total<1000000||total>=1024ULL*1024*1024)
    {
        printf("MF_MIDSTREAM_PCM_PARITY_FAIL codec=%s bytes=%llu seek=%d\n",
            argv[2],static_cast<unsigned long long>(total),seeked);
        return 10;
    }
    printf("MF_MIDSTREAM_FULL_PCM_AND_SEEK_PARITY_PASS codec=%s route=%s bytes=%llu chunks=%u\n",
        argv[2],argv[4],static_cast<unsigned long long>(total),chunks);
    return 0;
}
