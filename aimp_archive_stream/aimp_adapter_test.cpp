// Minimal AIMP 5.40 host contract verifier. Uses the published COM interfaces
// with a mock object factory to validate registration, expansion and streams.
#define NOMINMAX
#include <windows.h>
#include <apiPlugin.h>
#include <apiFileManager.h>
#include <apiObjects.h>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

class Counter
{
public:
    ULONG Add() { return ++refs_; }
    ULONG Sub() { return --refs_; }
private:
    std::atomic<ULONG> refs_{1};
};

class TestString final : public IAIMPString
{
public:
    explicit TestString(std::wstring value = {}) : text_(std::move(value)) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!IsEqualIID(iid,IID_IUnknown) && !IsEqualIID(iid,IID_IAIMPString)) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return ref_.Add(); }
    ULONG WINAPI Release() override { auto c=ref_.Sub(); if(!c) delete this; return c; }
    HRESULT WINAPI GetChar(int index, TChar* c) override { if(!c || index<0 || index >= GetLength()) return E_INVALIDARG; *c=text_[index]; return S_OK; }
    TChar* WINAPI GetData() override { return text_.data(); }
    int WINAPI GetLength() override { return static_cast<int>(text_.size()); }
    int WINAPI GetHashCode() override { return 0; }
    HRESULT WINAPI SetChar(int i,TChar c) override { if(i<0 || i>=GetLength()) return E_INVALIDARG; text_[i]=c; return S_OK; }
    HRESULT WINAPI SetData(TChar* str,int n) override { if(n<0 || (n>0 && !str))return E_INVALIDARG; text_.assign(str,static_cast<size_t>(n));return S_OK; }
    HRESULT WINAPI Add(IAIMPString* s) override { if(!s)return E_POINTER; text_.append(s->GetData(),s->GetLength());return S_OK; }
    HRESULT WINAPI Add2(TChar* p,int n) override {if(n<0 || (n>0 && !p))return E_INVALIDARG;text_.append(p,n);return S_OK;}
    HRESULT WINAPI ChangeCase(int) override {return E_NOTIMPL;}
    HRESULT WINAPI Clone(IAIMPString** out) override {if(!out)return E_POINTER;*out=new TestString(text_);return S_OK;}
    HRESULT WINAPI Compare(IAIMPString*,int*,BOOL) override {return E_NOTIMPL;}
    HRESULT WINAPI Compare2(TChar*,int,int*,BOOL) override {return E_NOTIMPL;}
    HRESULT WINAPI Delete(int,int) override {return E_NOTIMPL;}
    HRESULT WINAPI Find(IAIMPString*,int*,int,int) override {return E_NOTIMPL;}
    HRESULT WINAPI Find2(TChar*,int,int*,int,int) override {return E_NOTIMPL;}
    HRESULT WINAPI Insert(int,IAIMPString*) override {return E_NOTIMPL;}
    HRESULT WINAPI Insert2(int,TChar*,int) override {return E_NOTIMPL;}
    HRESULT WINAPI Replace(IAIMPString*,IAIMPString*,int) override {return E_NOTIMPL;}
    HRESULT WINAPI Replace2(TChar*,int,TChar*,int,int) override {return E_NOTIMPL;}
    HRESULT WINAPI SubString(int,int,IAIMPString**) override {return E_NOTIMPL;}
private:
    Counter ref_;
    std::wstring text_;
};

class TestList final : public IAIMPObjectList
{
public:
    ~TestList() { for(auto* p:items_)p->Release(); }
    HRESULT WINAPI QueryInterface(REFIID iid,void** out) override
    {
        if(!out)return E_POINTER; *out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown) && !IsEqualIID(iid,IID_IAIMPObjectList))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override{return ref_.Add();}
    ULONG WINAPI Release() override{auto c=ref_.Sub();if(!c)delete this;return c;}
    HRESULT WINAPI Add(IUnknown* obj) override{if(!obj)return E_POINTER;obj->AddRef();items_.push_back(obj);return S_OK;}
    HRESULT WINAPI Clear() override{for(auto* p:items_)p->Release();items_.clear();return S_OK;}
    HRESULT WINAPI Delete(int) override{return E_NOTIMPL;}
    HRESULT WINAPI Insert(int,IUnknown*) override{return E_NOTIMPL;}
    int WINAPI GetCount() override{return static_cast<int>(items_.size());}
    HRESULT WINAPI GetObject(int index,REFIID iid,void** out) override
    {
        if(index<0 || static_cast<size_t>(index)>=items_.size())return E_INVALIDARG;
        return items_[index]->QueryInterface(iid,out);
    }
    HRESULT WINAPI SetObject(int,IUnknown*) override{return E_NOTIMPL;}
private:
    Counter ref_;
    std::vector<IUnknown*> items_;
};

class TestCore final : public IAIMPCore
{
public:
    HRESULT WINAPI QueryInterface(REFIID iid,void** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;
        if(!IsEqualIID(iid,IID_IUnknown) && !IsEqualIID(iid,IID_IAIMPCore))return E_NOINTERFACE;
        *out=this;AddRef();return S_OK;
    }
    ULONG WINAPI AddRef() override {return ref_.Add();}
    ULONG WINAPI Release() override{return ref_.Sub();}
    HRESULT WINAPI CreateObject(REFIID iid,void** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;
        if(IsEqualIID(iid,IID_IAIMPString))*out=static_cast<IAIMPString*>(new TestString());
        else if(IsEqualIID(iid,IID_IAIMPObjectList))*out=static_cast<IAIMPObjectList*>(new TestList());
        else return E_NOINTERFACE;
        return S_OK;
    }
    HRESULT WINAPI GetPath(int,IAIMPString**) override{return E_NOTIMPL;}
    HRESULT WINAPI RegisterExtension(REFIID service,IUnknown* ext) override
    {
        IUnknown** destination=nullptr;
        if(IsEqualIID(service,IID_IAIMPServiceFileManager)) destination=&extension_;
        else if(IsEqualIID(service,IID_IAIMPServiceFileFormats)) destination=&formatExtension_;
        if(!destination || !ext || *destination)return E_INVALIDARG;
        *destination=ext;
        ext->AddRef();
        return S_OK;
    }
    HRESULT WINAPI RegisterService(IUnknown*) override{return E_NOTIMPL;}
    HRESULT WINAPI UnregisterExtension(IUnknown* ext) override
    {
        if(ext==extension_){extension_->Release();extension_=nullptr;return S_OK;}
        if(ext==formatExtension_){formatExtension_->Release();formatExtension_=nullptr;return S_OK;}
        return E_INVALIDARG;
    }
    IUnknown* extension_=nullptr;
    IUnknown* formatExtension_=nullptr;
private:
    Counter ref_;
};

int wmain(int argc,wchar_t** argv)
{
    const bool faultTest = argc == 5 && wcscmp(argv[3], L"faulttest") == 0;
    if(argc != 3 && !faultTest)
    {
        puts("Usage: aimp_adapter_test.exe <plugin.dll> <archive.zip|7z|rar> [faulttest original-audio-path]");
        return 2;
    }

    std::vector<unsigned char> expected;
    std::string faultMode;
    if (faultTest)
    {
        std::ifstream source(std::filesystem::path(argv[4]), std::ios::binary);
        if (!source)
        {
            puts("FAIL cannot open original audio for fault parity");
            return 1;
        }
        expected.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
        char mode[16]{};
        if (!GetEnvironmentVariableA("AST_AIMP_FAULT_MODE", mode, sizeof(mode)) ||
            (strcmp(mode, "read") != 0 && strcmp(mode, "seek") != 0) || expected.size() < 250000)
        {
            puts("FAIL fault mode or audio fixture invalid");
            return 1;
        }
        faultMode = mode;
    }

    HMODULE module=LoadLibraryW(argv[1]);
    if(!module)
    {
        printf("FAIL LoadLibrary Win32=%lu\n",GetLastError());
        return 1;
    }
    using EntryPoint=HRESULT(WINAPI*)(IAIMPPlugin**);
    const auto get=reinterpret_cast<EntryPoint>(GetProcAddress(module,"AIMPPluginGetHeader"));
    if(!get)
    {
        puts("FAIL AIMPPluginGetHeader export missing");
        return 1;
    }
    using FaultCounter=unsigned(__cdecl*)();
    const auto injected = faultTest ? reinterpret_cast<FaultCounter>(GetProcAddress(module, "ASTNativeFaultTestGetInjectedCount")) : nullptr;
    const auto recovered = faultTest ? reinterpret_cast<FaultCounter>(GetProcAddress(module, "ASTNativeFaultTestGetRecoveredCount")) : nullptr;
    if (faultTest && (!injected || !recovered))
    {
        puts("FAIL test-only DLL fault counter exports missing");
        return 1;
    }

    IAIMPPlugin* plugin=nullptr;
    if(FAILED(get(&plugin))||!plugin)
    {
        puts("FAIL construct AIMP plugin");
        return 1;
    }
    TestCore core;
    if(FAILED(plugin->Initialize(&core)) || !core.extension_ || !core.formatExtension_)
    {
        puts("FAIL AIMP expander and format registration");
        return 1;
    }
    IAIMPExtensionFileFormat* format=nullptr;
    if(FAILED(core.formatExtension_->QueryInterface(IID_IAIMPExtensionFileFormat,reinterpret_cast<void**>(&format))) || !format)
    {
        puts("FAIL AIMP file format interface");return 1;
    }
    IAIMPString* formats=nullptr;
    LongWord flags=0;
    if(FAILED(format->GetExtList(&formats)) || !formats ||
       std::wstring(formats->GetData(),formats->GetLength()).find(L"*.7z;")==std::wstring::npos ||
       FAILED(format->GetFlags(&flags)) || !(flags & AIMP_SERVICE_FILEFORMATS_CATEGORY_AUDIO))
    {
        puts("FAIL AIMP supported archive formats");return 1;
    }
    formats->Release();
    format->Release();
    IAIMPExtensionFileExpander* expander=nullptr;
    if(FAILED(core.extension_->QueryInterface(IID_IAIMPExtensionFileExpander,reinterpret_cast<void**>(&expander))) || !expander)
    {
        puts("FAIL expander interface");
        return 1;
    }
    auto* path=new TestString(argv[2]);
    IAIMPObjectList* files=nullptr;
    const HRESULT expand=expander->Expand(path,&files,nullptr);
    path->Release();
    expander->Release();
    if(FAILED(expand) || !files || files->GetCount()<1)
    {
        printf("FAIL expansion status=0x%08lX items=%d\n",static_cast<unsigned long>(expand),files?files->GetCount():0);
        return 1;
    }
    int checked=0;
    for(int n=0;n<files->GetCount();++n)
    {
        IAIMPVirtualFile* file=nullptr;
        if(FAILED(files->GetObject(n,IID_IAIMPVirtualFile,reinterpret_cast<void**>(&file)))||!file)
        {
            puts("FAIL returned virtual file");return 1;
        }
        IAIMPString* uri=nullptr;
        if(FAILED(file->GetValueAsObject(AIMP_VIRTUALFILE_PROPID_FILEURI,IID_IAIMPString,reinterpret_cast<void**>(&uri))) ||
           !uri || std::wstring(uri->GetData(),uri->GetLength()).find(std::wstring(argv[2])+L":")!=0)
        {
            puts("FAIL AIMP canonical virtual-file URI");return 1;
        }
        uri->Release();
        IAIMPString* source=nullptr;
        if(file->GetValueAsObject(AIMP_VIRTUALFILE_PROPID_AUDIOSOURCEFILE,IID_IAIMPString,reinterpret_cast<void**>(&source))!=E_NOTIMPL ||
           FAILED(file->Synchronize()) || !file->IsExists())
        {
            puts("FAIL AIMP virtual file properties");return 1;
        }
        IAIMPStream* stream=nullptr;
        if(FAILED(file->CreateStream(&stream))||!stream)
        {
            printf("FAIL stream index=%d\n",n);return 1;
        }
        if(stream->GetSize()<64 || stream->GetPosition()!=0)
        {
            printf("FAIL stream size or start position index=%d\n",n);return 1;
        }
        if (faultTest)
        {
            if (stream->GetSize() != static_cast<INT64>(expected.size()))
            {
                puts("FAIL fault stream size does not match source");
                return 1;
            }

            std::vector<unsigned char> block(65536);
            if (stream->Read(block.data(), static_cast<unsigned>(block.size())) != static_cast<int>(block.size()) ||
                !std::equal(block.begin(), block.end(), expected.begin()))
            {
                puts("FAIL fault initial stream-byte parity");
                return 1;
            }

            size_t compareAt = 65536;
            if (faultMode == "seek")
            {
                compareAt = 12345;
                if (FAILED(stream->Seek(static_cast<INT64>(compareAt), AIMP_STREAM_SEEKMODE_FROM_BEGINNING)) ||
                    stream->GetPosition() != static_cast<INT64>(compareAt))
                {
                    puts("FAIL injected seek recovery or position");
                    return 1;
                }
            }

            const int actual = stream->Read(block.data(), static_cast<unsigned>(block.size()));
            if (actual != static_cast<int>(block.size()) ||
                !std::equal(block.begin(), block.end(), expected.begin() + compareAt) ||
                stream->GetPosition() != static_cast<INT64>(compareAt + block.size()))
            {
                puts("FAIL recovered read data or position");
                return 1;
            }

            if (injected() != 1 || recovered() != 1)
            {
                printf("FAIL fault injection not exercised injected=%u recovered=%u\n", injected(), recovered());
                return 1;
            }

            if (FAILED(stream->Seek(-65536, AIMP_STREAM_SEEKMODE_FROM_END)) ||
                stream->Read(block.data(), static_cast<unsigned>(block.size())) != static_cast<int>(block.size()) ||
                !std::equal(block.begin(), block.end(), expected.end() - block.size()))
            {
                puts("FAIL recovered stream end-seek parity");
                return 1;
            }
            printf("PASS AIMP native fault %s injected=%u recovered=%u byte_parity=ok seek=ok\n", faultMode.c_str(), injected(), recovered());
            stream->Release();
            file->Release();
            ++checked;
            break;
        }

        unsigned char first[64]{},next[64]{};
        const int got=stream->Read(first,64);
        if(got!=64 || stream->GetPosition()!=64)
        {
            printf("FAIL initial read index=%d got=%d\n",n,got);return 1;
        }
        if(FAILED(stream->Seek(0,AIMP_STREAM_SEEKMODE_FROM_BEGINNING))||
           stream->Read(next,64)!=64 || memcmp(first,next,64)!=0)
        {
            printf("FAIL decoded rewind byte parity index=%d\n",n);return 1;
        }
        if(FAILED(stream->Seek(-64,AIMP_STREAM_SEEKMODE_FROM_END)) ||
           stream->Read(next,64)!=64 || stream->GetPosition()!=stream->GetSize())
        {
            printf("FAIL end seek index=%d\n",n);return 1;
        }
        printf("PASS AIMP virtual stream index=%d size=%lld seek=PASS\n",n,(long long)stream->GetSize());
        stream->Release();
        file->Release();
        checked++;
    }
    files->Release();
    plugin->Finalize();
    plugin->Release();
    FreeLibrary(module);
    printf("PASS AIMP plugin contract expanded=%d streamed=%d\n",checked,checked);
    return 0;
}
