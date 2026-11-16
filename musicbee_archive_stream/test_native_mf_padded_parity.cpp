#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "archive_reader.h"
#include "mf_audio_decoder.h"

static int fail(const char* reason)
{
    printf("NATIVE_MF_ENCODED_CACHE_PARITY_FAIL reason=%s\n",reason);
    return 1;
}

int main(int argc,char** argv)
{
    if (argc!=3)
        return fail("args");
    const AudioFormat codec=GetAudioFormatFromPath(argv[1]);
    if (!MfIsNativeMediaFormat(codec) || codec==AUDIO_FORMAT_AAC)
        return fail("format");
    MfAudioDecoder* original=nullptr;
    MfAudioDecoder* padded=nullptr;
    if (!MfOpenCacheFileDecoder(argv[1],codec,&original) || !original)
        return fail("original_open");
    if (!MfOpenCacheFileDecoder(argv[2],codec,&padded) || !padded)
    {
        MfCloseDecoder(original);
        return fail("padded_open");
    }
    const int rate=MfGetSampleRate(original);
    const int channels=MfGetChannels(original);
    bool valid=rate==MfGetSampleRate(padded) && channels==MfGetChannels(padded) &&
        rate>=8000 && rate<=384000 && (channels==1 || channels==2) &&
        MfGetBitsPerSample(original)==16 && MfGetBitsPerSample(padded)==16;
    if (!valid)
        return fail("decoded_format");

    unsigned char a[65536]{};
    unsigned char b[65536]{};
    size_t n=MfReadPcm(original,a,sizeof(a));
    size_t m=MfReadPcm(padded,b,sizeof(b));
    valid=n==m && n==sizeof(a) && memcmp(a,b,n)==0;
    printf("NATIVE_MF_CACHE_PARITY_BEGIN bytes=%zu same=%d\n",n,valid);
    if (!valid)
        return fail("first_buffer");
    valid=MfSeekMs(original,75000) && MfSeekMs(padded,75000);
    n=valid?MfReadPcm(original,a,sizeof(a)):0;
    m=valid?MfReadPcm(padded,b,sizeof(b)):0;
    valid=valid && n==m && n==sizeof(a) && memcmp(a,b,n)==0;
    printf("NATIVE_MF_CACHE_PARITY_75S bytes=%zu same=%d\n",n,valid);
    if (!valid)
        return fail("seek_pcm");
    if (!MfSeekMs(original,0) || !MfSeekMs(padded,0))
        return fail("rewind");

    uint64_t total=0;
    unsigned chunk=0;
    while (total<512ULL*1024ULL*1024ULL)
    {
        n=MfReadPcm(original,a,sizeof(a));
        m=MfReadPcm(padded,b,sizeof(b));
        if (n!=m || (n && memcmp(a,b,n)!=0))
        {
            printf("NATIVE_MF_CACHE_PARITY_MISMATCH chunk=%u offset=%llu sizes=%zu/%zu\n",
                chunk,static_cast<unsigned long long>(total),n,m);
            return fail("whole_track");
        }
        if (!n)
            break;
        total+=n;
        ++chunk;
    }
    MfCloseDecoder(original);
    MfCloseDecoder(padded);
    if (!valid || total<1000000 || total>=512ULL*1024ULL*1024ULL)
        return fail("length");
    printf("NATIVE_MF_PADDED_FULL_PCM_PARITY_PASS bytes=%llu\n",
        static_cast<unsigned long long>(total));
    return 0;
}
