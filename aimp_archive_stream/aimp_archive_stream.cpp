// AIMP 5.40 native FileExpander / VirtualFile adapter.
// The archive engines are statically linked into archive_backend.lib.
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#include <apiPlugin.h>
#include <apiFileManager.h>
#include <apiObjects.h>
#include <apiOptions.h>
#include "archive_backend.h"
#include "archive_adapter_preferences.h"
#include "../shared/archive_unified_cache.h"
#include "../shared/archive_unified_warmer.h"
#include "archive_format.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "debug_log.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
static std::atomic<unsigned> g_aimpFaultInjected{0};
static std::atomic<unsigned> g_aimpFaultRecovered{0};
extern "C" __declspec(dllexport) unsigned __cdecl ASTNativeFaultTestGetInjectedCount()
{
    return g_aimpFaultInjected.load();
}
extern "C" __declspec(dllexport) unsigned __cdecl ASTNativeFaultTestGetRecoveredCount()
{
    return g_aimpFaultRecovered.load();
}
#endif

namespace
{
    IAIMPCore* g_core = nullptr;
    HMODULE g_module = nullptr;

    std::wstring Utf8ToWide(const std::string& value)
    {
        if (value.empty())
            return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.c_str(), -1, nullptr, 0);
        if (count <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.c_str(), -1, wide.data(), count) != count)
            return {};
        wide.pop_back();
        return wide;
    }

    std::wstring FileUri(const std::wstring& archive, const ArchiveEntry& entry)
    {
        // An indexed virtual URI avoids duplicate-path ambiguity while keeping
        // the media extension in the basename for AIMP's decoder selection.
        return archive + L":" + std::to_wstring(entry.index) + L"/" + Utf8ToWide(entry.path);
    }

    HRESULT NewAimpString(const std::wstring& value, IAIMPString** output)
    {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (!g_core)
            return E_FAIL;
        IAIMPString* text = nullptr;
        const HRESULT result = g_core->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&text));
        if (FAILED(result) || !text)
            return FAILED(result) ? result : E_FAIL;
        const HRESULT writeResult = text->SetData(const_cast<wchar_t*>(value.data()), static_cast<int>(value.size()));
        if (FAILED(writeResult))
        {
            text->Release();
            return writeResult;
        }
        *output = text;
        return S_OK;
    }

    bool AskPassword(const char* archive, const char*, char* output, size_t capacity, bool* save)
    {
        if (!output || capacity == 0)
            return false;
        output[0] = '\0';
        const auto preferences = ArchiveAdapterPreferencesLoad();
        if (!preferences.promptPasswords)
            return false;
        if (save)
            *save = false;
        CREDUI_INFOW ui{};
        ui.cbSize = sizeof(ui);
        ui.pszCaptionText = L"Archive Stream - AIMP Password";
        ui.pszMessageText = L"Enter archive password. Username is unused; Windows does not persist these credentials.";
        std::wstring target = Utf8ToWide(archive ? archive : "");
        if (target.empty())
            target = L"AIMP Archive Stream";
        wchar_t username[CREDUI_MAX_USERNAME_LENGTH + 1]{};
        wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
        BOOL persist = FALSE;
        DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST;
        const DWORD response = CredUIPromptForCredentialsW(&ui, target.c_str(), nullptr, 0,
            username, CREDUI_MAX_USERNAME_LENGTH + 1,
            password, CREDUI_MAX_PASSWORD_LENGTH + 1, &persist, flags);
        if (response != NO_ERROR)
        {
            SecureZeroMemory(password, sizeof(password));
            return false;
        }
        const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, nullptr, 0, nullptr, nullptr);
        bool valid = needed > 1 && static_cast<size_t>(needed) <= capacity &&
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1,
                output, static_cast<int>(capacity), nullptr, nullptr) == needed;
        SecureZeroMemory(password, sizeof(password));
        if (!valid)
            output[0] = '\0';
        if (valid && save && preferences.askToSavePassword)
            *save = MessageBoxW(nullptr, L"Save the password to the configured plaintext password list after it is verified?", L"Archive Stream - AIMP", MB_YESNO | MB_ICONQUESTION) == IDYES;
        return valid;
    }

    struct ComCounter
    {
        std::atomic<ULONG> references{1};
        ULONG Add() { return references.fetch_add(1, std::memory_order_relaxed) + 1; }
        ULONG Sub() { return references.fetch_sub(1, std::memory_order_acq_rel) - 1; }
    };

    // Each virtual track gets its own stream with deterministic ownership.
    class EntryStream final : public IAIMPStream
    {
    public:
        EntryStream(std::wstring archive, ArchiveEntry entry)
            : archive_(std::move(archive)), entry_(std::move(entry)) {}
        ~EntryStream()
        {
            if (backend_.impl)
                ArchiveCloseEntryStream(backend_);
            if (cache_ != INVALID_HANDLE_VALUE)
                CloseHandle(cache_);
            if (!cachePath_.empty() && !cachePersistent_)
                DeleteFileW(cachePath_.c_str());
        }

        HRESULT Open()
        {
            if (entry_.size == 0 || entry_.isFolder)
                return E_INVALIDARG;
            const bool solid = entry_.isSolid &&
                (DetectArchiveFormatFromPath(archive_.c_str()) == ArchiveFormat::SevenZip ||
                 DetectArchiveFormatFromPath(archive_.c_str()) == ArchiveFormat::Rar);
            std::string error;
            if (!solid && ArchiveOpenEntryStream(archive_.c_str(), entry_.index, backend_, error))
            {
                unsigned char sample[4096]{};
                const size_t wanted = static_cast<size_t>((std::min)(entry_.size, static_cast<uint64_t>(sizeof(sample))));
                if (ArchiveReadEntryStream(backend_, sample, wanted) == wanted &&
                    ArchiveSeekEntryStream(backend_, 0))
                {
                    mode_ = Mode::Stream;
                    DebugLogWrite("INFO", "AIMP", "ROUTE_STREAM", "index=%u | size=%llu", entry_.index, (unsigned long long)entry_.size);
                    return S_OK;
                }
                ArchiveCloseEntryStream(backend_);
            }

            return OpenFallback(error) ? S_OK : E_FAIL;
        }

        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IAIMPStream))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPStream*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            ULONG refs = refs_.Sub();
            if (!refs)
                delete this;
            return refs;
        }
        INT64 WINAPI GetSize() override { return static_cast<INT64>(entry_.size); }
        HRESULT WINAPI SetSize(const INT64) override { return E_NOTIMPL; }
        INT64 WINAPI GetPosition() override { return static_cast<INT64>(position_); }
        HRESULT WINAPI Seek(const INT64 offset, int origin) override
        {
            INT64 base = 0;
            switch (origin)
            {
            case AIMP_STREAM_SEEKMODE_FROM_BEGINNING: break;
            case AIMP_STREAM_SEEKMODE_FROM_CURRENT: base = static_cast<INT64>(position_); break;
            case AIMP_STREAM_SEEKMODE_FROM_END: base = static_cast<INT64>(entry_.size); break;
            default: return E_INVALIDARG;
            }
            if ((offset > 0 && base > INT64_MAX - offset) ||
                (offset < 0 && base < INT64_MIN - offset))
                return E_INVALIDARG;
            const INT64 target = base + offset;
            if (target < 0 || static_cast<uint64_t>(target) > entry_.size)
                return E_INVALIDARG;
            if (mode_ == Mode::Stream)
            {
                bool seekFailed = false;
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
                if (!faultInjected_ && position_ >= 65536 && FaultModeMatches("seek"))
                {
                    faultInjected_ = true;
                    ++g_aimpFaultInjected;
                    seekFailed = true;
                }
                else
#endif
                    seekFailed = !ArchiveSeekEntryStream(backend_, static_cast<uint64_t>(target));

                if (seekFailed && !RecoverFromStreamFailure("archive seek failed"))
                    return E_FAIL;
            }
            if (mode_ == Mode::Cache)
            {
                LARGE_INTEGER offset64{};
                offset64.QuadPart = target;
                if (!SetFilePointerEx(cache_, offset64, nullptr, FILE_BEGIN))
                    return E_FAIL;
            }
            position_ = static_cast<uint64_t>(target);
            return S_OK;
        }
        int WINAPI Read(unsigned char* buffer, unsigned int count) override
        {
            if (!buffer || mode_ == Mode::None)
                return -1;
            const uint64_t available = entry_.size - position_;
            const DWORD wanted = static_cast<DWORD>((std::min)(available, static_cast<uint64_t>(count)));
            if (!wanted)
                return 0;
            size_t delivered = 0;
            if (mode_ == Mode::Stream)
            {
                ArchiveStreamReadResult result;
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
                if (!faultInjected_ && position_ >= 65536 && FaultModeMatches("read"))
                {
                    faultInjected_ = true;
                    ++g_aimpFaultInjected;
                    result.errorKind = ArchiveStreamErrorKind::BackendFailure;
                    result.message = "Test-injected AIMP decoder read failure";
                }
                else
#endif
                    result = ArchiveReadEntryStreamDetailed(backend_, buffer, wanted);

                if (result.errorKind != ArchiveStreamErrorKind::None)
                {
                    if ((result.errorKind == ArchiveStreamErrorKind::BackendFailure ||
                         result.errorKind == ArchiveStreamErrorKind::TruncatedEntry) &&
                        RecoverFromStreamFailure(result.message.c_str()))
                        return Read(buffer, count);

                    return -1;
                }
                delivered = result.bytesRead;
            }
            else if (mode_ == Mode::Memory)
            {
                memcpy(buffer, memory_.data() + static_cast<size_t>(position_), wanted);
                delivered = wanted;
            }
            else if (mode_ == Mode::Cache)
            {
                DWORD read = 0;
                if (!ReadFile(cache_, buffer, wanted, &read, nullptr))
                    return -1;
                delivered = read;
            }
            if (!delivered && wanted)
                return -1;
            position_ += delivered;
            return static_cast<int>(delivered);
        }
        HRESULT WINAPI Write(unsigned char*, unsigned int, unsigned int* written) override
        {
            if (written)
                *written = 0;
            return E_NOTIMPL;
        }
    private:
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
        bool FaultModeMatches(const char* expected) const
        {
            char mode[16]{};
            const DWORD count = GetEnvironmentVariableA("AST_AIMP_FAULT_MODE", mode, sizeof(mode));
            return count > 0 && count < sizeof(mode) && strcmp(mode, expected) == 0;
        }
#endif
        bool OpenFallback(std::string& error)
        {
            const auto preferences = ArchiveAdapterPreferencesLoad();
            uint64_t memoryLimit = static_cast<uint64_t>(preferences.memoryMb) * 1024 * 1024;
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
            char configured[16]{};
            const DWORD length = GetEnvironmentVariableA("AST_AIMP_FAULT_MEMORY_MB", configured, sizeof(configured));
            if (length > 0 && length < sizeof(configured))
            {
                if (strcmp(configured, "1") == 0 || strcmp(configured, "50") == 0)
                    memoryLimit = static_cast<uint64_t>(atoi(configured)) * 1024 * 1024;
            }
#endif
            if (entry_.size <= memoryLimit && entry_.size <= SIZE_MAX)
            {
                if (ArchiveExtractEntryToMemory(archive_.c_str(), entry_.index, memory_, error) &&
                    memory_.size() == entry_.size)
                {
                    mode_ = Mode::Memory;
                    DebugLogWrite("INFO", "AIMP", "ROUTE_MEMORY", "index=%u | bytes=%zu", entry_.index, memory_.size());
                    return true;
                }
                memory_.clear();
            }

            std::wstring reusable;
            if (ArchiveUnifiedCache::Find(archive_, entry_, preferences, reusable))
            {
                cachePath_ = reusable;
                cachePersistent_ = true;
                if (OpenCacheHandle())
                {
                    DebugLogWrite("INFO", "AIMP", "ROUTE_CACHE_HIT", "index=%u | size=%llu", entry_.index, (unsigned long long)entry_.size);
                    ArchiveUnifiedWarmer::Queue(archive_, entry_, preferences);
                    return true;
                }
                cachePath_.clear();
                cachePersistent_ = false;
            }

            if (!ArchiveUnifiedCache::Prepare(archive_, entry_, preferences, cachePath_, error))
            {
                DebugLogWrite("WARN", "AIMP", "CACHE_FAIL", "index=%u | error=%s", entry_.index, error.c_str());
                return false;
            }

            cachePersistent_ = true;
            if (!OpenCacheHandle())
                return false;

            DebugLogWrite("INFO", "AIMP", "ROUTE_CACHE", "index=%u | size=%llu", entry_.index, (unsigned long long)entry_.size);
            ArchiveUnifiedWarmer::Queue(archive_, entry_, preferences);
            return true;
        }

        bool OpenCacheHandle()
        {
            if (cache_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(cache_);
                cache_ = INVALID_HANDLE_VALUE;
            }

            cache_ = CreateFileW(cachePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
            if (cache_ == INVALID_HANDLE_VALUE)
                return false;

            LARGE_INTEGER size{};
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(position_);
            if (!GetFileSizeEx(cache_, &size) || size.QuadPart < 0 ||
                static_cast<uint64_t>(size.QuadPart) != entry_.size ||
                !SetFilePointerEx(cache_, position, nullptr, FILE_BEGIN))
            {
                CloseHandle(cache_);
                cache_ = INVALID_HANDLE_VALUE;
                return false;
            }

            mode_ = Mode::Cache;
            return true;
        }

        bool RecoverFromStreamFailure(const char* reason)
        {
            DebugLogWrite("WARN", "AIMP", "STREAM_RECOVER", "index=%u | resume_at=%llu | reason=%s", entry_.index, (unsigned long long)position_, reason ? reason : "unknown");
            ArchiveCloseEntryStream(backend_);
            mode_ = Mode::None;
            std::string error;
            if (!OpenFallback(error))
            {
                DebugLogWrite("ERROR", "AIMP", "STREAM_RECOVER_FAIL", "index=%u | error=%s", entry_.index, error.c_str());
                return false;
            }
            DebugLogWrite("INFO", "AIMP", "STREAM_RECOVER_OK", "index=%u | resume_at=%llu | route=%s", entry_.index, (unsigned long long)position_, mode_ == Mode::Memory ? "memory" : "cache");
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
            ++g_aimpFaultRecovered;
#endif
            return true;
        }

        enum class Mode { None, Stream, Memory, Cache } mode_ = Mode::None;
        ComCounter refs_;
        std::wstring archive_;
        ArchiveEntry entry_;
        ArchiveBackendStream backend_{};
        std::vector<unsigned char> memory_;
        std::wstring cachePath_;
        HANDLE cache_ = INVALID_HANDLE_VALUE;
        bool cachePersistent_ = false;
        uint64_t position_ = 0;
#ifdef ARCHIVE_STREAM_AIMP_FAULT_TEST
        bool faultInjected_ = false;
#endif
    };

    class VirtualFile final : public IAIMPVirtualFile
    {
    public:
        VirtualFile(std::wstring archive, ArchiveEntry entry)
            : archive_(std::move(archive)), entry_(std::move(entry)) {}
        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) &&
                !IsEqualIID(iid, IID_IAIMPVirtualFile) &&
                !IsEqualIID(iid, IID_IAIMPPropertyList))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPVirtualFile*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            const ULONG count = refs_.Sub();
            if (!count)
                delete this;
            return count;
        }

        void WINAPI BeginUpdate() override {}
        void WINAPI EndUpdate() override {}
        HRESULT WINAPI Reset() override { return E_NOTIMPL; }
        HRESULT WINAPI GetValueAsFloat(int, double*) override { return E_NOTIMPL; }
        HRESULT WINAPI GetValueAsInt32(int property, int* value) override
        {
            if (!value)
                return E_POINTER;
            if (property != AIMP_VIRTUALFILE_PROPID_INDEXINSET)
                return E_INVALIDARG;
            *value = static_cast<int>(entry_.index);
            return S_OK;
        }
        HRESULT WINAPI GetValueAsInt64(int, INT64*) override { return E_INVALIDARG; }
        HRESULT WINAPI GetValueAsObject(int property, REFIID iid, void** output) override
        {
            if (!output)
                return E_POINTER;
            *output = nullptr;
            if (!IsEqualIID(iid, IID_IAIMPString) && !IsEqualIID(iid, IID_IUnknown))
                return E_NOINTERFACE;
            std::wstring text;
            if (property == AIMP_VIRTUALFILE_PROPID_FILEURI)
                text = FileUri(archive_, entry_);
            else if (property == AIMP_VIRTUALFILE_PROPID_AUDIOSOURCEFILE)
                return E_NOTIMPL;
            else if (property == AIMP_VIRTUALFILE_PROPID_FILEFORMAT)
            {
                std::wstring name = Utf8ToWide(entry_.path);
                const auto dot = name.rfind(L'.');
                if (dot == std::wstring::npos)
                    return E_INVALIDARG;
                text = name.substr(dot + 1);
            }
            else
                return E_INVALIDARG;
            IAIMPString* result = nullptr;
            const HRESULT status = NewAimpString(text, &result);
            if (SUCCEEDED(status))
                *output = result;
            return status;
        }
        HRESULT WINAPI SetValueAsFloat(int, const double) override { return E_NOTIMPL; }
        HRESULT WINAPI SetValueAsInt32(int, int) override { return E_NOTIMPL; }
        HRESULT WINAPI SetValueAsInt64(int, const INT64) override { return E_NOTIMPL; }
        HRESULT WINAPI SetValueAsObject(int, IUnknown*) override { return E_NOTIMPL; }

        HRESULT WINAPI CreateStream(IAIMPStream** output) override
        {
            if (!output)
                return E_POINTER;
            *output = nullptr;
            auto* decoder = new (std::nothrow) EntryStream(archive_, entry_);
            if (!decoder)
                return E_OUTOFMEMORY;
            const HRESULT status = decoder->Open();
            if (FAILED(status))
            {
                decoder->Release();
                return status;
            }
            *output = decoder;
            return S_OK;
        }
        HRESULT WINAPI GetFileInfo(IAIMPFileInfo* info) override
        {
            if (!info)
                return E_POINTER;
            const auto preferences = ArchiveAdapterPreferencesLoad();
            const std::wstring name = ArchiveAdapterFormatTitle(archive_, Utf8ToWide(entry_.path), entry_.index + 1, preferences.titleTemplate);
            IAIMPString* nameValue = nullptr;
            HRESULT result = NewAimpString(name, &nameValue);
            if (FAILED(result))
                return result;
            result = info->SetValueAsObject(AIMP_FILEINFO_PROPID_TITLE, nameValue);
            nameValue->Release();
            if (FAILED(result))
                return result;
            result = NewAimpString(FileUri(archive_, entry_), &nameValue);
            if (FAILED(result))
                return result;
            result = info->SetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, nameValue);
            nameValue->Release();
            if (FAILED(result))
                return result;
            return info->SetValueAsInt64(AIMP_FILEINFO_PROPID_FILESIZE,
                static_cast<INT64>(entry_.size));
        }
        BOOL WINAPI IsExists() override
        {
            const DWORD attr = GetFileAttributesW(archive_.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        }
        HRESULT WINAPI IsInSameStream(IAIMPVirtualFile*) override { return E_FAIL; }
        HRESULT WINAPI Synchronize() override { return IsExists() ? S_OK : E_FAIL; }

    private:
        ComCounter refs_;
        std::wstring archive_;
        ArchiveEntry entry_;
    };

    // AIMP will not offer ZIP/7z/RAR through its normal Add Files dialog
    // unless the plugin also declares the container extensions.
    class ArchiveFileFormat final : public IAIMPExtensionFileFormat
    {
    public:
        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) &&
                !IsEqualIID(iid, IID_IAIMPExtensionFileFormat))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPExtensionFileFormat*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            const ULONG count = refs_.Sub();
            if (!count)
                delete this;
            return count;
        }
        HRESULT WINAPI GetDescription(IAIMPString** value) override
        {
            return NewAimpString(L"Archive Stream ZIP / 7z / RAR", value);
        }
        HRESULT WINAPI GetExtList(IAIMPString** value) override
        {
            // AIMP SDK requires a semicolon after every extension, including
            // the final one. The .001 forms are restricted by backend sniffing.
            return NewAimpString(L"*.zip;*.7z;*.rar;*.zip.001;*.7z.001;", value);
        }
        HRESULT WINAPI GetFlags(LongWord* flags) override
        {
            if (!flags)
                return E_POINTER;
            *flags = AIMP_SERVICE_FILEFORMATS_CATEGORY_AUDIO;
            return S_OK;
        }
    private:
        ComCounter refs_;
    };

    class Expander final : public IAIMPExtensionFileExpander
    {
    public:
        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IAIMPExtensionFileExpander))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPExtensionFileExpander*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            const ULONG count = refs_.Sub();
            if (!count)
                delete this;
            return count;
        }
        HRESULT WINAPI Expand(IAIMPString* file, IAIMPObjectList** output,
            IAIMPProgressCallback*) override
        {
            if (!file || !output)
                return E_POINTER;
            *output = nullptr;
            try
            {
                DebugLogWrite("INFO", "AIMP", "EXPAND_REQUEST", "uri=%ls", file->GetData() ? file->GetData() : L"");
                const int size = file->GetLength();
                if (size <= 0 || !file->GetData() || !g_core)
                    return E_FAIL;
                const std::wstring archive(file->GetData(), static_cast<size_t>(size));
                if (DetectArchiveFormatFromPath(archive.c_str()) == ArchiveFormat::Unknown)
                    return E_FAIL;
                std::vector<ArchiveEntry> entries;
                std::string error;
                if (!ArchiveListEntries(archive.c_str(), entries, error))
                {
                    DebugLogWrite("WARN", "AIMP", "EXPAND_FAIL", "error=%s", error.c_str());
                    return E_FAIL;
                }
                IAIMPObjectList* list = nullptr;
                const HRESULT create = g_core->CreateObject(IID_IAIMPObjectList,
                    reinterpret_cast<void**>(&list));
                if (FAILED(create) || !list)
                    return FAILED(create) ? create : E_FAIL;

                for (const auto& entry : entries)
                {
                    if (entry.isFolder || !entry.isSupportedAudio || entry.size == 0)
                        continue;
                    auto* virtualFile = new (std::nothrow) VirtualFile(archive, entry);
                    if (!virtualFile)
                    {
                        list->Release();
                        return E_OUTOFMEMORY;
                    }
                    const HRESULT added = list->Add(virtualFile);
                    virtualFile->Release();
                    if (FAILED(added))
                    {
                        list->Release();
                        return added;
                    }
                }
                if (!list->GetCount())
                {
                    list->Release();
                    return E_FAIL;
                }
                *output = list;
                DebugLogWrite("INFO", "AIMP", "EXPAND_OK", "count=%d", list->GetCount());
                return S_OK;
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
            catch (...)
            {
                return E_FAIL;
            }
        }
    private:
        ComCounter refs_;
    };

    // The AIMP Options panel links to the shared settings UI. Preferences
    // are per-user and intentionally independent from foobar/Winamp settings.
    class OptionsFrame final : public IAIMPOptionsDialogFrame
    {
    public:
        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown) && !IsEqualIID(iid, IID_IAIMPOptionsDialogFrame))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPOptionsDialogFrame*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            const ULONG count = refs_.Sub();
            if (!count)
                delete this;
            return count;
        }
        HRESULT WINAPI GetName(IAIMPString** output) override
        {
            return NewAimpString(L"Archive Stream Player", output);
        }
        HWND WINAPI CreateFrame(HWND parent) override
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = g_module;
            wc.lpszClassName = L"ArchiveStreamAimpOptionsFrame";
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&wc);
            frame_ = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
                0, 0, 550, 260, parent, nullptr, g_module, nullptr);
            if (!frame_)
                return nullptr;
            CreateWindowExW(0, L"STATIC",
                L"Archive audio: ZIP / 7z / RAR, streaming and cache fallback.",
                WS_CHILD | WS_VISIBLE, 18, 22, 515, 24, frame_, nullptr, g_module, nullptr);
            CreateWindowExW(0, L"STATIC",
                L"Shared settings apply to AIMP, XMPlay, MusicBee and bridge players.",
                WS_CHILD | WS_VISIBLE, 18, 54, 515, 24, frame_, nullptr, g_module, nullptr);
            CreateWindowExW(0, L"BUTTON", L"Configure archive settings...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 18, 92, 235, 32,
                frame_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5001)), g_module, nullptr);
            return frame_;
        }
        void WINAPI DestroyFrame() override
        {
            if (frame_)
                DestroyWindow(frame_);
            frame_ = nullptr;
        }
        void WINAPI Notification(int) override {}
    private:
        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
        {
            if (msg == WM_COMMAND && LOWORD(w) == 5001 && HIWORD(w) == BN_CLICKED)
            {
                ArchiveAdapterOpenSettings(g_module, hwnd);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, w, l);
        }
        ComCounter refs_;
        HWND frame_ = nullptr;
    };

    class Plugin final : public IAIMPPlugin
    {
    public:
        ~Plugin() { Finalize(); }
        HRESULT WINAPI QueryInterface(REFIID iid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (!IsEqualIID(iid, IID_IUnknown))
                return E_NOINTERFACE;
            *out = static_cast<IAIMPPlugin*>(this);
            AddRef();
            return S_OK;
        }
        ULONG WINAPI AddRef() override { return refs_.Add(); }
        ULONG WINAPI Release() override
        {
            const ULONG count = refs_.Sub();
            if (!count)
                delete this;
            return count;
        }
        TChar* WINAPI InfoGet(int index) override
        {
            switch (index)
            {
            case AIMP_PLUGIN_INFO_NAME: return const_cast<wchar_t*>(L"Archive Stream Player");
            case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<wchar_t*>(L"Archive Stream Player");
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
                return const_cast<wchar_t*>(L"ZIP/7z/RAR archive expansion using a static backend.");
            case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
                return const_cast<wchar_t*>(L"Expands archive audio to virtual tracks with streaming, memory and cache fallbacks.");
            default:
                return const_cast<wchar_t*>(L"");
            }
        }
        LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }
        HRESULT WINAPI Initialize(IAIMPCore* core) override
        {
            if (!core || g_core)
                return E_INVALIDARG;
            ArchiveSetDataDirectoryName("AIMPArchiveStream");
            const auto preferences = ArchiveAdapterPreferencesLoad();
            if (!preferences.passwordListPath.empty())
            {
                const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, preferences.passwordListPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (bytes > 1)
                {
                    std::string path(static_cast<size_t>(bytes), '\0');
                    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, preferences.passwordListPath.c_str(), -1, path.data(), bytes, nullptr, nullptr) == bytes)
                        ArchivePasswordSetListPath(path.c_str());
                }
            }
            ArchivePasswordSetPromptCallback(AskPassword);
            char traceSwitch[8]{};
            if (GetEnvironmentVariableA("ARCHIVE_STREAM_ADAPTER_DEBUG", traceSwitch, sizeof(traceSwitch)) > 0 && traceSwitch[0] == '1')
            {
                char tracePath[MAX_PATH * 4]{};
                if (ArchiveBuildPluginDataFilePath("aimp_archive_stream.log", tracePath, sizeof(tracePath)))
                    DebugLogConfigure(true, tracePath, true);
            }
            DebugLogWrite("INFO", "AIMP", "PLUGIN_INIT", "core=%p", core);
            core->AddRef();
            g_core = core;
            expander_ = new (std::nothrow) Expander();
            fileFormat_ = new (std::nothrow) ArchiveFileFormat();
            if (!expander_ || !fileFormat_)
            {
                if (fileFormat_)
                {
                    fileFormat_->Release();
                    fileFormat_ = nullptr;
                }
                if (expander_)
                {
                    expander_->Release();
                    expander_ = nullptr;
                }
                ArchivePasswordSetPromptCallback(nullptr);
                core->Release();
                g_core = nullptr;
                return E_OUTOFMEMORY;
            }
            HRESULT status = core->RegisterExtension(IID_IAIMPServiceFileManager, expander_);
            const bool expanderRegistered = SUCCEEDED(status);
            if (expanderRegistered)
                status = core->RegisterExtension(IID_IAIMPServiceFileFormats, fileFormat_);
            if (FAILED(status))
            {
                DebugLogWrite("ERROR", "AIMP", "REGISTER_FAIL", "hresult=0x%08X | expander_registered=%d", static_cast<unsigned>(status), expanderRegistered ? 1 : 0);
                if (expanderRegistered)
                    core->UnregisterExtension(expander_);
                fileFormat_->Release();
                fileFormat_ = nullptr;
                expander_->Release();
                expander_ = nullptr;
                ArchivePasswordSetPromptCallback(nullptr);
                core->Release();
                g_core = nullptr;
                return status;
            }
            // Options registration is optional for older AIMP hosts; failure
            // must not disable the fully functional archive decoder.
            options_ = new (std::nothrow) OptionsFrame();
            if (options_)
            {
                const HRESULT optionResult = core->RegisterExtension(IID_IAIMPServiceOptionsDialog, options_);
                if (FAILED(optionResult))
                {
                    DebugLogWrite("WARN", "AIMP", "OPTIONS_REGISTER_FAIL", "hresult=0x%08X", static_cast<unsigned>(optionResult));
                    options_->Release();
                    options_ = nullptr;
                }
            }
            DebugLogWrite("INFO", "AIMP", "REGISTER_OK", "file_expander=1 | audio_extensions=1 | options=%d", options_ ? 1 : 0);
            return S_OK;
        }
        HRESULT WINAPI Finalize() override
        {
            if (g_core)
            {
                DebugLogWrite("INFO", "AIMP", "PLUGIN_FINALIZE", "expander=%p | format=%p", expander_, fileFormat_);
                if (options_)
                {
                    g_core->UnregisterExtension(options_);
                    options_->Release();
                    options_ = nullptr;
                }
                if (fileFormat_)
                {
                    g_core->UnregisterExtension(fileFormat_);
                    fileFormat_->Release();
                    fileFormat_ = nullptr;
                }
                if (expander_)
                {
                    g_core->UnregisterExtension(expander_);
                    expander_->Release();
                    expander_ = nullptr;
                }
                ArchivePasswordSetPromptCallback(nullptr);
                g_core->Release();
                g_core = nullptr;
            }
            return S_OK;
        }
        void WINAPI SystemNotification(int, IUnknown*) override {}
    private:
        ComCounter refs_;
        Expander* expander_ = nullptr;
        ArchiveFileFormat* fileFormat_ = nullptr;
        OptionsFrame* options_ = nullptr;
    };
}

extern "C" HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** output)
{
    if (!output)
        return E_POINTER;
    *output = new (std::nothrow) Plugin();
    return *output ? S_OK : E_OUTOFMEMORY;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = dll;
        DisableThreadLibraryCalls(dll);
    }
    return TRUE;
}
