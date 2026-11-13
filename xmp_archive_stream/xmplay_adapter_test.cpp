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
    if(argc!=3)
    {
        puts("Usage: xmplay_adapter_test xmp-archive-stream.dll archive.7z|zip|rar");
        return 2;
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
    if(!api || !api->CheckFile || !api->GetFileList || !api->DecompressFile)
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
    fclose(file.handle);
    FreeLibrary(module);
    printf("PASS XMPlay archive callback entries=%d\n",checked);
    return checked?0:1;
}
