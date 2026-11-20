// media_player_2022/mf_bytestream_probe_harness.cpp
// Desktop Media Foundation file-extension dispatch test, separate from modern Media Player.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* url = argc > 1 ? argv[1] : L"D:\\Temp\\AST-MediaPlayer2022-MFQA\\mf-probe-payload.astmfprobe";
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
    const CLSID probeClass = {0x9BCD35B6, 0x0DB6, 0x480B, {0x93,0xA1,0x34,0x50,0xEA,0x40,0xF0,0x16}};
    IMFByteStreamHandler* direct = nullptr;
    const HRESULT directHr = CoCreateInstance(probeClass, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFByteStreamHandler), reinterpret_cast<void**>(&direct));
    wprintf(L"MF_BYTE_DIRECT_COM_ACTIVATION hr=0x%08X object=%d\\n", static_cast<unsigned>(directHr), direct ? 1 : 0);
    if (direct)
        direct->Release();

    IMFSourceResolver* resolver = nullptr;
    hr = MFCreateSourceResolver(&resolver);
    if (SUCCEEDED(hr))
    {
        MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
        IUnknown* object = nullptr;
        const DWORD flags = MF_RESOLUTION_MEDIASOURCE | (argc >= 3 && wcscmp(argv[2], L"--allow-mismatch") == 0 ? MF_RESOLUTION_CONTENT_DOES_NOT_HAVE_TO_MATCH_EXTENSION_OR_MIME_TYPE : 0);
        hr = resolver->CreateObjectFromURL(url, flags, nullptr, &type, &object);
        wprintf(L"MF_BYTESTREAM_RESOLVER_RESULT hr=0x%08X type=%d url=%ls\n", static_cast<unsigned>(hr), static_cast<int>(type), url);
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
    // Failure is expected: this is a diagnostic byte-stream handler.
    return 0;
}
