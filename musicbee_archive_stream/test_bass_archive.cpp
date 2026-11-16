// Standalone BASS bridge probe (not equivalent to native MusicBee host dispatch).
// Compile against official external BASS SDK; never redistribute BASS.dll.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cstring>
#include "bass.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        puts("Usage: bass_archive_probe.exe BASS-Archive-Plugin.dll entry-0.ast");
        return 2;
    }
    if (!BASS_Init(0, 44100, 0, nullptr, nullptr))
    {
        printf("BASS_ARCHIVE_INIT_FAIL error=%d\n", BASS_ErrorGetCode());
        return 3;
    }
    HPLUGIN plugin = BASS_PluginLoad(argv[1], BASS_UNICODE);
    printf("BASS_ARCHIVE_ADDON_LOAD handle=%u error=%d\n", plugin, BASS_ErrorGetCode());
    fflush(stdout);
    if (!plugin)
        return 4;
    HSTREAM stream = BASS_StreamCreateFile(FALSE, argv[2], 0, 0, BASS_STREAM_DECODE | BASS_UNICODE);
    printf("BASS_ARCHIVE_STREAM_OPEN handle=%u error=%d\n", stream, BASS_ErrorGetCode());
    fflush(stdout);
    if (!stream)
        return 5;
    BASS_CHANNELINFO info{};
    BASS_ChannelGetInfo(stream, &info);
    printf("BASS_ARCHIVE_CHANNEL_INFO freq=%u chans=%u ctype=%u\n", info.freq, info.chans, info.ctype);
    const char* title = BASS_ChannelGetTags(stream, BASS_TAG_OGG);
    if (!title || strncmp(title, "TITLE=", 6) != 0 || strlen(title) <= 6 ||
        title[strlen(title) + 1] != '\0')
    {
        printf("BASS_ARCHIVE_TITLE_TAG_FAIL title=%s\n", title ? title : "(null)");
        return 9;
    }
    const char* apeTitle = BASS_ChannelGetTags(stream, BASS_TAG_APE);
    const char* mfTitle = BASS_ChannelGetTags(stream, BASS_TAG_MF);
    if (!apeTitle || !mfTitle || strcmp(title, apeTitle) != 0 || strcmp(title, mfTitle) != 0)
        return 10;
    printf("BASS_ARCHIVE_NATIVE_TITLE_TAG_PASS %s\n", title);
    const QWORD length = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
    printf("BASS_ARCHIVE_LENGTH_PCM_BYTES=%llu\n", static_cast<unsigned long long>(length));
    fflush(stdout);
    std::vector<unsigned char> pcm(65536);
    DWORD read = BASS_ChannelGetData(stream, pcm.data(), static_cast<DWORD>(pcm.size()));
    printf("BASS_ARCHIVE_FIRST_PCM_BYTES=%u error=%d\n", read, BASS_ErrorGetCode());
    fflush(stdout);
    if (read != pcm.size())
        return 6;
    QWORD seek = length / 3;
    seek -= seek % (static_cast<QWORD>(info.chans) * sizeof(int16_t));
    BOOL seeked = BASS_ChannelSetPosition(stream, seek, BASS_POS_BYTE);
    printf("BASS_ARCHIVE_NATIVE_SEEK_RESULT=%d error=%d target_pcm_bytes=%llu\n", seeked, BASS_ErrorGetCode(), static_cast<unsigned long long>(seek));
    if (!seeked)
        return 7;
    read = BASS_ChannelGetData(stream, pcm.data(), static_cast<DWORD>(pcm.size()));
    printf("BASS_ARCHIVE_SEEKED_PCM_BYTES=%u error=%d\n", read, BASS_ErrorGetCode());
    BASS_StreamFree(stream);
    BASS_PluginFree(plugin);
    BASS_Free();
    if (read != pcm.size())
        return 8;
    puts("BASS_ARCHIVE_MP3_STANDALONE_PCM_AND_SEEK_ALL_PASS");
    return 0;
}
