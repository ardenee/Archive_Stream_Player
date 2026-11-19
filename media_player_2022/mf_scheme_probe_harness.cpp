// media_player_2022/mf_scheme_probe_harness.cpp
// Desktop Media Foundation dispatch test, separate from modern Media Player.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* url = argc > 1 ? argv[1] : L"astprobe://archive-stream.test/track.mp3";
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        wprintf(L"COM_INIT_FAIL hr=0x%08X\n", static_cast<unsigned>(hr));
        return 2;
    }
    const HRESULT started = MFStartup(MF_VERSION);
    if (FAILED(started))
    {
        wprintf(L"MF_START_FAIL hr=0x%08X\n", static_cast<unsigned>(started));
        CoUninitialize();
        return 3;
    }
    const CLSID probeClass = {0x8E55B829, 0x7771, 0x4AD1, {0xA0,0xF2,0x3E,0x6C,0x29,0x4F,0xB7,0xD2}};
    IMFSchemeHandler* direct = nullptr;
    const HRESULT directHr = CoCreateInstance(probeClass, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFSchemeHandler), reinterpret_cast<void**>(&direct));
    wprintf(L"MF_DIRECT_COM_ACTIVATION hr=0x%08X object=%d\\n", static_cast<unsigned>(directHr), direct ? 1 : 0);
    if (direct)
        direct->Release();

    IMFSourceResolver* resolver = nullptr;
    hr = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(hr))
    {
        MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
        IUnknown* object = nullptr;
        const DWORD flags = MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_BYTESTREAM;
        hr = resolver->CreateObjectFromURL(url, flags, nullptr, &type, &object);
        wprintf(L"MF_SCHEME_RESOLVER_RESULT hr=0x%08X type=%d url=%ls\n", static_cast<unsigned>(hr), static_cast<int>(type), url);
        if (object)
            object->Release();
        resolver->Release();
    }
    else
    {
        wprintf(L"MF_CREATE_RESOLVER_FAIL hr=0x%08X\n", static_cast<unsigned>(hr));
    }
    MFShutdown();
    CoUninitialize();
    // Failure is expected: this is a dispatch-only scheme handler.
    return 0;
}
