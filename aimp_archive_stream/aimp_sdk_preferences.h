#pragma once
// AIMP-native configuration UI. All visible controls are created by
// IAIMPServiceUI; the shared registry/settings backend stays host-independent.
#include <apiGUI.h>
#include <apiOptions.h>
#include <apiObjects.h>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <string>
#include <vector>
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_adapter_logging.h"
#include "../shared/archive_unified_cache.h"
#include "../archive_backend/archive_passwords.h"

class AimpSdkPreferences final : public IAIMPUIChangeEvents, public IAIMPUIMouseEvents
{
public:
    using ModifiedCallback = void (*)(void*);
    explicit AimpSdkPreferences(IAIMPCore* core) : core_(core) {}

    HRESULT WINAPI QueryInterface(REFIID iid, void** output) override
    {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAIMPUIChangeEvents))
            *output = static_cast<IAIMPUIChangeEvents*>(this);
        else if (IsEqualIID(iid, IID_IAIMPUIMouseEvents))
            *output = static_cast<IAIMPUIMouseEvents*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG WINAPI AddRef() override { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG WINAPI Release() override { return references_.fetch_sub(1, std::memory_order_acq_rel) - 1; }

    HWND Create(HWND parent, void* callbackContext, ModifiedCallback callback)
    {
        DebugLogWrite("INFO", "AIMP", "SETTINGS_FRAME_CREATE", "parent=%p | core=%p", parent, core_);
        if (!parent || !core_)
            return nullptr;
        callbackContext_ = callbackContext;
        callback_ = callback;
        loading_ = true;
        const HRESULT uiResult = core_->QueryInterface(IID_IAIMPServiceUI, reinterpret_cast<void**>(&service_));
        if (FAILED(uiResult) || !service_)
            return nullptr;
        IAIMPString* formName = String(L"ArchiveStreamOptions");
        if (!formName)
            return nullptr;
        HRESULT hr = service_->CreateForm(parent, AIMPUI_SERVICE_CREATEFORM_FLAGS_CHILD, formName, nullptr, &form_);
        formName->Release();
        if (FAILED(hr) || !form_)
            return nullptr;
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_BORDERSTYLE, AIMPUI_FLAGS_BORDERSTYLE_NONE);

        if (!AddControl(form_, IID_IAIMPUIPageControl, pageControl_))
        {
            DebugLogWrite("WARN", "AIMP", "UI_NATIVE_CREATE_FAIL", "phase=page_control");
            return nullptr;
        }
        TAIMPUIControlPlacement fill{};
        fill.Alignment = ualClient;
        pageControl_->SetPlacement(fill);
        constexpr const wchar_t* pageIds[] = {L"astPlayback", L"astPasswords", L"astDiagnostics", L"astTrackTitles"};
        constexpr const wchar_t* pageNames[] = {L"Playback & cache", L"Passwords", L"Diagnostics", L"Track titles"};
        for (int i = 0; i < 4; ++i)
        {
            IAIMPString* name = String(pageIds[i]);
            if (!name)
                return nullptr;
            hr = pageControl_->Add(name, &pages_[i]);
            name->Release();
            if (FAILED(hr) || !pages_[i])
                return nullptr;
            if (!SetString(pages_[i], AIMPUI_TABSHEET_PROPID_CAPTION, pageNames[i]))
            {
                DebugLogWrite("WARN", "AIMP", "SETTINGS_FRAME_CREATE_FAIL", "reason=tab_caption | page=%d", i);
                return nullptr;
            }
        }
        if (!BuildPlayback() || !BuildPasswords() || !BuildDiagnostics() || !BuildTitles())
        {
            DebugLogWrite("WARN", "AIMP", "UI_NATIVE_CREATE_FAIL", "phase=controls");
            return nullptr;
        }
        Load();
        DebugLogWrite("INFO", "AIMP", "SETTINGS_FRAME_READY", "pages=%d | handle=%p", pageControl_->GetCount(), form_->GetHandle());
        return form_->GetHandle();
    }

    void Destroy()
    {
        // Dispose native controls before releasing the AIMP-created child form.
        for (auto*& control : controls_)
        {
            if (control)
            {
                control->Release();
                control = nullptr;
            }
        }
        for (auto*& page : pages_)
        {
            if (page)
            {
                page->Release();
                page = nullptr;
            }
        }
        if (pageControl_)
        {
            pageControl_->Release();
            pageControl_ = nullptr;
        }
        if (form_)
        {
            form_->Release(FALSE);
            form_ = nullptr;
        }
        if (service_)
        {
            service_->Release();
            service_ = nullptr;
        }
        memory_ = nullptr;
        cacheSize_ = nullptr;
        cacheAge_ = nullptr;
        passwordPath_ = nullptr;
        logPath_ = nullptr;
        titlePreset_ = nullptr;
        autoCleanup_ = nullptr;
        warmSolid_ = nullptr;
        promptPasswords_ = nullptr;
        savePasswords_ = nullptr;
        debugLogging_ = nullptr;
        openCache_ = nullptr;
        clearCache_ = nullptr;
        browse_ = nullptr;
        openLog_ = nullptr;
    }

    void Load()
    {
        if (!form_)
            return;
        loading_ = true;
        const auto current = ArchiveAdapterPreferencesLoad();
        SetText(memory_, std::to_wstring(current.memoryMb));
        SetText(cacheSize_, std::to_wstring(current.cacheLimitMb));
        SetText(cacheAge_, std::to_wstring(current.cacheAgeDays));
        SetText(passwordPath_, current.passwordListPath);
        SetText(logPath_, current.logDirectory);
        SetChecked(autoCleanup_, current.cacheAutoCleanup);
        SetChecked(warmSolid_, current.warmSolidArchives);
        SetChecked(promptPasswords_, current.promptPasswords);
        SetChecked(savePasswords_, current.askToSavePassword);
        SetChecked(debugLogging_, current.debugLogging);
        int selected = 0;
        for (size_t i = 0; i < presets_.size(); ++i)
        {
            if (current.titleTemplate == presets_[i])
            {
                selected = static_cast<int>(i);
                break;
            }
        }
        titlePreset_->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, selected);
        unsigned actualBuffer = 0;
        unsigned actualCache = 0;
        unsigned actualAge = 0;
        const bool numericMatch = ReadNumber(memory_, 1, 512, actualBuffer) && actualBuffer == current.memoryMb &&
            ReadNumber(cacheSize_, 0, 20480, actualCache) && actualCache == current.cacheLimitMb &&
            ReadNumber(cacheAge_, 1, 365, actualAge) && actualAge == current.cacheAgeDays;
        const bool checkboxMatch = Checked(autoCleanup_) == current.cacheAutoCleanup &&
            Checked(warmSolid_) == current.warmSolidArchives &&
            Checked(promptPasswords_) == current.promptPasswords &&
            Checked(savePasswords_) == current.askToSavePassword &&
            Checked(debugLogging_) == current.debugLogging;
        const bool pathMatch = GetText(passwordPath_) == current.passwordListPath &&
            GetText(logPath_) == current.logDirectory;
        DebugLogWrite(numericMatch && checkboxMatch && pathMatch ? "INFO" : "WARN", "AIMP", "SETTINGS_NATIVE_LOAD",
            "numeric=%d | checkboxes=%d | paths=%d | buffer=%u | pages=%d", numericMatch ? 1 : 0, checkboxMatch ? 1 : 0,
            pathMatch ? 1 : 0, actualBuffer, pageControl_ ? pageControl_->GetCount() : 0);
        loading_ = false;
    }

    void Reset()
    {
        if (!form_)
            return;
        const ArchiveAdapterPreferences defaults;
        loading_ = true;
        SetText(memory_, std::to_wstring(defaults.memoryMb));
        SetText(cacheSize_, std::to_wstring(defaults.cacheLimitMb));
        SetText(cacheAge_, std::to_wstring(defaults.cacheAgeDays));
        SetText(passwordPath_, defaults.passwordListPath);
        SetText(logPath_, defaults.logDirectory);
        SetChecked(autoCleanup_, defaults.cacheAutoCleanup);
        SetChecked(warmSolid_, defaults.warmSolidArchives);
        SetChecked(promptPasswords_, defaults.promptPasswords);
        SetChecked(savePasswords_, defaults.askToSavePassword);
        SetChecked(debugLogging_, defaults.debugLogging);
        titlePreset_->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, 0);
        loading_ = false;
        MarkModified();
    }

    bool Save()
    {
        if (!form_)
            return false;
        ArchiveAdapterPreferences settings = ArchiveAdapterPreferencesLoad();
        if (!ReadNumber(memory_, 1, 512, settings.memoryMb) ||
            !ReadNumber(cacheSize_, 0, 20480, settings.cacheLimitMb) ||
            !ReadNumber(cacheAge_, 1, 365, settings.cacheAgeDays))
        {
            MessageBoxW(form_->GetHandle(), L"Buffer must be 1-512 MB, cache size 0-20480 MB and age 1-365 days.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        settings.cacheAutoCleanup = Checked(autoCleanup_);
        settings.warmSolidArchives = Checked(warmSolid_);
        settings.promptPasswords = Checked(promptPasswords_);
        settings.askToSavePassword = Checked(savePasswords_);
        settings.debugLogging = Checked(debugLogging_);
        settings.passwordListPath = GetText(passwordPath_);
        settings.logDirectory = GetText(logPath_);
        int selected = 0;
        titlePreset_->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &selected);
        if (selected >= 0 && selected < static_cast<int>(presets_.size()))
            settings.titleTemplate = presets_[static_cast<size_t>(selected)];
        if (settings.passwordListPath.size() >= 4096 || settings.logDirectory.size() >= 4096)
        {
            MessageBoxW(form_->GetHandle(), L"File and folder paths must be shorter than 4096 characters.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!ArchiveAdapterPreferencesSave(settings))
        {
            MessageBoxW(form_->GetHandle(), L"Failed to save shared archive settings.",
                L"Archive Stream Player", MB_OK | MB_ICONERROR);
            return false;
        }
        const std::string passwords = ArchiveAdapterWideToUtf8(settings.passwordListPath);
        if (!passwords.empty())
            ArchivePasswordSetListPath(passwords.c_str());
        ArchiveAdapterLogging::Configure(L"aimp", settings.debugLogging);
        return true;
    }

    // AIMP's GUI demo dispatches native button activation through
    // IAIMPUIChangeEvents, including keyboard activation. Do not dispatch
    // actions on mouse-up as well, or a click can run them twice.
    void WINAPI OnChanged(IUnknown* sender) override
    {
        if (loading_)
            return;
        if (SameObject(sender, openCache_))
            OpenFolder(ArchiveUnifiedCache::Folder());
        else if (SameObject(sender, clearCache_))
        {
            if (MessageBoxW(form_->GetHandle(), L"Delete unused managed cache files?",
                L"Archive Stream Player", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                const bool cleared = ArchiveUnifiedCache::Cleanup(0);
                MessageBoxW(form_->GetHandle(), cleared ? L"Managed cache cleared." : L"Some cached files are currently in use.",
                    L"Archive Stream Player", MB_OK | (cleared ? MB_ICONINFORMATION : MB_ICONWARNING));
            }
        }
        else if (SameObject(sender, browse_))
            BrowsePasswordList();
        else if (SameObject(sender, openLog_))
            OpenFolder(ArchiveAdapterLogging::Directory(ArchiveAdapterPreferencesLoad()));
        else
            MarkModified();
    }
    void WINAPI OnMouseDoubleClick(IUnknown*, TAIMPUIMouseButton, int, int, WORD) override {}
    void WINAPI OnMouseDown(IUnknown*, TAIMPUIMouseButton, int, int, WORD) override {}
    void WINAPI OnMouseLeave(IUnknown*) override {}
    void WINAPI OnMouseMove(IUnknown*, int, int, WORD) override {}
    void WINAPI OnMouseUp(IUnknown*, TAIMPUIMouseButton, int, int, WORD) override {}

private:
    static constexpr std::array<const wchar_t*, 6> presets_ = {
        L"<filename>", L"<archivetype>: <filename>", L"<#>. <filename>",
        L"<filename> [<filetype>]", L"<archivetype>: <#> - <filename>",
        L"[<archive>] <#> - <filename>"
    };
    static bool SameObject(IUnknown* left, IUnknown* right)
    {
        if (!left || !right)
            return false;
        IUnknown* leftIdentity = nullptr;
        IUnknown* rightIdentity = nullptr;
        const bool valid = SUCCEEDED(left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&leftIdentity))) &&
            SUCCEEDED(right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&rightIdentity)));
        const bool same = valid && leftIdentity == rightIdentity;
        if (leftIdentity)
            leftIdentity->Release();
        if (rightIdentity)
            rightIdentity->Release();
        return same;
    }
    std::atomic<ULONG> references_{1};
    IAIMPCore* core_ = nullptr;
    IAIMPServiceUI* service_ = nullptr;
    IAIMPUIForm* form_ = nullptr;
    IAIMPUIPageControl* pageControl_ = nullptr;
    std::array<IAIMPUITabSheet*, 4> pages_{};
    std::vector<IAIMPUIControl*> controls_;
    IAIMPUIEdit* memory_ = nullptr;
    IAIMPUIEdit* cacheSize_ = nullptr;
    IAIMPUIEdit* cacheAge_ = nullptr;
    IAIMPUIEdit* passwordPath_ = nullptr;
    IAIMPUIEdit* logPath_ = nullptr;
    IAIMPUIComboBox* titlePreset_ = nullptr;
    IAIMPUICheckBox* autoCleanup_ = nullptr;
    IAIMPUICheckBox* warmSolid_ = nullptr;
    IAIMPUICheckBox* promptPasswords_ = nullptr;
    IAIMPUICheckBox* savePasswords_ = nullptr;
    IAIMPUICheckBox* debugLogging_ = nullptr;
    IAIMPUIButton* openCache_ = nullptr;
    IAIMPUIButton* clearCache_ = nullptr;
    IAIMPUIButton* browse_ = nullptr;
    IAIMPUIButton* openLog_ = nullptr;
    void* callbackContext_ = nullptr;
    ModifiedCallback callback_ = nullptr;
    bool loading_ = false;

    IAIMPString* String(const std::wstring& value) const
    {
        IAIMPString* result = nullptr;
        if (!core_ || FAILED(core_->CreateObject(IID_IAIMPString, reinterpret_cast<void**>(&result))) || !result)
            return nullptr;
        if (FAILED(result->SetData(const_cast<wchar_t*>(value.data()), static_cast<int>(value.size()))))
        {
            result->Release();
            return nullptr;
        }
        return result;
    }
    bool SetString(IAIMPPropertyList* object, int property, const std::wstring& value) const
    {
        if (!object)
            return false;
        IAIMPString* text = String(value);
        if (!text)
            return false;
        const HRESULT result = object->SetValueAsObject(property, text);
        text->Release();
        return SUCCEEDED(result);
    }
    bool SetText(IAIMPUIEdit* control, const std::wstring& value) const
    {
        return SetString(control, AIMPUI_BASEEDIT_PROPID_TEXT, value);
    }
    std::wstring GetText(IAIMPUIEdit* control) const
    {
        if (!control)
            return {};
        IAIMPString* text = nullptr;
        if (FAILED(control->GetValueAsObject(AIMPUI_BASEEDIT_PROPID_TEXT, IID_IAIMPString, reinterpret_cast<void**>(&text))) || !text)
            return {};
        const int count = text->GetLength();
        std::wstring value;
        if (count > 0 && count < 32768 && text->GetData())
            value.assign(text->GetData(), static_cast<size_t>(count));
        text->Release();
        return value;
    }
    bool ReadNumber(IAIMPUIEdit* control, unsigned min, unsigned max, unsigned& output) const
    {
        const auto value = GetText(control);
        if (value.empty() || value.size() > 9)
            return false;
        unsigned result = 0;
        for (const wchar_t character : value)
        {
            if (character < L'0' || character > L'9')
                return false;
            result = result * 10 + static_cast<unsigned>(character - L'0');
        }
        if (result < min || result > max)
            return false;
        output = result;
        return true;
    }
    void SetChecked(IAIMPUICheckBox* control, bool checked) const
    {
        if (control)
            control->SetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, checked ? AIMPUI_CHECKSTATE_CHECKED : AIMPUI_CHECKSTATE_UNCHECKED);
    }
    bool Checked(IAIMPUICheckBox* control) const
    {
        int state = 0;
        return control && SUCCEEDED(control->GetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, &state)) && state == AIMPUI_CHECKSTATE_CHECKED;
    }

    void MarkModified()
    {
        if (!loading_ && callback_)
            callback_(callbackContext_);
    }
    bool AddControl(IAIMPUIWinControl* parent, REFIID iid, IAIMPUIPageControl*& control)
    {
        return SUCCEEDED(service_->CreateControl(form_, parent, nullptr, static_cast<IAIMPUIChangeEvents*>(this), iid, reinterpret_cast<void**>(&control))) && control;
    }
    template <typename T>
    T* Add(IAIMPUIWinControl* parent, REFIID iid, int height, int width = 0, bool wide = true)
    {
        T* control = nullptr;
        if (FAILED(service_->CreateControl(form_, parent, nullptr, static_cast<IAIMPUIChangeEvents*>(this), iid, reinterpret_cast<void**>(&control))) || !control)
            return nullptr;
        controls_.push_back(control);
        TAIMPUIControlPlacement position{};
        position.Alignment = ualTop;
        position.Bounds = RECT{0, 0, width, height};
        position.AlignmentMargins = RECT{wide ? 10L : 0L, 2L, wide ? 10L : 0L, 2L};
        if (FAILED(control->SetPlacement(position)))
            return nullptr;
        return control;
    }
    IAIMPUILabel* Label(IAIMPUIWinControl* page, const wchar_t* text, int height = 30)
    {
        IAIMPUILabel* label = Add<IAIMPUILabel>(page, IID_IAIMPUILabel, height);
        if (label)
        {
            SetString(label, AIMPUI_LABEL_PROPID_TEXT, text);
            label->SetValueAsInt32(AIMPUI_LABEL_PROPID_AUTOSIZE, 0);
            label->SetValueAsInt32(AIMPUI_LABEL_PROPID_WORDWRAP, 1);
        }
        return label;
    }
    IAIMPUIEdit* Edit(IAIMPUIWinControl* page, const wchar_t* title, int height = 35)
    {
        Label(page, title, 25);
        return Add<IAIMPUIEdit>(page, IID_IAIMPUIEdit, height);
    }
    IAIMPUICheckBox* CheckBox(IAIMPUIWinControl* page, const wchar_t* title)
    {
        IAIMPUICheckBox* box = Add<IAIMPUICheckBox>(page, IID_IAIMPUICheckBox, 33);
        if (box)
            SetString(box, AIMPUI_CHECKBOX_PROPID_CAPTION, title);
        return box;
    }
    IAIMPUIButton* Button(IAIMPUIWinControl* page, const wchar_t* title)
    {
        IAIMPUIButton* button = Add<IAIMPUIButton>(page, IID_IAIMPUIButton, 36);
        if (button)
            SetString(button, AIMPUI_BUTTON_PROPID_CAPTION, title);
        return button;
    }
    bool BuildPlayback()
    {
        IAIMPUIWinControl* page = pages_[0];
        memory_ = Edit(page, L"Playback buffer (MB, 1-512)");
        cacheSize_ = Edit(page, L"Maximum managed cache (MB, 0-20480)");
        cacheAge_ = Edit(page, L"Maximum cache age (days, 1-365)");
        autoCleanup_ = CheckBox(page, L"Automatically clean old managed-cache files");
        warmSolid_ = CheckBox(page, L"Warm solid archives in the background");
        openCache_ = Button(page, L"Open managed cache");
        clearCache_ = Button(page, L"Clear managed cache...");
        return memory_ && cacheSize_ && cacheAge_ && autoCleanup_ && warmSolid_ && openCache_ && clearCache_;
    }
    bool BuildPasswords()
    {
        IAIMPUIWinControl* page = pages_[1];
        passwordPath_ = Edit(page, L"Password list file (one password per line)", 42);
        browse_ = Button(page, L"Browse for password list...");
        promptPasswords_ = CheckBox(page, L"Prompt for passwords when necessary");
        savePasswords_ = CheckBox(page, L"Ask before saving successful passwords to the list");
        Label(page, L"The password list is plain text. Keep it in a private folder.", 55);
        return passwordPath_ && browse_ && promptPasswords_ && savePasswords_;
    }
    bool BuildDiagnostics()
    {
        IAIMPUIWinControl* page = pages_[2];
        debugLogging_ = CheckBox(page, L"Enable Archive Stream diagnostics logging");
        logPath_ = Edit(page, L"Log directory (blank = shared default)", 42);
        openLog_ = Button(page, L"Open log folder");
        Label(page, L"Each player writes a separate log file. Changes apply to the next track.", 58);
        return debugLogging_ && logPath_ && openLog_;
    }
    bool BuildTitles()
    {
        IAIMPUIWinControl* page = pages_[3];
        Label(page, L"Archive-entry title preset:");
        titlePreset_ = Add<IAIMPUIComboBox>(page, IID_IAIMPUIComboBox, 39);
        if (!titlePreset_)
            return false;
        titlePreset_->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_STYLE, AIMPUI_COMBOBOX_STYLE_LIST);
        for (const wchar_t* title : presets_)
        {
            IAIMPString* value = String(title);
            if (!value)
                return false;
            const HRESULT hr = titlePreset_->Add(value, 0);
            value->Release();
            if (FAILED(hr))
                return false;
        }
        Label(page, L"Controls archive-entry titles; embedded audio tags are not modified.", 64);
        return true;
    }
    void BrowsePasswordList()
    {
        wchar_t path[4096]{};
        wcsncpy_s(path, GetText(passwordPath_).c_str(), _TRUNCATE);
        static const wchar_t filter[] = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
        OPENFILENAMEW chooser{};
        chooser.lStructSize = sizeof(chooser);
        chooser.hwndOwner = form_ ? form_->GetHandle() : nullptr;
        chooser.lpstrFile = path;
        chooser.nMaxFile = static_cast<DWORD>(std::size(path));
        chooser.lpstrFilter = filter;
        chooser.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&chooser))
        {
            SetText(passwordPath_, path);
            MarkModified();
        }
    }
    void OpenFolder(const std::wstring& folder) const
    {
        if (!folder.empty())
            ShellExecuteW(form_ ? form_->GetHandle() : nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
};
