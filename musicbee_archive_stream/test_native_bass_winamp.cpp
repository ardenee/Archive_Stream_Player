// Optional low-level BASS/Winamp compatibility harness for MusicBee 3.x x86.
// Runs OUTSIDE MusicBee: proving BASS returns PCM is NOT proof of MusicBee
// playlist dispatch, which remained unverified after input-plugin import.
// Must not be bundled with BASS, Winamp SDK or third-party binaries.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

template<class Fn> Fn exportFn(HMODULE module, const char* name)
{
    if (!module) return nullptr;
    return reinterpret_cast<Fn>(GetProcAddress(module, name));
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
    {
        puts("Usage: musicbee_native_probe.exe WinampInputPlugin.dll virtualTrack");
        return 10;
    }
    char plugin[MAX_PATH * 4]{};
    if (!WideCharToMultiByte(CP_ACP, 0, argv[1], -1, plugin, sizeof(plugin), nullptr, nullptr))
        return 11;
    char file[8192]{};
    if (!WideCharToMultiByte(CP_ACP, 0, argv[2], -1, file, sizeof(file), nullptr, nullptr))
        return 12;
    HMODULE bass = LoadLibraryW(L"bass.dll");
    HMODULE adapter = LoadLibraryW(L"bass_winamp.dll");
    if (!bass || !adapter)
    {
        printf("BASS_LOAD_FAIL bass=%p adapter=%p error=%lu\n", bass, adapter, GetLastError());
        return 13;
    }
    auto bassInit = exportFn<BOOL(WINAPI*)(int,DWORD,DWORD,HWND,void*)>(bass,"BASS_Init");
    auto bassFree = exportFn<BOOL(WINAPI*)()>(bass,"BASS_Free");
    auto bassError = exportFn<int(WINAPI*)()>(bass,"BASS_ErrorGetCode");
    auto bassData = exportFn<DWORD(WINAPI*)(DWORD,void*,DWORD)>(bass,"BASS_ChannelGetData");
    auto bassLength = exportFn<uint64_t(WINAPI*)(DWORD,DWORD)>(bass,"BASS_ChannelGetLength");
    auto bassChanFree = exportFn<BOOL(WINAPI*)(DWORD)>(bass,"BASS_StreamFree");
    auto loadPlugin = exportFn<DWORD(WINAPI*)(const char*)>(adapter,"BASS_WINAMP_LoadPlugin");
    auto name = exportFn<const char*(WINAPI*)(DWORD)>(adapter,"BASS_WINAMP_GetName");
    auto extensions = exportFn<const char*(WINAPI*)(DWORD)>(adapter,"BASS_WINAMP_GetExtentions");
    auto create = exportFn<DWORD(WINAPI*)(const char*,DWORD)>(adapter,"BASS_WINAMP_StreamCreate");
    auto unload = exportFn<BOOL(WINAPI*)(DWORD)>(adapter,"BASS_WINAMP_UnloadPlugin");
    if (!bassInit || !bassFree || !bassError || !bassData || !loadPlugin || !create)
    {
        puts("BASS_IMPORT_FAIL");
        return 14;
    }
    if (!bassInit(0,44100,0,GetConsoleWindow(),nullptr))
    {
        printf("BASS_INIT_FAIL error=%d\n", bassError());
        return 15;
    }
    DWORD pluginHandle=loadPlugin(plugin);
    printf("BASS_WINAMP_PLUGIN_LOADED=%u bass_error=%d\n",pluginHandle,bassError());
    fflush(stdout);
    if (!pluginHandle)
        return 16;
    printf("BASS_WINAMP_PLUGIN_NAME=%s extensions=%s\n",name ? name(pluginHandle) : "(unavailable)",extensions ? extensions(pluginHandle) : "(unavailable)");
    fflush(stdout);
    DWORD stream=create(file,0x200000);
    printf("BASS_WINAMP_STREAM=%u bass_error=%d\n",stream,bassError());
    fflush(stdout);
    if (!stream)
        return 17;
    std::vector<unsigned char> pcm(65536);
    DWORD bytes=0;
    for (int attempt=0;attempt<35;attempt++)
    {
        bytes=bassData(stream,pcm.data(),static_cast<DWORD>(pcm.size()));
        if (bytes>0 && bytes!=0xFFFFFFFF) break;
        if (attempt==0 || attempt==10 || attempt==20)
            printf("BASS_WAIT attempt=%d count=%u error=%d\n",attempt,bytes,bassError());
        Sleep(100);
    }
    printf("BASS_WINAMP_PCM_BYTES=%u bass_error=%d length=%llu first=%d\n",bytes,bassError(),bassLength ? static_cast<unsigned long long>(bassLength(stream,0)) : 0ull,pcm[0]);
    fflush(stdout);
    if (bassChanFree)
        bassChanFree(stream);
    if (unload)
        unload(pluginHandle);
    bassFree();
    return bytes>0 && bytes!=0xFFFFFFFF ? 0 : 18;
}
