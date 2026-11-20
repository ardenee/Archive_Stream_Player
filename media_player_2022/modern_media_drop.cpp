// media_player_2022/modern_media_drop.cpp
// Standalone OLE archive drop target for Microsoft's Media Player (2022).
// Does not inject into the packaged application or replace its drop handler.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <ole2.h>
#include <shlobj_core.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr wchar_t kClass[] = L"ArchiveStreamModernMediaDrop";
constexpr wchar_t kFamily[] = L"Microsoft.ZuneMusic_8wekyb3d8bbwe";
constexpr wchar_t kAppId[] = L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic";
constexpr UINT kStatusMessage = WM_APP + 18;
constexpr UINT kArchiveMessage = WM_APP + 19;
HWND g_window = nullptr;
std::wstring g_status = L"Drop a ZIP, 7z or RAR music archive here.";
std::thread g_worker;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_closing{false};
std::mutex g_sessionMutex;
std::wstring g_exeDirectory;
HANDLE g_loopbackHelper = nullptr;
HANDLE g_bridgeJob = nullptr;

struct Session
{
    HANDLE bridge = nullptr;
    DWORD pid = 0;
    std::wstring directory;
    std::wstring shutdownUrl;
};
std::vector<Session> g_sessions;

void Log(const wchar_t* event, const std::wstring& details = {})
{
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    std::wstring root = std::wstring(local) + L"\\ArchiveStreamPlayer";
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring folder = root + L"\\Logs";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring line = L"event=" + std::wstring(event) + L" | pid=" + std::to_wstring(GetCurrentProcessId()) + L" | " + details + L"\r\n";
    int needed = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return;
    std::string bytes(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), bytes.data(), needed, nullptr, nullptr);
    HANDLE file = CreateFileW((folder + L"\\modern_media_drop.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void Status(const std::wstring& text)
{
    if (g_window && !g_closing)
        PostMessageW(g_window, kStatusMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
    Log(L"STATUS", text);
}

std::wstring Quote(const std::wstring& arg)
{
    return L"\"" + arg + L"\"";
}

std::wstring ModulePath()
{
    std::vector<wchar_t> buffer(32768);
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return n && n < buffer.size() ? std::wstring(buffer.data(), n) : L"";
}

std::wstring TempRoot()
{
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(local) + L"\\ArchiveStreamPlayer\\ModernMediaPlayer" : L"";
}

std::wstring UniqueSessionDirectory()
{
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return {};
    wchar_t name[64]{};
    swprintf_s(name, L"%08x%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
        id.Data1, id.Data2, id.Data3, id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
        id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
    const std::wstring root = TempRoot();
    if (root.empty())
        return {};
    std::wstring parent = root.substr(0, root.find_last_of(L'\\'));
    CreateDirectoryW(parent.c_str(), nullptr);
    CreateDirectoryW(root.c_str(), nullptr);
    const std::wstring directory = root + L"\\" + name;
    return CreateDirectoryW(directory.c_str(), nullptr) ? directory : L"";
}

bool SupportedArchive(const std::wstring& file)
{
    if (file.empty())
        return false;
    DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    const size_t dot = file.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return false;
    std::wstring extension = file.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".zip" || extension == L".7z" || extension == L".rar";
}

bool ExecuteHidden(const std::wstring& executable, const std::wstring& args, DWORD& exitCode, std::string* output = nullptr)
{
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring command = Quote(executable) + L" " + args;
    BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!started)
    {
        CloseHandle(readPipe);
        return false;
    }
    // Drain stdout while the subprocess runs. Waiting for exit before
    // reading may deadlock when CheckNetIsolation lists many exemptions.
    const ULONGLONG began = GetTickCount64();
    DWORD waited = WAIT_TIMEOUT;
    while (GetTickCount64() - began < 7000)
    {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[2048]{};
            const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
            DWORD read = 0;
            if (ReadFile(readPipe, buffer, want, &read, nullptr) && read && output)
                output->append(buffer, read);
            continue;
        }
        waited = WaitForSingleObject(pi.hProcess, 30);
        if (waited == WAIT_OBJECT_0)
            break;
    }
    if (waited != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    char buffer[2048]{};
    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available)
    {
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &read, nullptr) || !read)
            break;
        if (output)
            output->append(buffer, read);
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return waited == WAIT_OBJECT_0;
}

std::wstring CheckNetIsolation()
{
    wchar_t windows[MAX_PATH]{};
    if (!GetSystemDirectoryW(windows, MAX_PATH))
        return {};
    return std::wstring(windows) + L"\\CheckNetIsolation.exe";
}

bool ExemptionPresent()
{
    std::string output;
    DWORD exitCode = 1;
    if (!ExecuteHidden(CheckNetIsolation(), L"LoopbackExempt -s", exitCode, &output) || exitCode != 0)
        return false;
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return output.find("microsoft.zunemusic_8wekyb3d8bbwe") != std::string::npos;
}

bool ModifyExemption(bool enable)
{
    DWORD code = 1;
    std::wstring command = L"LoopbackExempt ";
    command += enable ? L"-a " : L"-d ";
    command += L"-n=";
    command += kFamily;
    if (!ExecuteHidden(CheckNetIsolation(), command, code))
        return false;
    return code == 0 && ExemptionPresent() == enable;
}

int Helper(DWORD parentPid, const std::wstring& eventName)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    if (!parent || !ready)
    {
        if (ready)
            CloseHandle(ready);
        if (parent)
            CloseHandle(parent);
        return 2;
    }
    bool created = false;
    const bool already = ExemptionPresent();
    if (!already)
        created = ModifyExemption(true);
    if (!already && !created)
    {
        Log(L"LOOPBACK_ENABLE_FAILED");
        CloseHandle(parent);
        CloseHandle(ready);
        return 3;
    }
    Log(created ? L"LOOPBACK_ENABLED_FOR_SESSION" : L"LOOPBACK_PREEXISTING");
    SetEvent(ready);
    CloseHandle(ready);
    // The elevated helper owns the change and removes it even if the UI crashes.
    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
    if (created)
    {
        if (ModifyExemption(false))
            Log(L"LOOPBACK_RESTORED");
        else
            Log(L"LOOPBACK_RESTORE_FAILED");
    }
    return 0;
}

bool EnsureLoopback()
{
    if (ExemptionPresent())
    {
        Status(L"Local streaming access is enabled.");
        return true;
    }
    Status(L"Requesting Windows permission for local streaming...");
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return false;
    wchar_t eventName[100]{};
    swprintf_s(eventName, L"Local\\ASTMediaPlayerLoopback-%lu-%08x", GetCurrentProcessId(), id.Data1);
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (!event)
        return false;
    std::wstring command = L"--loopback-helper " + std::to_wstring(GetCurrentProcessId()) + L" " + Quote(eventName);
    const std::wstring self = ModulePath();
    SHELLEXECUTEINFOW exec{};
    exec.cbSize = sizeof(exec);
    exec.fMask = SEE_MASK_NOCLOSEPROCESS;
    exec.lpVerb = L"runas";
    exec.lpFile = self.c_str();
    exec.lpParameters = command.c_str();
    exec.nShow = SW_HIDE;
    bool success = false;
    if (ShellExecuteExW(&exec))
    {
        HANDLE waiters[]{event, exec.hProcess};
        const DWORD wait = WaitForMultipleObjects(2, waiters, FALSE, 30000);
        success = wait == WAIT_OBJECT_0 && ExemptionPresent();
        if (success)
        {
            g_loopbackHelper = exec.hProcess;
            Log(L"LOOPBACK_TEMP_PERMISSION_READY");
        }
        else
        {
            Log(L"LOOPBACK_PERMISSION_DENIED_OR_TIMEOUT");
            CloseHandle(exec.hProcess);
        }
    }
    else
    {
        Log(L"LOOPBACK_UAC_CANCELLED", std::to_wstring(GetLastError()));
    }
    CloseHandle(event);
    return success;
}

bool ActivatePlaylist(const std::wstring& playlist)
{
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return false;
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(playlist.c_str(), nullptr, IID_PPV_ARGS(&item));
    IShellItemArray* array = nullptr;
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromShellItem(item, IID_PPV_ARGS(&array));
    if (item)
        item->Release();
    IApplicationActivationManager* manager = nullptr;
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (SUCCEEDED(hr))
    {
        DWORD pid = 0;
        hr = manager->ActivateForFile(kAppId, array, L"open", &pid);
        Log(L"MODERN_PLAYER_ACTIVATE", L"pid=" + std::to_wstring(pid) + L" hr=" + std::to_wstring(static_cast<unsigned>(hr)));
    }
    if (manager)
        manager->Release();
    if (array)
        array->Release();
    CoUninitialize();
    return SUCCEEDED(hr);
}

bool StartBridge(const std::wstring& archive, Session& session, std::wstring& playlist)
{
    std::wstring root = g_exeDirectory + L"\\archive_player_bridge.exe";
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES)
        root = g_exeDirectory + L"\\..\\..\\..\\..\\archive_player_bridge\\x64\\Release\\archive_player_bridge.exe";
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Status(L"Archive HTTP backend is missing. Build its x64 Release target.");
        return false;
    }
    session.directory = UniqueSessionDirectory();
    if (session.directory.empty())
        return false;
    const std::wstring output = session.directory + L"\\bridge-output.txt";
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE file = CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = file;
    si.hStdError = file;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // No bounded lifetime while the drop target is open; closing it sends /shutdown.
    std::wstring command = Quote(root) + L" --archive " + Quote(archive) +
        L" --playlist-dir " + Quote(session.directory) + L" --prompt-password 1";
    BOOL ok = CreateProcessW(root.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, g_exeDirectory.c_str(), &si, &pi);
    CloseHandle(file);
    if (!ok)
    {
        Status(L"Failed to start the archive streaming backend.");
        return false;
    }
    // The bridge must never outlive its owner, even after a forced termination.
    // Assign while suspended, before any archive worker threads can start.
    if (!g_bridgeJob || !AssignProcessToJobObject(g_bridgeJob, pi.hProcess) || ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
    {
        Log(L"BRIDGE_JOB_ASSIGN_FAILED", std::to_wstring(GetLastError()));
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Status(L"Could not establish safe archive bridge ownership.");
        return false;
    }
    session.bridge = pi.hProcess;
    session.pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    std::wstring text;
    for (int attempt = 0; attempt < 120 && !g_closing; ++attempt)
    {
        HANDLE reader = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reader != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER length{};
            if (GetFileSizeEx(reader, &length) && length.QuadPart > 0 && length.QuadPart < 131072)
            {
                std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
                DWORD got = 0;
                if (ReadFile(reader, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr))
                {
                    bytes.resize(got);
                    int chars = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
                    if (chars > 0)
                    {
                        text.assign(static_cast<size_t>(chars), L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), chars);
                    }
                }
            }
            CloseHandle(reader);
        }
        if (text.find(L"BRIDGE_READY=1") != std::wstring::npos)
        {
            std::wstring key = L"PLAYLIST_M3U8=";
            size_t pos = text.find(key);
            if (pos != std::wstring::npos)
            {
                pos += key.size();
                const size_t end = text.find_first_of(L"\r\n", pos);
                playlist = text.substr(pos, end == std::wstring::npos ? end : end - pos);
            }
            key = L"URL_M3U8=";
            pos = text.find(key);
            if (pos != std::wstring::npos)
            {
                pos += key.size();
                const size_t end = text.find_first_of(L"\r\n", pos);
                session.shutdownUrl = text.substr(pos, end == std::wstring::npos ? end : end - pos);
                const size_t suffix = session.shutdownUrl.rfind(L"playlist.m3u8");
                if (suffix != std::wstring::npos)
                    session.shutdownUrl.replace(suffix, 13, L"shutdown");
            }
            if (!playlist.empty() && !session.shutdownUrl.empty() &&
                GetFileAttributesW(playlist.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                Log(L"BRIDGE_READY", archive);
                return true;
            }
        }
        if (WaitForSingleObject(session.bridge, 0) == WAIT_OBJECT_0)
        {
            Log(L"BRIDGE_EARLY_EXIT", text.substr(0, 600));
            break;
        }
        Sleep(150);
    }
    Status(L"Could not prepare the archive playlist; see the bridge log.");
    return false;
}

void ShutdownBridge(Session& session)
{
    if (!session.shutdownUrl.empty())
    {
        // HTTP-only signal to our random-secret localhost bridge.
        const std::wstring& url = session.shutdownUrl;
        const size_t colon = url.find(L':', 7);
        const size_t slash = colon == std::wstring::npos ? std::wstring::npos : url.find(L'/', colon);
        if (colon != std::wstring::npos && slash != std::wstring::npos)
        {
            const int port = _wtoi(url.c_str() + colon + 1);
            if (port > 0 && port <= 65535)
            {
                WSADATA data{};
                if (!WSAStartup(MAKEWORD(2,2), &data))
                {
                    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (socketHandle != INVALID_SOCKET)
                    {
                        sockaddr_in addr{};
                        addr.sin_family = AF_INET;
                        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        addr.sin_port = htons(static_cast<unsigned short>(port));
                        if (!connect(socketHandle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
                        {
                            std::wstring route = url.substr(slash);
                            std::string ascii;
                            ascii.reserve(route.size());
                            for (wchar_t codepoint : route)
                                ascii.push_back(codepoint <= 127 ? static_cast<char>(codepoint) : '_');
                            std::string req = "POST " + ascii + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                            send(socketHandle, req.data(), static_cast<int>(req.size()), 0);
                        }
                        closesocket(socketHandle);
                    }
                    WSACleanup();
                }
            }
        }
    }
    if (session.bridge)
    {
        if (WaitForSingleObject(session.bridge, 5000) != WAIT_OBJECT_0)
        {
            TerminateProcess(session.bridge, 1);
            WaitForSingleObject(session.bridge, 1000);
        }
        CloseHandle(session.bridge);
        session.bridge = nullptr;
    }
    if (!session.directory.empty())
    {
        const std::wstring names[] = {L"archive-stream.m3u8", L"archive-stream.asx", L"bridge-output.txt"};
        for (const auto& name : names)
            DeleteFileW((session.directory + L"\\" + name).c_str());
        RemoveDirectoryW(session.directory.c_str());
    }
}

void ProcessArchive(const std::wstring archive)
{
    Status(L"Preparing " + archive.substr(archive.find_last_of(L"\\/") + 1) + L" ...");
    if (!EnsureLoopback())
    {
        Status(L"Loopback permission was not granted. Archive not opened.");
        g_busy = false;
        return;
    }
    Session session;
    std::wstring playlist;
    if (!StartBridge(archive, session, playlist))
    {
        ShutdownBridge(session);
        g_busy = false;
        return;
    }
    if (ActivatePlaylist(playlist))
    {
        {
            std::lock_guard<std::mutex> lock(g_sessionMutex);
            g_sessions.push_back(std::move(session));
        }
        Status(L"Playing in Media Player. Keep this drop window open while listening.");
    }
    else
    {
        ShutdownBridge(session);
        Status(L"Media Player could not open the generated playlist.");
    }
    g_busy = false;
}

bool QueueArchive(const std::wstring& archive)
{
    if (g_busy.exchange(true))
    {
        Status(L"An archive is already being prepared.");
        return false;
    }
    if (!SupportedArchive(archive))
    {
        Status(L"Drop one existing ZIP, 7z or RAR file.");
        g_busy = false;
        return false;
    }
    if (g_worker.joinable())
        g_worker.join();
    g_worker = std::thread(ProcessArchive, archive);
    return true;
}

class DropTarget final : public IDropTarget
{
    volatile LONG references_ = 1;
    bool valid_ = false;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ptr) override
    {
        if (!ptr)
            return E_POINTER;
        *ptr = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget))
        {
            *ptr = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = InterlockedDecrement(&references_);
        if (n == 0)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object, DWORD, POINTL, DWORD* effect) override
    {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        valid_ = false;
        STGMEDIUM medium{};
        if (object && SUCCEEDED(object->QueryGetData(&format)) && SUCCEEDED(object->GetData(&format, &medium)))
        {
            HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
            if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1)
            {
                UINT needed = DragQueryFileW(drop, 0, nullptr, 0);
                if (needed > 0 && needed < 32768)
                {
                    std::wstring path(static_cast<size_t>(needed + 1), L'\0');
                    if (DragQueryFileW(drop, 0, path.data(), needed + 1))
                    {
                        path.resize(needed);
                        valid_ = SupportedArchive(path);
                    }
                }
            }
            ReleaseStgMedium(&medium);
        }
        if (effect)
            *effect = valid_ ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override
    {
        if (effect)
            *effect = valid_ ? (*effect & DROPEFFECT_COPY) : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        valid_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* object, DWORD, POINTL, DWORD* effect) override
    {
        if (effect)
            *effect = DROPEFFECT_NONE;
        if (!valid_ || !object)
            return S_OK;
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        if (FAILED(object->GetData(&format, &medium)))
            return S_OK;
        HDROP drop = reinterpret_cast<HDROP>(medium.hGlobal);
        if (drop)
        {
            if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1)
            {
                UINT needed = DragQueryFileW(drop, 0, nullptr, 0);
                std::wstring archive(static_cast<size_t>(needed + 1), L'\0');
                if (DragQueryFileW(drop, 0, archive.data(), needed + 1))
                {
                    archive.resize(needed);
                    PostMessageW(g_window, kArchiveMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(archive)));
                    if (effect)
                        *effect = DROPEFFECT_COPY;
                }
            }
            else
                Status(L"Drop one archive at a time.");
        }
        ReleaseStgMedium(&medium);
        valid_ = false;
        return S_OK;
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message)
    {
    case kArchiveMessage:
    {
        auto* archive = reinterpret_cast<std::wstring*>(lp);
        if (archive)
        {
            QueueArchive(*archive);
            delete archive;
        }
        return 0;
    }
    case kStatusMessage:
    {
        auto* value = reinterpret_cast<std::wstring*>(lp);
        if (value)
        {
            g_status = *value;
            delete value;
            InvalidateRect(window, nullptr, TRUE);
        }
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT bounds{};
        GetClientRect(window, &bounds);
        HBRUSH background = CreateSolidBrush(RGB(22, 27, 35));
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        RECT panel{20, 18, bounds.right - 20, bounds.bottom - 22};
        HBRUSH fill = CreateSolidBrush(RGB(32, 40, 52));
        HPEN stroke = CreatePen(PS_SOLID, 2, RGB(75, 119, 162));
        HGDIOBJ oldFill = SelectObject(dc, fill);
        HGDIOBJ oldPen = SelectObject(dc, stroke);
        RoundRect(dc, panel.left, panel.top, panel.right, panel.bottom, 18, 18);
        SelectObject(dc, oldFill);
        SelectObject(dc, oldPen);
        DeleteObject(fill);
        DeleteObject(stroke);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(242, 246, 250));
        HFONT title = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, title);
        RECT titleRect{36, 56, bounds.right - 36, 107};
        DrawTextW(dc, L"Archive Stream  \u2192  Media Player", -1, &titleRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(title);
        SetTextColor(dc, RGB(185, 199, 214));
        HFONT normal = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        oldFont = SelectObject(dc, normal);
        RECT messageRect{38, 119, bounds.right - 38, bounds.bottom - 64};
        DrawTextW(dc, g_status.c_str(), -1, &messageRect, DT_CENTER | DT_WORDBREAK | DT_VCENTER);
        RECT footer{38, bounds.bottom - 65, bounds.right - 38, bounds.bottom - 37};
        SetTextColor(dc, RGB(151, 174, 195));
        DrawTextW(dc, L"ZIP  \u00b7  7Z  \u00b7  RAR     |     Close this window to stop managed streams", -1, &footer, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(normal);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CLOSE:
        g_closing = true;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wp, lp);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    // Parse the original command line with the Windows quoting rules.
    // Explorer and Start-Process can append whitespace after a quoted path.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 9;
    std::wstring startupArchive;
    std::wstring testDropArchive;
    if (argc == 4 && wcscmp(argv[1], L"--loopback-helper") == 0)
    {
        DWORD parent = wcstoul(argv[2], nullptr, 10);
        const std::wstring eventName = argv[3];
        LocalFree(argv);
        return parent ? Helper(parent, eventName) : 9;
    }
    if (argc == 3 && wcscmp(argv[1], L"--archive") == 0)
        startupArchive = argv[2];
    // Dragging a file onto the installed desktop/Start-menu shortcut passes
    // its path as the sole argument, without a custom --archive switch.
    if (argc == 2 && SupportedArchive(argv[1]))
        startupArchive = argv[1];
    if (argc == 3 && wcscmp(argv[1], L"--qa-ole-drop") == 0)
        testDropArchive = argv[2];
    LocalFree(argv);
    const std::wstring module = ModulePath();
    if (module.empty())
        return 3;
    g_exeDirectory = module.substr(0, module.find_last_of(L'\\'));
    g_bridgeJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!g_bridgeJob || !SetInformationJobObject(g_bridgeJob, JobObjectExtendedLimitInformation, &jobLimits, sizeof(jobLimits)))
    {
        if (g_bridgeJob)
            CloseHandle(g_bridgeJob);
        Log(L"BRIDGE_JOB_SETUP_FAILED", std::to_wstring(GetLastError()));
        return 10;
    }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole))
        return 4;
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = WindowProc;
    cls.lpszClassName = kClass;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&cls))
    {
        OleUninitialize();
        return 5;
    }
    g_window = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"Archive Stream - Media Player (2022)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 600, 302,
        nullptr, nullptr, instance, nullptr);
    if (!g_window)
    {
        OleUninitialize();
        return 6;
    }
    auto* dropTarget = new DropTarget();
    const HRESULT registered = RegisterDragDrop(g_window, dropTarget);
    if (FAILED(registered))
        Log(L"REGISTER_DROP_TARGET_FAILED", std::to_wstring(static_cast<unsigned>(registered)));
    ShowWindow(g_window, show == SW_HIDE ? SW_SHOW : show);
    UpdateWindow(g_window);
    if (!startupArchive.empty())
        PostMessageW(g_window, kArchiveMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(startupArchive)));
    // QA-only: exercise the exact OLE IDropTarget contract using a Shell IDataObject.
    // No hooks or synthetic input are installed in Media Player or Explorer.
    if (!testDropArchive.empty())
    {
        IShellItem* item = nullptr;
        IDataObject* payload = nullptr;
        HRESULT hr = SHCreateItemFromParsingName(testDropArchive.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr))
            hr = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&payload));
        if (item)
            item->Release();
        if (SUCCEEDED(hr) && payload)
        {
            DWORD effect = DROPEFFECT_COPY;
            POINTL pt{};
            hr = dropTarget->DragEnter(payload, 0, pt, &effect);
            if (SUCCEEDED(hr) && (effect & DROPEFFECT_COPY))
                hr = dropTarget->Drop(payload, 0, pt, &effect);
            else
                hr = E_FAIL;
            payload->Release();
        }
        Log(SUCCEEDED(hr) ? L"OLE_DROP_QA_PASS" : L"OLE_DROP_QA_FAILED", std::to_wstring(static_cast<unsigned>(hr)));
        if (FAILED(hr))
            Status(L"OLE shell-file drop self-test failed.");
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_closing = true;
    if (SUCCEEDED(registered))
        RevokeDragDrop(g_window);
    dropTarget->Release();
    if (g_worker.joinable())
        g_worker.join();
    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        for (auto& session : g_sessions)
            ShutdownBridge(session);
        g_sessions.clear();
    }
    if (g_loopbackHelper)
    {
        // Elevated helper removes only exemptions it created on our exit.
        CloseHandle(g_loopbackHelper);
    }
    OleUninitialize();
    if (g_bridgeJob)
        CloseHandle(g_bridgeJob);
    return 0;
}
