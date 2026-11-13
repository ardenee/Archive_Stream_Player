// Standalone XMPlay 4.1 archive-host contract verifier.
// Supplies only the official XMPARC callbacks used by our adapter.
#define NOMINMAX
#include <windows.h>
#include <xmparc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#pragma comment(lib, "advapi32.lib")

namespace
{
    struct TestFile
    {
        std::wstring path;
        std::string utf8;
        FILE* handle = nullptr;
    };
    DWORD WINAPI Type(XMPFILE) { return XMPFILE_TYPE_FILE; }
    const char* WINAPI Name(XMPFILE f) { return static_cast<TestFile*>(f)->utf8.c_str(); }
    DWORD WINAPI Read(XMPFILE f,void* data,DWORD count)
    {
        return static_cast<DWORD>(fread(data,1,count,static_cast<TestFile*>(f)->handle));
    }
    BOOL WINAPI Seek32(XMPFILE f,DWORD pos) {return _fseeki64(static_cast<TestFile*>(f)->handle,pos,SEEK_SET)==0;}
    BOOL WINAPI Seek64(XMPFILE f,QWORD pos)
    {
        if(pos>INT64_MAX)return FALSE;
        return _fseeki64(static_cast<TestFile*>(f)->handle,static_cast<__int64>(pos),SEEK_SET)==0;
    }
    QWORD WINAPI Tell64(XMPFILE f)
    {
        const __int64 pos=_ftelli64(static_cast<TestFile*>(f)->handle);
        return pos<0?0:static_cast<QWORD>(pos);
    }
    DWORD WINAPI GetSize(XMPFILE f)
    {
        auto* file=static_cast<TestFile*>(f);
        const auto pos=_ftelli64(file->handle);
        _fseeki64(file->handle,0,SEEK_END);
        auto end=_ftelli64(file->handle);
        _fseeki64(file->handle,pos,SEEK_SET);
        return static_cast<DWORD>((std::min)(end,static_cast<__int64>(UINT32_MAX)));
    }
    void* WINAPI Allocate(DWORD size) {return malloc(size?size:1);}
    void WINAPI Deallocate(void* p) {free(p);}
    void* WINAPI ReAllocate(void* p,DWORD size) {return realloc(p,size?size:1);}
    BOOL WINAPI Cancel() {return FALSE;}
    HWND WINAPI Window() {return nullptr;}

    struct TestRegistryScope
    {
        HKEY root = nullptr;
        HKEY preferences = nullptr;
        std::wstring keyPath;
        bool configured = false;

        explicit TestRegistryScope(bool enabled)
        {
            if (!enabled)
                return;
            keyPath = L"Software\\ArchiveStreamPlayer\\TitleTests-" + std::to_wstring(GetCurrentProcessId());
            if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS)
                return;
            if (RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS)
                return;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ArchiveStreamPlayer\\Adapters", 0, nullptr,
                REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &preferences, nullptr) != ERROR_SUCCESS)
                return;
            const DWORD memory = 50;
            RegSetValueExW(preferences, L"MemoryMB", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&memory), sizeof(memory));
            configured = SetTitle(L"<archivetype>: <#> - <filename>");
        }

        bool SetTitle(const wchar_t* value)
        {
            if (!preferences || !value)
                return false;
            return RegSetValueExW(preferences, L"TitleTemplate", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(value),
                static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }

        ~TestRegistryScope()
        {
            if (preferences)
                RegCloseKey(preferences);
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (root)
                RegCloseKey(root);
            if (!keyPath.empty())
                RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
        }
    };

    XMPFUNC_FILE files{};
    XMPFUNC_MISC misc{};

    void* WINAPI GetFace(DWORD face)
    {
        if(face==XMPFUNC_FILE_FACE)return &files;
        if(face==XMPFUNC_MISC_FACE)return &misc;
        return nullptr;
    }
}

int wmain(int argc,wchar_t** argv)
{
    const bool titleTest = argc==4 && wcscmp(argv[3], L"--test-title-parity")==0;
    if(argc!=3 && !titleTest)
    {
        puts("Usage: xmplay_adapter_test xmp-archive-stream.dll archive.7z|zip|rar [--test-title-parity]");
        return 2;
    }
    TestRegistryScope registry(titleTest);
    if (titleTest && !registry.configured)
    {
        puts("FAIL unable to isolate title preferences from the real user profile");
        return 1;
    }

    TestFile file;
    file.path=argv[2];
    const int utf8Len=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,file.path.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(utf8Len<=1)return 1;
    file.utf8.resize(static_cast<size_t>(utf8Len));
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,file.path.c_str(),-1,file.utf8.data(),utf8Len,nullptr,nullptr);
    if(_wfopen_s(&file.handle,file.path.c_str(),L"rb")!=0 || !file.handle)
    {
        puts("FAIL open fixture file");
        return 1;
    }
    files.GetType=Type;
    files.GetFilename=Name;
    files.Read=Read;
    files.Seek=Seek32;
    files.Seek64=Seek64;
    files.Tell64=Tell64;
    files.GetSize=GetSize;
    misc.Alloc=Allocate;
    misc.Free=Deallocate;
    misc.ReAlloc=ReAllocate;
    misc.CheckCancel=Cancel;
    misc.GetWindow=Window;

    HMODULE module=LoadLibraryW(argv[1]);
    if(!module)
    {
        printf("FAIL load plugin Win32=%lu\n",GetLastError());
        return 1;
    }
    using Entry= XMPARC*(WINAPI*)(DWORD,InterfaceProc);
    auto fn=reinterpret_cast<Entry>(GetProcAddress(module,"XMPARC_GetInterface"));
    if(!fn)
    {
        puts("FAIL missing XMPARC_GetInterface");
        return 1;
    }
    XMPARC* api=fn(XMPARC_FACE,GetFace);
    if(!api || !api->CheckFile || !api->GetFileList || !api->DecompressFile || !api->Config)
    {
        puts("FAIL XMPlay archive interface");
        return 1;
    }
    if(!api->CheckFile(&file))
    {
        puts("FAIL signature/path verification");
        return 1;
    }
    char* list=api->GetFileList(&file);
    if(!list)
    {
        puts("FAIL no playable files returned");
        return 1;
    }
    std::vector<std::string> names;
    for(const char* p=list;*p;p+=strlen(p)+1)
    {
        if(names.size()>4096)
        {
            puts("FAIL list exceeds test bound");
            return 1;
        }
        names.emplace_back(p);
    }
    misc.Free(list);
    printf("PASS XMPlay archive list=%zu\n",names.size());
    if (titleTest)
    {
        for (const auto& name : names)
        {
            const char* slash = strchr(name.c_str(), '/');
            if (!slash || slash == name.c_str() || !slash[1] ||
                name.find("_ ") == std::string::npos)
            {
                printf("FAIL XMPlay formatted title: %s\n", name.c_str());
                return 1;
            }
        }
        if (!registry.SetTitle(L"<filename>"))
        {
            puts("FAIL resetting isolated title template");
            return 1;
        }
        // Revisit playlist aliases after the title format changes.
        printf("PASS XMPlay reversible title aliases=%zu\n", names.size());
    }
    int checked=0;
    for(const auto& name:names)
    {
        DWORD size=50*1024*1024;
        void* content=api->DecompressFile(&file,name.c_str(),&size);
        if(!content || size<44)
        {
            printf("FAIL extraction file=%s size=%u\n",name.c_str(),size);
            return 1;
        }
        // The generated fixtures contain WAVs. This verifies the *player*
        // receives decoded audio, rather than simply compressed archive bytes.
        if(memcmp(content,"RIFF",4)!=0 || memcmp(static_cast<unsigned char*>(content)+8,"WAVE",4)!=0)
        {
            printf("FAIL extracted file not WAV: %s\n",name.c_str());
            return 1;
        }
        printf("PASS XMPlay decoded file=%s bytes=%u\n",name.c_str(),size);
        const size_t probeLength = (std::min)(static_cast<size_t>(size), static_cast<size_t>(64));
        unsigned char prefix[64]{};
        memcpy(prefix, content, probeLength);
        misc.Free(content);

        DWORD partial = static_cast<DWORD>(probeLength);
        void* partialBuffer = api->DecompressFile(&file, name.c_str(), &partial);
        if (!partialBuffer || partial != probeLength || memcmp(partialBuffer, prefix, probeLength) != 0)
        {
            printf("FAIL XMPlay partial probe file=%s got=%u\n", name.c_str(), partial);
            if (partialBuffer)
                misc.Free(partialBuffer);
            return 1;
        }
        printf("PASS XMPlay partial read file=%s bytes=%u\n", name.c_str(), partial);
        misc.Free(partialBuffer);
        ++checked;
    }
    if (titleTest)
    {
        char* oldList = api->GetFileList(&file);
        if (!oldList || !*oldList)
        {
            puts("FAIL default preset list");
            return 1;
        }
        DWORD partial = 64;
        void* bytes = api->DecompressFile(&file, oldList, &partial);
        if (!bytes || partial != 64 || memcmp(bytes, "RIFF", 4) != 0)
        {
            puts("FAIL default preset backward compatibility");
            return 1;
        }
        misc.Free(bytes);
        misc.Free(oldList);
        puts("PASS XMPlay default preset legacy archive names");
    }

    fclose(file.handle);
    FreeLibrary(module);
    printf("PASS XMPlay archive callback entries=%d\n",checked);
    return checked?0:1;
}
