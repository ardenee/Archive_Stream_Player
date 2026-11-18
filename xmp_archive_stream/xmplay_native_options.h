#pragma once

// Compact XMPlay-hosted configuration UI, styled after XMPlay's own SDK
// examples (Tahoma 8pt, 262x175 dialog units, standard OK/Cancel/Apply).
// Only the settings backend is shared with other player adapters.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <array>
#include <string>
#include "archive_adapter_preferences.h"
#include "archive_adapter_logging.h"
#include "archive_unified_cache.h"
#include "archive_passwords.h"

class XmplayNativeOptions
{
public:
    INT_PTR Show(HMODULE module, HWND owner)
    {
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_TAB_CLASSES};
        InitCommonControlsEx(&init);
        return DialogBoxParamW(module, MAKEINTRESOURCEW(2000), owner, DialogProc, reinterpret_cast<LPARAM>(this));
    }

    // XMPARC_FLAG_OPTIONS: XMPlay creates resource 1000 within its own
    // Options tree. The instance must outlive that host-owned child page.
    static INT_PTR CALLBACK HostedPageProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
    {
        static XmplayNativeOptions hosted;
        if (message == WM_INITDIALOG)
        {
            INITCOMMONCONTROLSEX init{sizeof(init), ICC_TAB_CLASSES};
            InitCommonControlsEx(&init);
            return DialogProc(dialog, message, wparam, reinterpret_cast<LPARAM>(&hosted));
        }
        return DialogProc(dialog, message, wparam, lparam);
    }

private:
    enum : int
    {
        Tab = 4001, Apply = 4002,
        Buffer = 4101, CacheSize = 4102, CacheAge = 4103, AutoCleanup = 4104,
        WarmSolid = 4105, OpenCache = 4106, ClearCache = 4107,
        BufferLabel = 4111, CacheSizeLabel = 4112, CacheAgeLabel = 4113,
        PasswordPath = 4201, BrowsePassword = 4202, PromptPassword = 4203,
        SavePassword = 4204, PasswordLabel = 4211, PasswordHelp = 4212,
        EnableLogging = 4301, LogDirectory = 4302, OpenLog = 4303, LogLabel = 4311,
        TitlePreset = 4401, TitleLabel = 4411, TitleHelp1 = 4412, TitleHelp2 = 4413
    };

    static constexpr std::array<const wchar_t*, 6> TitlePresets = {
        L"<filename>", L"<archivetype>: <filename>", L"<#>. <filename>",
        L"<filename> [<filetype>]", L"<archivetype>: <#> - <filename>",
        L"[<archive>] <#> - <filename>"
    };
    HWND dialog_ = nullptr;
    bool loading_ = false;

    static std::wstring ReadText(HWND dialog, int id)
    {
        const HWND control = GetDlgItem(dialog, id);
        if (!control)
            return {};
        const int length = GetWindowTextLengthW(control);
        if (length < 0 || length > 4095)
            return {};
        std::wstring value(static_cast<size_t>(length + 1), L'\0');
        const int copied = GetWindowTextW(control, value.data(), length + 1);
        if (copied < 0)
            return {};
        value.resize(static_cast<size_t>(copied));
        return value;
    }

    static bool ReadNumber(HWND dialog, int id, unsigned minimum, unsigned maximum, unsigned& value)
    {
        const std::wstring text = ReadText(dialog, id);
        if (text.empty() || text.size() > 9)
            return false;
        unsigned number = 0;
        for (const wchar_t ch : text)
        {
            if (ch < L'0' || ch > L'9')
                return false;
            number = number * 10 + static_cast<unsigned>(ch - L'0');
        }
        if (number < minimum || number > maximum)
            return false;
        value = number;
        return true;
    }

    static bool Checked(HWND dialog, int id)
    {
        return SendDlgItemMessageW(dialog, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    static void Check(HWND dialog, int id, bool value)
    {
        SendDlgItemMessageW(dialog, id, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    static void SetNumber(HWND dialog, int id, unsigned value)
    {
        SetDlgItemTextW(dialog, id, std::to_wstring(value).c_str());
    }

    void Load()
    {
        loading_ = true;
        const auto values = ArchiveAdapterPreferencesLoad();
        SetNumber(dialog_, Buffer, values.memoryMb);
        SetNumber(dialog_, CacheSize, values.cacheLimitMb);
        SetNumber(dialog_, CacheAge, values.cacheAgeDays);
        Check(dialog_, AutoCleanup, values.cacheAutoCleanup);
        Check(dialog_, WarmSolid, values.warmSolidArchives);
        Check(dialog_, PromptPassword, values.promptPasswords);
        Check(dialog_, SavePassword, values.askToSavePassword);
        Check(dialog_, EnableLogging, values.debugLogging);
        SetDlgItemTextW(dialog_, PasswordPath, values.passwordListPath.c_str());
        SetDlgItemTextW(dialog_, LogDirectory, values.logDirectory.c_str());
        int index = 0;
        for (size_t i = 0; i < TitlePresets.size(); ++i)
        {
            if (values.titleTemplate == TitlePresets[i])
            {
                index = static_cast<int>(i);
                break;
            }
        }
        SendDlgItemMessageW(dialog_, TitlePreset, CB_SETCURSEL, index, 0);
        EnableWindow(GetDlgItem(dialog_, Apply), FALSE);
        loading_ = false;
    }

    bool Save()
    {
        ArchiveAdapterPreferences values = ArchiveAdapterPreferencesLoad();
        if (!ReadNumber(dialog_, Buffer, 1, 512, values.memoryMb) ||
            !ReadNumber(dialog_, CacheSize, 0, 20480, values.cacheLimitMb) ||
            !ReadNumber(dialog_, CacheAge, 1, 365, values.cacheAgeDays))
        {
            MessageBoxW(dialog_, L"Use a buffer from 1-512 MB, cache from 0-20480 MB, and age from 1-365 days.",
                L"Archive Stream Player", MB_OK | MB_ICONWARNING);
            return false;
        }
        values.cacheAutoCleanup = Checked(dialog_, AutoCleanup);
        values.warmSolidArchives = Checked(dialog_, WarmSolid);
        values.promptPasswords = Checked(dialog_, PromptPassword);
        values.askToSavePassword = Checked(dialog_, SavePassword);
        values.debugLogging = Checked(dialog_, EnableLogging);
        values.passwordListPath = ReadText(dialog_, PasswordPath);
        values.logDirectory = ReadText(dialog_, LogDirectory);
        const int selected = static_cast<int>(SendDlgItemMessageW(dialog_, TitlePreset, CB_GETCURSEL, 0, 0));
        if (selected >= 0 && selected < static_cast<int>(TitlePresets.size()))
            values.titleTemplate = TitlePresets[static_cast<size_t>(selected)];
        if (!ArchiveAdapterPreferencesSave(values))
        {
            MessageBoxW(dialog_, L"Unable to save shared Archive Stream settings.",
                L"Archive Stream Player", MB_OK | MB_ICONERROR);
            return false;
        }
        const std::string passwordList = ArchiveAdapterWideToUtf8(values.passwordListPath);
        if (!passwordList.empty())
            ArchivePasswordSetListPath(passwordList.c_str());
        ArchiveAdapterLogging::Configure(L"xmplay", values.debugLogging);
        EnableWindow(GetDlgItem(dialog_, Apply), FALSE);
        return true;
    }

    void ShowPage(int page)
    {
        static constexpr int playback[] = {
            Buffer, CacheSize, CacheAge, AutoCleanup, WarmSolid, OpenCache, ClearCache,
            BufferLabel, CacheSizeLabel, CacheAgeLabel
        };
        static constexpr int passwords[] = {
            PasswordPath, BrowsePassword, PromptPassword, SavePassword, PasswordLabel, PasswordHelp
        };
        static constexpr int diagnostics[] = {EnableLogging, LogDirectory, OpenLog, LogLabel};
        static constexpr int titles[] = {TitlePreset, TitleLabel, TitleHelp1, TitleHelp2};
        const std::array<const int*, 4> controls{playback, passwords, diagnostics, titles};
        const std::array<size_t, 4> sizes{
            std::size(playback), std::size(passwords), std::size(diagnostics), std::size(titles)
        };
        for (int index = 0; index < 4; ++index)
        {
            for (size_t j = 0; j < sizes[index]; ++j)
            {
                if (const HWND control = GetDlgItem(dialog_, controls[index][j]))
                    ShowWindow(control, page == index ? SW_SHOW : SW_HIDE);
            }
        }
    }

    void BrowsePasswordFile()
    {
        wchar_t path[4096]{};
        const std::wstring current = ReadText(dialog_, PasswordPath);
        wcsncpy_s(path, current.c_str(), _TRUNCATE);
        static const wchar_t filter[] = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
        OPENFILENAMEW chooser{};
        chooser.lStructSize = sizeof(chooser);
        chooser.hwndOwner = dialog_;
        chooser.lpstrFile = path;
        chooser.nMaxFile = static_cast<DWORD>(std::size(path));
        chooser.lpstrFilter = filter;
        chooser.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        chooser.lpstrTitle = L"Choose archive password list";
        if (GetOpenFileNameW(&chooser))
            SetDlgItemTextW(dialog_, PasswordPath, path);
    }

    void OpenFolder(const std::wstring& folder)
    {
        if (!folder.empty())
            ShellExecuteW(dialog_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OnCommand(WORD id, WORD notification)
    {
        if (notification == BN_CLICKED)
        {
            switch (id)
            {
            case IDOK:
                if (Save())
                    EndDialog(dialog_, IDOK);
                return;
            case IDCANCEL:
                EndDialog(dialog_, IDCANCEL);
                return;
            case Apply:
                Save();
                return;
            case BrowsePassword:
                BrowsePasswordFile();
                return;
            case OpenCache:
                OpenFolder(ArchiveUnifiedCache::Folder());
                return;
            case ClearCache:
                if (MessageBoxW(dialog_, L"Delete managed cache files not currently in use?",
                    L"Archive Stream Player", MB_YESNO | MB_ICONQUESTION) == IDYES)
                {
                    const bool cleaned = ArchiveUnifiedCache::Cleanup(0);
                    MessageBoxW(dialog_, cleaned ? L"Managed cache cleared." : L"Some files could not be deleted.",
                        L"Archive Stream Player", MB_OK | (cleaned ? MB_ICONINFORMATION : MB_ICONWARNING));
                }
                return;
            case OpenLog:
                OpenFolder(ArchiveAdapterLogging::Directory(ArchiveAdapterPreferencesLoad()));
                return;
            }
        }
        if (!loading_ && (notification == EN_CHANGE || notification == BN_CLICKED || notification == CBN_SELCHANGE))
            EnableWindow(GetDlgItem(dialog_, Apply), TRUE);
    }

    static INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == WM_INITDIALOG)
        {
            auto* self = reinterpret_cast<XmplayNativeOptions*>(lparam);
            if (!self)
                return FALSE;
            self->dialog_ = dialog;
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            constexpr const wchar_t* tabs[] = {
                L"Playback & cache", L"Passwords", L"Diagnostics", L"Track titles"
            };
            for (int i = 0; i < 4; ++i)
            {
                TCITEMW item{};
                item.mask = TCIF_TEXT;
                item.pszText = const_cast<wchar_t*>(tabs[i]);
                TabCtrl_InsertItem(GetDlgItem(dialog, Tab), i, &item);
            }
            for (const auto* preset : TitlePresets)
                SendDlgItemMessageW(dialog, TitlePreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset));
            self->Load();
            self->ShowPage(0);
            return TRUE;
        }
        auto* self = reinterpret_cast<XmplayNativeOptions*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
        if (!self)
            return FALSE;
        if (message == WM_NOTIFY)
        {
            const auto* notice = reinterpret_cast<const NMHDR*>(lparam);
            if (notice && notice->idFrom == Tab && notice->code == TCN_SELCHANGE)
            {
                self->ShowPage(TabCtrl_GetCurSel(GetDlgItem(dialog, Tab)));
                return TRUE;
            }
        }
        if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN)
        {
            // Avoid drawing a rectangular label background over XMPlay's
            // themed tab page; let the tab show through all labels.
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetStockObject(HOLLOW_BRUSH));
        }
        if (message == WM_COMMAND)
        {
            self->OnCommand(LOWORD(wparam), HIWORD(wparam));
            return TRUE;
        }
        if (message == WM_CLOSE)
        {
            // XMPlay owns its embedded Options page. It must never be
            // destroyed as though it were our modal Config dialog.
            if ((GetWindowLongPtrW(dialog, GWL_STYLE) & WS_CHILD) != 0)
                return FALSE;
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(dialog, GWLP_USERDATA, 0);
            self->dialog_ = nullptr;
        }
        return FALSE;
    }
};
