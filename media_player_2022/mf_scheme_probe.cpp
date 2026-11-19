// media_player_2022/mf_scheme_probe.cpp
// Diagnostic-only Media Foundation scheme handler. Never returns media data.
#include <windows.h>
#include <mfidl.h>
#include <mferror.h>
#include <atomic>
#include <new>
#include <string>
#include <cstdio>

namespace
{
const CLSID kProbeClass = {0x8E55B829, 0x7771, 0x4AD1, {0xA0,0xF2,0x3E,0x6C,0x29,0x4F,0xB7,0xD2}};
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

void ProbeLog(const wchar_t* action, const wchar_t* url = L"")
{
    wchar_t local[MAX_PATH + 1]{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!size || size >= MAX_PATH)
        return;

    const std::wstring base = std::wstring(local) + L"\\ArchiveStreamPlayer";
    const std::wstring folder = base + L"\\Logs";
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring filename = folder + L"\\mf_scheme_probe.log";
    HANDLE file = CreateFileW(filename.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    SYSTEMTIME now{};
    GetSystemTime(&now);
    wchar_t line[1200]{};
    swprintf_s(line, L"%04u-%02u-%02uT%02u:%02u:%02uZ pid=%lu event=%ls url=%.*ls\r\n",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId(), action, 640, url ? url : L"");
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1)
    {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(file);
}

class ProbeHandler final : public IMFSchemeHandler
{
    std::atomic<ULONG> references_{1};
public:
    ProbeHandler()
    {
        ++g_objects;
        ProbeLog(L"HANDLER_CREATED");
    }
    ~ProbeHandler()
    {
        ProbeLog(L"HANDLER_RELEASED");
        --g_objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, __uuidof(IMFSchemeHandler)))
        {
            *result = static_cast<IMFSchemeHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --references_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE BeginCreateObject(LPCWSTR url, DWORD, IPropertyStore*, IUnknown** cookie, IMFAsyncCallback*, IUnknown*) override
    {
        if (cookie)
            *cookie = nullptr;
        ProbeLog(L"BEGIN_CREATE_OBJECT", url);
        // Intentionally fail: proves dispatch, never impersonates a playable source.
        return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    }
    HRESULT STDMETHODCALLTYPE EndCreateObject(IMFAsyncResult*, MF_OBJECT_TYPE* type, IUnknown** object) override
    {
        if (type)
            *type = MF_OBJECT_INVALID;
        if (object)
            *object = nullptr;
        ProbeLog(L"END_CREATE_OBJECT");
        return MF_E_UNSUPPORTED_BYTESTREAM_TYPE;
    }
    HRESULT STDMETHODCALLTYPE CancelObjectCreation(IUnknown*) override
    {
        ProbeLog(L"CANCEL_CREATE_OBJECT");
        return E_NOTIMPL;
    }
};

class ProbeFactory final : public IClassFactory
{
    std::atomic<ULONG> references_{1};
public:
    ProbeFactory() { ++g_objects; }
    ~ProbeFactory() { --g_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IClassFactory))
        {
            *result = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --references_;
        if (!count)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override
    {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto* handler = new(std::nothrow) ProbeHandler();
        if (!handler)
            return E_OUTOFMEMORY;
        HRESULT hr = handler->QueryInterface(iid, result);
        handler->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock)
            ++g_locks;
        else
            --g_locks;
        return S_OK;
    }
};
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result)
{
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (!IsEqualCLSID(clsid, kProbeClass))
        return CLASS_E_CLASSNOTAVAILABLE;
    ProbeLog(L"CLASS_FACTORY_REQUESTED");
    auto* factory = new(std::nothrow) ProbeFactory();
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, result);
    factory->Release();
    return hr;
}
STDAPI DllCanUnloadNow()
{
    return g_objects.load() == 0 && g_locks.load() == 0 ? S_OK : S_FALSE;
}
