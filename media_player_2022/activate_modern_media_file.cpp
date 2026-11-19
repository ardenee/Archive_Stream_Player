// media_player_2022/activate_modern_media_file.cpp
// Actual Microsoft.ZuneMusic ActivateForFile test; no player injection or hooks.
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>

static void Report(int argc, wchar_t** argv, const wchar_t* step, HRESULT hr, DWORD pid = 0)
{
    wprintf(L"MF_MODERN_ACTIVATE step=%ls hr=0x%08X pid=%lu\n", step, static_cast<unsigned>(hr), pid);
    if (argc >= 3 && argv[2] && *argv[2])
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, argv[2], L"a") == 0 && file)
        {
            fwprintf(file, L"step=%ls hr=0x%08X pid=%lu\n", step, static_cast<unsigned>(hr), pid);
            fclose(file);
        }
    }
}
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 1;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        Report(argc, argv, L"CoInitializeEx", hr);
        return 2;
    }
    IShellItem* item = nullptr;
    hr = SHCreateItemFromParsingName(argv[1], nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr))
    {
        Report(argc, argv, L"SHCreateItemFromParsingName", hr);
        CoUninitialize();
        return 3;
    }
    IShellItemArray* items = nullptr;
    hr = SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&items));
    item->Release();
    if (FAILED(hr))
    {
        Report(argc, argv, L"SHCreateShellItemArrayFromShellItem", hr);
        CoUninitialize();
        return 4;
    }
    IApplicationActivationManager* manager = nullptr;
    hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (SUCCEEDED(hr))
    {
        DWORD pid = 0;
        hr = manager->ActivateForFile(L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic", items, L"open", &pid);
        Report(argc, argv, L"ActivateForFile", hr, pid);
        manager->Release();
    }
    else
    {
        Report(argc, argv, L"CoCreateActivationManager", hr);
    }
    items->Release();
    CoUninitialize();
    return FAILED(hr) ? 5 : 0;
}
