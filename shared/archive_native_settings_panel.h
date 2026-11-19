#pragma once

// Native player preferences. These controls edit the shared adapter registry
// values directly; no PowerShell process is needed for normal configuration.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <cwchar>
#include "archive_adapter_preferences.h"
#include "archive_adapter_logging.h"
#include "archive_unified_cache.h"
#include "archive_passwords.h"

class ArchiveNativeSettingsPanel
{
public:
    explicit ArchiveNativeSettingsPanel(const wchar_t* host = L"aimp") : hostName_(host) {}
    using ModifiedCallback = void (*)(void*);
    HWND Create(HWND parent, HMODULE module, void* context, ModifiedCallback onModified)
    {
        if (!parent || !module)
            return nullptr;
        module_ = module;
        modifiedContext_ = context;
        modifiedCallback_ = onModified;
        dpi_ = GetDpiForWindow(parent);
        if (!dpi_)
            dpi_ = 96;
        font_ = CreateFontW(-MulDiv(9, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_TAB_CLASSES};
        InitCommonControlsEx(&init);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = module_;
        windowClass.lpszClassName = L"ArchiveStreamNativeSettingsV3";
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
        RECT bounds{};
        GetClientRect(parent, &bounds);
        frame_ = CreateWindowExW(0, windowClass.lpszClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, std::max(400L, bounds.right), std::max(350L, bounds.bottom),
            parent, nullptr, module_, this);
        if (!frame_ && font_)
        {
            DeleteObject(font_);
            font_ = nullptr;
        }
        return frame_;
    }

    void Destroy()
    {
        if (frame_)
            DestroyWindow(frame_);
        frame_ = nullptr;
        for (auto& controls : pageControls_)
            controls.clear();
        tab_ = nullptr;
        status_ = nullptr;
        save_ = nullptr;
        reload_ = nullptr;
        buffer_ = nullptr;
        cacheLimit_ = nullptr;
        cacheAge_ = nullptr;
        cacheCleanup_ = nullptr;
        warmSolid_ = nullptr;
        passwordPath_ = nullptr;
        promptPasswords_ = nullptr;
        savePasswords_ = nullptr;
        debugLogging_ = nullptr;
        logDirectory_ = nullptr;
        titleTemplate_ = nullptr;
        useFileTags_ = nullptr;
        titlePrefix_ = nullptr;
        titlePostfix_ = nullptr;
        passwordBrowse_ = nullptr;
        logOpen_ = nullptr;
        if (font_)
        {
            DeleteObject(font_);
            font_ = nullptr;
        }
    }

    void Load()
    {
        if (!frame_)
            return;
        loading_ = true;
        const auto options = ArchiveAdapterPreferencesLoad();
        SetNumber(buffer_, options.memoryMb);
        SetNumber(cacheLimit_, options.cacheLimitMb);
        SetNumber(cacheAge_, options.cacheAgeDays);
        SetChecked(cacheCleanup_, options.cacheAutoCleanup);
        SetChecked(warmSolid_, options.warmSolidArchives);
        SetChecked(promptPasswords_, options.promptPasswords);
        SetChecked(savePasswords_, options.askToSavePassword);
        SetChecked(debugLogging_, options.debugLogging);
        SetChecked(useFileTags_, options.useFileTags);
        SetWindowTextW(titlePrefix_, options.titlePrefix.c_str());
        SetWindowTextW(titlePostfix_, options.titlePostfix.c_str());
        SetWindowTextW(passwordPath_, options.passwordListPath.c_str());
        SetWindowTextW(logDirectory_, options.logDirectory.c_str());
        unsigned chosen = 0;
        for (unsigned index = 0; index < titleTemplates_.size(); ++index)
        {
            if (options.titleTemplate == titleTemplates_[index])
            {
                chosen = index;
                break;
            }
        }
        SendMessageW(titleTemplate_, CB_SETCURSEL, chosen, 0);
        dirty_ = false;
        SetWindowTextW(status_, L"Settings loaded. Changes also apply to other Archive Stream adapters.");
        loading_ = false;
    }

    void Reset()
    {
        if (!frame_)
            return;
        const ArchiveAdapterPreferences defaults;
        loading_ = true;
        SetNumber(buffer_, defaults.memoryMb);
        SetNumber(cacheLimit_, defaults.cacheLimitMb);
        SetNumber(cacheAge_, defaults.cacheAgeDays);
        SetChecked(cacheCleanup_, defaults.cacheAutoCleanup);
        SetChecked(warmSolid_, defaults.warmSolidArchives);
        SetChecked(promptPasswords_, defaults.promptPasswords);
        SetChecked(savePasswords_, defaults.askToSavePassword);
        SetChecked(debugLogging_, defaults.debugLogging);
        SetChecked(useFileTags_, defaults.useFileTags);
        SetWindowTextW(titlePrefix_, defaults.titlePrefix.c_str());
        SetWindowTextW(titlePostfix_, defaults.titlePostfix.c_str());
        SetWindowTextW(passwordPath_, defaults.passwordListPath.c_str());
        SetWindowTextW(logDirectory_, defaults.logDirectory.c_str());
        SendMessageW(titleTemplate_, CB_SETCURSEL, 0, 0);
        loading_ = false;
        MarkModified();
        SetWindowTextW(status_, L"Defaults selected; click Save settings to commit.");
    }

    bool Save()
    {
        if (!frame_)
            return false;
        ArchiveAdapterPreferences settings = ArchiveAdapterPreferencesLoad();
        if (!ReadNumber(buffer_, 1, 512, settings.memoryMb) ||
            !ReadNumber(cacheLimit_, 0, 20480, settings.cacheLimitMb) ||
            !ReadNumber(cacheAge_, 1, 365, settings.cacheAgeDays))
        {
            MessageBoxW(frame_, L"Enter valid numbers: buffer 1-512 MB, cache 0-20480 MB, age 1-365 days.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        settings.cacheAutoCleanup = Checked(cacheCleanup_);
        settings.warmSolidArchives = Checked(warmSolid_);
        settings.promptPasswords = Checked(promptPasswords_);
        settings.askToSavePassword = Checked(savePasswords_);
        settings.debugLogging = Checked(debugLogging_);
        settings.useFileTags = Checked(useFileTags_);
        settings.titlePrefix = ReadText(titlePrefix_);
        settings.titlePostfix = ReadText(titlePostfix_);
        settings.passwordListPath = ReadText(passwordPath_);
        settings.logDirectory = ReadText(logDirectory_);
        const int selected = static_cast<int>(SendMessageW(titleTemplate_, CB_GETCURSEL, 0, 0));
        if (selected >= 0 && selected < static_cast<int>(titleTemplates_.size()))
            settings.titleTemplate = titleTemplates_[selected];
        if (ArchiveAdapterWideToUtf8(settings.titlePrefix).size() > 20 ||
            ArchiveAdapterWideToUtf8(settings.titlePostfix).size() > 20)
        {
            MessageBoxW(frame_, L"Title prefix and postfix must each be at most 20 UTF-8 bytes.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (settings.passwordListPath.size() >= 4096 || settings.logDirectory.size() >= 4096)
        {
            MessageBoxW(frame_, L"Password list and log directory paths must be shorter than 4096 characters.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (!ArchiveAdapterPreferencesSave(settings))
        {
            MessageBoxW(frame_, L"Unable to save shared adapter settings to the user registry.",
                L"Archive Stream Player", MB_OK | MB_ICONERROR);
            return false;
        }
        const std::string passwordPath = ArchiveAdapterWideToUtf8(settings.passwordListPath);
        if (!passwordPath.empty())
            ArchivePasswordSetListPath(passwordPath.c_str());
        ArchiveAdapterLogging::Configure(hostName_, settings.debugLogging);
        dirty_ = false;
        SetWindowTextW(status_, L"Saved. Playback changes apply on the next track; other players share these values.");
        return true;
    }

private:
    enum : int
    {
        kTab = 4100, kSave = 4101, kReload = 4102, kOpenCache = 4103,
        kClearCache = 4104, kBrowsePasswords = 4105, kOpenLog = 4106,
        kBuffer = 4200, kCacheLimit = 4201, kCacheAge = 4202, kAutoCleanup = 4203,
        kWarmSolid = 4204, kPasswordPath = 4205, kPrompt = 4206, kSavePassword = 4207,
        kDebugLogging = 4208, kLogDirectory = 4209, kTitleTemplate = 4210, kUseFileTags = 4211,
        kTitlePrefix = 4212, kTitlePostfix = 4213
    };
    static constexpr std::array<const wchar_t*, 6> titleTemplates_ = {
        L"<filename>", L"<archivetype>: <filename>", L"<#>. <filename>",
        L"<filename> [<filetype>]", L"<archivetype>: <#> - <filename>",
        L"[<archive>] <#> - <filename>"
    };
    HWND frame_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr;
    int Scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND tab_ = nullptr;
    HWND status_ = nullptr;
    HWND save_ = nullptr;
    HWND reload_ = nullptr;
    HWND buffer_ = nullptr;
    HWND cacheLimit_ = nullptr;
    HWND cacheAge_ = nullptr;
    HWND cacheCleanup_ = nullptr;
    HWND warmSolid_ = nullptr;
    HWND passwordPath_ = nullptr;
    HWND promptPasswords_ = nullptr;
    HWND savePasswords_ = nullptr;
    HWND debugLogging_ = nullptr;
    HWND logDirectory_ = nullptr;
    HWND titleTemplate_ = nullptr;
    HWND useFileTags_ = nullptr;
    HWND titlePrefix_ = nullptr;
    HWND titlePostfix_ = nullptr;
    HWND passwordBrowse_ = nullptr;
    HWND logOpen_ = nullptr;
    std::array<std::vector<HWND>, 4> pageControls_;
    const wchar_t* hostName_ = L"aimp";
    HMODULE module_ = nullptr;
    void* modifiedContext_ = nullptr;
    ModifiedCallback modifiedCallback_ = nullptr;
    bool loading_ = false;
    bool dirty_ = false;
    int activePage_ = 0;

    static void SetChecked(HWND control, bool checked)
    {
        if (control)
            SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    static bool Checked(HWND control)
    {
        return control && SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    static void SetNumber(HWND control, unsigned number)
    {
        if (control)
            SetWindowTextW(control, std::to_wstring(number).c_str());
    }

    static std::wstring ReadText(HWND control)
    {
        if (!control)
            return {};
        const int length = GetWindowTextLengthW(control);
        if (length < 0 || length > 32767)
            return {};
        std::wstring buffer(static_cast<size_t>(length + 1), L'\0');
        const int read = GetWindowTextW(control, buffer.data(), length + 1);
        if (read < 0)
            return {};
        buffer.resize(static_cast<size_t>(read));
        return buffer;
    }

    static bool ReadNumber(HWND control, unsigned minimum, unsigned maximum, unsigned& result)
    {
        const std::wstring text = ReadText(control);
        if (text.empty() || text.size() > 9)
            return false;
        unsigned value = 0;
        for (wchar_t ch : text)
        {
            if (ch < L'0' || ch > L'9')
                return false;
            value = value * 10 + static_cast<unsigned>(ch - L'0');
        }
        if (value < minimum || value > maximum)
            return false;
        result = value;
        return true;
    }

    void MarkModified()
    {
        if (loading_)
            return;
        dirty_ = true;
        SetWindowTextW(status_, L"Unsaved changes. Use Save settings to apply to all players.");
        if (modifiedCallback_)
            modifiedCallback_(modifiedContext_);
    }

    HWND AddControl(int page, const wchar_t* kind, const wchar_t* text,
        DWORD style, int x, int y, int width, int height, int id = 0)
    {
        HWND control = CreateWindowExW(0, kind, text,
            WS_CHILD | style, Scale(x), Scale(y), Scale(width), Scale(height), frame_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
        if (control)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(
                font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            if (page >= 0 && page < 4)
                pageControls_[page].push_back(control);
        }
        return control;
    }

    void BuildControls()
    {
        tab_ = AddControl(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_TABSTOP | TCS_TABS, 12, 10, 550, 330, kTab);
        for (int i = 0; i < 4; ++i)
        {
            const wchar_t* titles[] = { L"Playback & cache", L"Passwords", L"Diagnostics", L"Track titles" };
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(titles[i]);
            TabCtrl_InsertItem(tab_, i, &item);
        }
        // The tab-page controls are children of the frame, not of the tab,
        // so the AIMP host can resize the options frame without recreating them.
        AddControl(0, L"STATIC", L"Playback buffer (MB, 1-512):", 0, 32, 66, 306, 21);
        buffer_ = AddControl(0, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 352, 62, 88, 24, kBuffer);
        AddControl(0, L"STATIC", L"Maximum managed cache (MB, 0-20480):", 0, 32, 102, 306, 21);
        cacheLimit_ = AddControl(0, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 352, 98, 88, 24, kCacheLimit);
        AddControl(0, L"STATIC", L"Maximum cache age (days, 1-365):", 0, 32, 138, 306, 21);
        cacheAge_ = AddControl(0, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 352, 134, 88, 24, kCacheAge);
        cacheCleanup_ = AddControl(0, L"BUTTON", L"Automatically clean old managed-cache files", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 174, 385, 22, kAutoCleanup);
        warmSolid_ = AddControl(0, L"BUTTON", L"Warm solid archives in the background", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 205, 385, 22, kWarmSolid);
        AddControl(0, L"BUTTON", L"Open managed cache", BS_PUSHBUTTON | WS_TABSTOP, 32, 243, 160, 29, kOpenCache);
        AddControl(0, L"BUTTON", L"Clear managed cache...", BS_PUSHBUTTON | WS_TABSTOP, 207, 243, 170, 29, kClearCache);
        AddControl(0, L"STATIC", hostName_ && _wcsicmp(hostName_, L"xmplay") == 0 ? L"XMPlay requests memory blocks; the backend reads direct streams when possible, then uses RAM/cache." : L"Stream from archives first; use memory next and the managed cache only as a fallback.", 0, 32, 284, 500, 36);

        AddControl(1, L"STATIC", L"Password list file (one password per line):", 0, 32, 70, 440, 21);
        passwordPath_ = AddControl(1, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 32, 99, 405, 24, kPasswordPath);
        passwordBrowse_ = AddControl(1, L"BUTTON", L"Browse...", WS_TABSTOP | BS_PUSHBUTTON, 449, 97, 92, 28, kBrowsePasswords);
        promptPasswords_ = AddControl(1, L"BUTTON", L"Prompt for passwords when necessary", WS_TABSTOP | BS_AUTOCHECKBOX, 32, 156, 470, 23, kPrompt);
        savePasswords_ = AddControl(1, L"BUTTON", L"Ask before saving successful passwords to the list", WS_TABSTOP | BS_AUTOCHECKBOX, 32, 194, 480, 23, kSavePassword);
        AddControl(1, L"STATIC", L"The password list is plain text. Keep it in a private folder.", 0, 32, 248, 490, 34);

        debugLogging_ = AddControl(2, L"BUTTON", L"Enable Archive Stream diagnostics logging", WS_TABSTOP | BS_AUTOCHECKBOX, 32, 70, 470, 24, kDebugLogging);
        AddControl(2, L"STATIC", L"Log directory (blank = shared default):", 0, 32, 119, 480, 22);
        logDirectory_ = AddControl(2, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 32, 148, 407, 24, kLogDirectory);
        logOpen_ = AddControl(2, L"BUTTON", L"Open log folder", WS_TABSTOP | BS_PUSHBUTTON, 32, 193, 157, 30, kOpenLog);
        AddControl(2, L"STATIC", L"Each host writes a separate log file. Changes apply to this player immediately.", 0, 32, 257, 500, 38);

        AddControl(3, L"STATIC", L"Archive-entry title preset:", 0, 32, 77, 470, 22);
        titleTemplate_ = AddControl(3, WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 32, 113, 464, 208, kTitleTemplate);
        for (const wchar_t* preset : titleTemplates_)
            SendMessageW(titleTemplate_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset));
        useFileTags_ = AddControl(3, L"BUTTON", L"Use File Tags (overrides title preset)", WS_TABSTOP | BS_AUTOCHECKBOX, 32, 165, 480, 25, kUseFileTags);
        AddControl(3, L"STATIC", L"Prefix (preset titles only):", 0, 32, 202, 235, 20);
        titlePrefix_ = AddControl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 32, 225, 215, 24, kTitlePrefix);
        AddControl(3, L"STATIC", L"Postfix (preset titles only):", 0, 268, 202, 235, 20);
        titlePostfix_ = AddControl(3, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 268, 225, 215, 24, kTitlePostfix);
        AddControl(3, L"STATIC", L"Use File Tags overrides presets and decorations; embedded audio and archive paths stay unchanged.", 0, 32, 268, 500, 40);

        // AIMP already owns Apply/OK/Reset. A second Save/Reload row is
        // redundant and visually conflicts with its Options page.
        if (_wcsicmp(hostName_, L"aimp") != 0)
        {
            save_ = AddControl(-1, L"BUTTON", L"Save settings", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 22, 352, 134, 32, kSave);
            reload_ = AddControl(-1, L"BUTTON", L"Reload", WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 167, 352, 112, 32, kReload);
            status_ = AddControl(-1, L"STATIC", L"", WS_VISIBLE, 22, 390, 560, 39);
        }
        TabCtrl_SetCurSel(tab_, 0);
        ShowPage(0);
        Load();
    }

    void ShowPage(int page)
    {
        activePage_ = std::clamp(page, 0, 3);
        for (int i = 0; i < 4; ++i)
        {
            for (HWND control : pageControls_[i])
                ShowWindow(control, activePage_ == i ? SW_SHOW : SW_HIDE);
        }
        if (frame_)
            InvalidateRect(frame_, nullptr, TRUE);
    }

    void Layout()
    {
        if (!frame_ || !tab_)
            return;
        RECT bounds{};
        GetClientRect(frame_, &bounds);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int footerSpace = save_ ? Scale(90) : Scale(20);
        MoveWindow(tab_, Scale(12), Scale(10), std::max(Scale(350), width - Scale(24)),
            std::max(Scale(290), height - footerSpace - Scale(10)), TRUE);
        if (save_ && reload_ && status_)
        {
            const int footer = std::max(Scale(336), height - Scale(72));
            MoveWindow(save_, Scale(22), footer, Scale(134), Scale(30), TRUE);
            MoveWindow(reload_, Scale(167), footer, Scale(112), Scale(30), TRUE);
            MoveWindow(status_, Scale(22), footer + Scale(35), std::max(Scale(280), width - Scale(40)), Scale(33), TRUE);
        }
        const int pathWidth = std::max(Scale(140), std::min(Scale(450), width - Scale(177)));
        MoveWindow(passwordPath_, Scale(32), Scale(99), pathWidth, Scale(24), TRUE);
        MoveWindow(passwordBrowse_, Scale(43) + pathWidth, Scale(97), Scale(92), Scale(28), TRUE);
        MoveWindow(logDirectory_, Scale(32), Scale(148), std::max(Scale(180), width - Scale(90)), Scale(24), TRUE);
        // AIMP can embed us in a narrower host page than the design width,
        // particularly at 150% DPI. Clip every control, including checkboxes
        // and title preset combinations, before the host paints it.
        for (const auto& page : pageControls_)
        {
            for (HWND control : page)
            {
                RECT child{};
                GetWindowRect(control, &child);
                MapWindowPoints(nullptr, frame_, reinterpret_cast<POINT*>(&child), 2);
                const int maxRight = width - Scale(16);
                if (child.right > maxRight && maxRight > child.left)
                    MoveWindow(control, child.left, child.top, maxRight - child.left, child.bottom - child.top, TRUE);
            }
        }
    }

    void BrowsePasswords()
    {
        wchar_t selected[4096]{};
        const std::wstring current = ReadText(passwordPath_);
        wcsncpy_s(selected, current.c_str(), _TRUNCATE);
        static const wchar_t filter[] = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
        OPENFILENAMEW chooser{};
        chooser.lStructSize = sizeof(chooser);
        chooser.hwndOwner = frame_;
        chooser.lpstrFile = selected;
        chooser.nMaxFile = static_cast<DWORD>(std::size(selected));
        chooser.lpstrFilter = filter;
        chooser.lpstrTitle = L"Choose password list";
        chooser.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&chooser))
            SetWindowTextW(passwordPath_, selected);
    }

    void OpenFolder(const std::wstring& folder)
    {
        if (folder.empty())
            return;
        ShellExecuteW(frame_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OnCommand(int id, int notify)
    {
        if (notify == BN_CLICKED)
        {
            if (id == kSave)
            {
                Save();
                return;
            }
            if (id == kReload)
            {
                if (!dirty_ || MessageBoxW(frame_, L"Discard unsaved changes and reload shared settings?",
                    L"Archive Stream Player", MB_YESNO | MB_ICONQUESTION) == IDYES)
                    Load();
                return;
            }
            if (id == kBrowsePasswords)
            {
                BrowsePasswords();
                return;
            }
            if (id == kOpenCache)
            {
                OpenFolder(ArchiveUnifiedCache::Folder());
                return;
            }
            if (id == kClearCache)
            {
                if (MessageBoxW(frame_, L"Delete managed cache files? Files currently in use may be retained.",
                    L"Archive Stream Player", MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    const bool cleaned = ArchiveUnifiedCache::Cleanup(0);
                    MessageBoxW(frame_, cleaned ? L"Managed cache cleared." : L"Some cache files could not be removed.",
                        L"Archive Stream Player", MB_OK | (cleaned ? MB_ICONINFORMATION : MB_ICONWARNING));
                }
                return;
            }
            if (id == kOpenLog)
            {
                const ArchiveAdapterPreferences settings = ArchiveAdapterPreferencesLoad();
                OpenFolder(ArchiveAdapterLogging::Directory(settings));
                return;
            }
        }
        if (notify == EN_CHANGE || notify == BN_CLICKED || notify == CBN_SELCHANGE)
            MarkModified();
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<CREATESTRUCTW*>(l);
            auto* self = reinterpret_cast<ArchiveNativeSettingsPanel*>(creation->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->frame_ = hwnd;
            return TRUE;
        }
        auto* self = reinterpret_cast<ArchiveNativeSettingsPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return DefWindowProcW(hwnd, message, w, l);
        switch (message)
        {
        case WM_CREATE:
            self->BuildControls();
            self->Layout();
            return 0;
        case WM_SIZE:
            self->Layout();
            return 0;
        case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<const NMHDR*>(l);
            if (header && header->idFrom == kTab && header->code == TCN_SELCHANGE)
            {
                self->ShowPage(TabCtrl_GetCurSel(self->tab_));
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
        case WM_COMMAND:
            self->OnCommand(LOWORD(w), HIWORD(w));
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->frame_ = nullptr;
            return DefWindowProcW(hwnd, message, w, l);
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
};
