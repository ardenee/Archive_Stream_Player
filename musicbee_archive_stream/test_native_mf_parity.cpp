#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "archive_reader.h"
#include "mf_audio_decoder.h"

int main(int argc,char** argv)
{
    if (argc!=4)
        return 2;
    const AudioFormat format=GetAudioFormatFromPath(argv[1]);
    if (!MfIsNativeMediaFormat(format))
        return 3;
    uint64_t encoded=0;
    char name[1024]{};
    if (!ArchiveGetEntryInfo(argv[2],0,name,sizeof(name),&encoded) || !encoded)
        return 4;
    MfAudioDecoder* local=nullptr;
    MfAudioDecoder* archived=nullptr;
    if (!MfOpenCacheFileDecoder(argv[1],format,&local) || !local)
    {
        printf("MF_PCM_REF_OPEN_FAIL\n");
        return 5;
    }
    if (!MfOpenArchiveDecoder(argv[2],0,encoded,format,&archived) || !archived)
    {
        printf("MF_PCM_ARCHIVE_OPEN_FAIL\n");
        MfCloseDecoder(local);
        return 6;
    }
    const int rate=MfGetSampleRate(local);
    const int channels=MfGetChannels(local);
    printf("MF_COMPARE_REF rate=%d channels=%d bits=%d duration_ms=%d\n",
           rate,channels,MfGetBitsPerSample(local),MfGetLengthMs(local));
    printf("MF_COMPARE_ARCHIVE rate=%d channels=%d bits=%d duration_ms=%d\n",
           MfGetSampleRate(archived),MfGetChannels(archived),MfGetBitsPerSample(archived),MfGetLengthMs(archived));
    if (rate!=MfGetSampleRate(archived) || channels!=MfGetChannels(archived) ||
        MfGetBitsPerSample(local)!=16 || MfGetBitsPerSample(archived)!=16 || channels<1 || channels>2)
        return 7;
    unsigned char a[65536]{};
    unsigned char b[65536]{};
    size_t n=MfReadPcm(archived,a,sizeof(a));
    size_t m=MfReadPcm(local,b,sizeof(b));
    bool firstMatch=n==m && n && memcmp(a,b,n)==0;
    printf("MF_FIRST_PCM_PARITY equal=%d bytes=%zu/%zu\n",firstMatch,n,m);
    bool sought=MfSeekMs(archived,75000) && MfSeekMs(local,75000);
    n=sought?MfReadPcm(archived,a,sizeof(a)):0;
    m=sought?MfReadPcm(local,b,sizeof(b)):0;
    bool seekMatch=sought && n==m && n && memcmp(a,b,n)==0;
    printf("MF_75S_SEEK_PCM_PARITY sought=%d equal=%d bytes=%zu/%zu\n",sought,seekMatch,n,m);
    if (!MfSeekMs(archived,0) || !MfSeekMs(local,0))
        return 8;
    uint64_t total=0;
    bool identical=true;
    while (total<1024ULL*1024ULL*1024ULL)
    {
        n=MfReadPcm(archived,a,sizeof(a));
        m=MfReadPcm(local,b,sizeof(b));
        if (n!=m || (n && memcmp(a,b,n)!=0))
        {
            printf("MF_PCM_MISMATCH offset=%llu chunk=%zu/%zu\n",(unsigned long long)total,n,m);
            identical=false;
            break;
        }
        if (!n)
            break;
        total+=n;
    }
    printf("MF_FULL_TRACK_PCM_COMPARE equal=%d bytes=%llu\n",identical,(unsigned long long)total);
    MfCloseDecoder(archived);
    MfCloseDecoder(local);
    if (!firstMatch || !seekMatch || !identical || total<1000000)
        return 9;
    puts("MF_LOCAL_VS_ARCHIVE_FULL_TRACK_PCM_AND_SEEK_PARITY_PASS");
    return 0;
}
