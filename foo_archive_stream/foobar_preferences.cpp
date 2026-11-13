#include <foobar2000/SDK/foobar2000.h>
#include "foobar_resource.h"
#include "foobar_settings.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_unified_cache.h"
#include "../shared/archive_adapter_logging.h"
#include <filesystem>

#include "archive_cache.h"
#include "archive_common.h"

#include <commdlg.h>
#include <climits>
#include <shellapi.h>
#include <string>

namespace
{
    const GUID kGeneralPageGuid = { 0x8af10230, 0x345e, 0x42de, {0x91,0xc5,0xab,0x81,0xf8,0x65,0x21,0x3a} };
    const GUID kDisplayPageGuid = { 0x8af10231, 0x345e, 0x42de, {0x91,0xc5,0xab,0x81,0xf8,0x65,0x21,0x3b} };

    struct TitlePreset
    {
        const char* utf8;
        const wchar_t* wide;
    };

    // Keep the same six template presets as in_archive_stream/config_ui.cpp.
    constexpr TitlePreset kTitlePresets[] = {
        { "<filename>", L"<filename>" },
        { "<archivetype>: <filename>", L"<archivetype>: <filename>" },
        { "<#>. <filename>", L"<#>. <filename>" },
        { "<filename> [<filetype>]", L"<filename> [<filetype>]" },
        { "<archivetype>: <#> - <filename>", L"<archivetype>: <#> - <filename>" },
        { "[<archive>] <#> - <filename>", L"[<archive>] <#> - <filename>" }
    };

    std::string ReadUtf8(HWND hwnd, int id, std::string& error)
    {
        HWND edit = GetDlgItem(hwnd, id);
        if (!edit)
        {
            error = "Preferences control is missing";
            return {};
        }

        const int length = GetWindowTextLengthW(edit);
        if (length < 0 || length >= 4096)
        {
            error = "Settings path or formatting text exceeds the control limit";
            return {};
        }

        std::wstring wide(static_cast<size_t>(length) + 1, L'\0');
        const int written = GetWindowTextW(edit, wide.data(), length + 1);
        if (written == 0)
            return {};

        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), written, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
        {
            error = "Settings text contains invalid Unicode";
            return {};
        }

        std::string text(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), written, text.data(), bytes, nullptr, nullptr) != bytes)
        {
            error = "Unable to convert settings text to UTF-8";
            return {};
        }

        return text;
    }

    void SetUtf8(HWND hwnd, int id, const std::string& text)
    {
        if (text.empty())
        {
            SetDlgItemTextW(hwnd, id, L"");
            return;
        }

        const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, nullptr, 0);
        if (chars <= 0)
            return;

        std::wstring wide(static_cast<size_t>(chars), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(), -1, wide.data(), chars) == chars)
            SetDlgItemTextW(hwnd, id, wide.c_str());
    }

    bool ReadPositiveNumber(HWND hwnd, int id, const char* label, int& value, std::string& error)
    {
        BOOL valid = FALSE;
        const UINT parsed = GetDlgItemInt(hwnd, id, &valid, FALSE);
        if (!valid)
        {
            error = std::string("Invalid number for ") + label;
            return false;
        }

        if (parsed > static_cast<UINT>(INT_MAX))
        {
            error = std::string("Number is too large for ") + label;
            return false;
        }

        value = static_cast<int>(parsed);
        return true;
    }

    bool Checked(HWND hwnd, int id)
    {
        return SendDlgItemMessageW(hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    void SetChecked(HWND hwnd, int id, bool checked)
    {
        SendDlgItemMessageW(hwnd, id, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void BrowsePath(HWND hwnd, int id, const wchar_t* filter)
    {
        wchar_t path[MAX_PATH * 4] = {};
        GetDlgItemTextW(hwnd, id, path, _countof(path));

        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = hwnd;
        picker.lpstrFilter = filter;
        picker.lpstrFile = path;
        picker.nMaxFile = _countof(path);
        picker.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
        picker.lpstrTitle = L"Archive Stream - Select Settings File";

        if (GetSaveFileNameW(&picker))
            SetDlgItemTextW(hwnd, id, path);
    }

    class ArchivePreferencesInstance : public preferences_page_instance
    {
    public:
        ArchivePreferencesInstance(preferences_page_callback::ptr callback, bool display)
            : callback_(callback), display_(display)
        {
        }

        t_uint32 get_state() override
        {
            return preferences_state::resettable | (changed_ ? preferences_state::changed : 0);
        }

        HWND get_wnd() override { return hwnd_; }

        void apply() override
        {
            if (!hwnd_)
                return;

            FoobarArchiveSettings settings = FoobarArchiveSettingsLoad();
            std::string error;
            if (!ReadControls(settings, error) || !FoobarArchiveSettingsSave(settings, error))
            {
                popup_message::g_show(error.c_str(), "Archive Stream - Invalid Settings");
                return;
            }

            changed_ = false;
            callback_->on_state_changed();
        }

        void reset() override
        {
            FillControls(FoobarArchiveSettingsDefaults());
            Changed();
        }

        static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_INITDIALOG)
            {
                auto* self = reinterpret_cast<ArchivePreferencesInstance*>(lParam);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
                self->hwnd_ = hwnd;
                if (self->display_)
                {
                    for (const auto& preset : kTitlePresets)
                        SendDlgItemMessageW(hwnd, IDC_DISPLAY_PRESET, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.wide));
                }

                self->FillControls(FoobarArchiveSettingsLoad());
                return TRUE;
            }

            auto* self = reinterpret_cast<ArchivePreferencesInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (!self)
                return FALSE;

            if (message == WM_NCDESTROY)
            {
                self->hwnd_ = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return FALSE;
            }

            if (message == WM_COMMAND)
            {
                const int id = LOWORD(wParam);
                const int event = HIWORD(wParam);

                if (self->display_ && id == IDC_DISPLAY_PRESET && event == CBN_SELCHANGE)
                {
                    const LRESULT index = SendDlgItemMessageW(hwnd, IDC_DISPLAY_PRESET, CB_GETCURSEL, 0, 0);
                    if (index >= 0 && index < static_cast<LRESULT>(_countof(kTitlePresets)))
                        SetUtf8(hwnd, IDC_DISPLAY_TEMPLATE, kTitlePresets[index].utf8);

                    self->Changed();
                    return TRUE;
                }

                if (event == EN_CHANGE || event == BN_CLICKED)
                {
                    if (event == BN_CLICKED && !self->display_)
                    {
                        if (id == IDC_PASSWORD_BROWSE)
                        {
                            BrowsePath(hwnd, IDC_PASSWORD_PATH, L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0");
                            return TRUE;
                        }
                        if (id == IDC_LOG_BROWSE)
                        {
                            BrowsePath(hwnd, IDC_LOG_PATH, L"Log files (*.log)\0*.log\0All files (*.*)\0*.*\0");
                            return TRUE;
                        }
                        if (id == IDC_LOG_FOLDER)
                        {
                            if (ArchiveAdapterPreferencesConfigured())
                            {
                                const std::wstring folder = ArchiveAdapterLogging::Directory(ArchiveAdapterPreferencesLoad());
                                std::error_code ec;
                                std::filesystem::create_directories(std::filesystem::path(folder), ec);
                                if (!folder.empty() && !ec &&
                                    reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32)
                                    return TRUE;
                                popup_message::g_show("Unable to open the shared diagnostic log folder.", "Archive Stream");
                                return TRUE;
                            }

                            wchar_t path[MAX_PATH * 4] = {};
                            GetDlgItemTextW(hwnd, IDC_LOG_PATH, path, _countof(path));
                            if (!path[0])
                            {
                                char defaultPath[MAX_PATH * 4] = {};
                                if (ArchiveBuildPluginDataFilePath("foo_archive_stream.log", defaultPath, sizeof(defaultPath)))
                                    MultiByteToWideChar(CP_ACP, 0, defaultPath, -1, path, _countof(path));
                            }

                            wchar_t* slash = wcsrchr(path, L'\\');
                            wchar_t* forward = wcsrchr(path, L'/');
                            if (forward && (!slash || forward > slash))
                                slash = forward;

                            if (slash)
                            {
                                *slash = L'\0';
                                ShellExecuteW(hwnd, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
                            }
                            return TRUE;
                        }
                        if (id == IDC_SHARED_SETTINGS)
                        {
                            ArchiveAdapterOpenSettings(GetModuleHandleW(L"foo_archive_stream.dll"), hwnd);
                            return TRUE;
                        }
                        if (id == IDC_CACHE_VIEW)
                        {
                            const std::wstring folder = ArchiveUnifiedCache::Folder();
                            if (!folder.empty())
                                ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            return TRUE;
                        }
                        if (id == IDC_CACHE_CLEAR)
                        {
                            if (MessageBoxW(hwnd, L"Clear reusable archive tracks shared by all players, as well as older foobar2000 cache entries? Files being used by another player may remain locked.", L"Archive Stream - Clear Cache", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                            {
                                const bool shared = ArchiveUnifiedCache::Cleanup(0);
                                const bool legacy = ArchiveCacheClear();
                                if (!shared || !legacy)
                                    popup_message::g_show("Some cached tracks could not be removed. Close any other players and try again.", "Archive Stream");
                            }
                            return TRUE;
                        }
                    }

                    self->Changed();
                }
            }

            return FALSE;
        }

    private:
        void Changed()
        {
            if (!loading_)
            {
                changed_ = true;
                callback_->on_state_changed();
            }
        }

        void FillControls(const FoobarArchiveSettings& settings)
        {
            if (!hwnd_)
                return;

            loading_ = true;

            if (display_)
            {
                SetUtf8(hwnd_, IDC_DISPLAY_PREFIX, settings.displayPrefix);
                SetUtf8(hwnd_, IDC_DISPLAY_POSTFIX, settings.displayPostfix);
                SetUtf8(hwnd_, IDC_DISPLAY_TEMPLATE, settings.displayTemplate);

                LRESULT index = -1;
                for (size_t i = 0; i < _countof(kTitlePresets); ++i)
                {
                    if (settings.displayTemplate == kTitlePresets[i].utf8)
                    {
                        index = static_cast<LRESULT>(i);
                        break;
                    }
                }
                SendDlgItemMessageW(hwnd_, IDC_DISPLAY_PRESET, CB_SETCURSEL, index, 0);
            }
            else
            {
                SetDlgItemInt(hwnd_, IDC_BUFFER_MB, static_cast<UINT>(settings.bufferSizeMb), FALSE);
                SetDlgItemInt(hwnd_, IDC_CACHE_MB, static_cast<UINT>(settings.cacheSizeMb), FALSE);
                SetDlgItemInt(hwnd_, IDC_CACHE_AGE_DAYS, static_cast<UINT>(settings.cacheAgeDays), FALSE);
                SetChecked(hwnd_, IDC_CACHE_SIZE_CLEANUP, settings.cacheSizeCleanup);
                SetChecked(hwnd_, IDC_CACHE_AGE_CLEANUP, settings.cacheAgeCleanup);
                SetUtf8(hwnd_, IDC_PASSWORD_PATH, settings.passwordListPath);
                SetChecked(hwnd_, IDC_LOGGING_ENABLED, settings.debugLogging);
                SetUtf8(hwnd_, IDC_LOG_PATH, settings.debugLogPath);
            }

            loading_ = false;
        }

        bool ReadControls(FoobarArchiveSettings& settings, std::string& error)
        {
            error.clear();

            if (display_)
            {
                settings.displayPrefix = ReadUtf8(hwnd_, IDC_DISPLAY_PREFIX, error);
                if (!error.empty())
                    return false;

                settings.displayPostfix = ReadUtf8(hwnd_, IDC_DISPLAY_POSTFIX, error);
                if (!error.empty())
                    return false;

                settings.displayTemplate = ReadUtf8(hwnd_, IDC_DISPLAY_TEMPLATE, error);
                return error.empty();
            }

            if (!ReadPositiveNumber(hwnd_, IDC_BUFFER_MB, "buffer size", settings.bufferSizeMb, error) ||
                !ReadPositiveNumber(hwnd_, IDC_CACHE_MB, "cache size", settings.cacheSizeMb, error) ||
                !ReadPositiveNumber(hwnd_, IDC_CACHE_AGE_DAYS, "cache age", settings.cacheAgeDays, error))
                return false;

            settings.cacheSizeCleanup = Checked(hwnd_, IDC_CACHE_SIZE_CLEANUP);
            settings.cacheAgeCleanup = Checked(hwnd_, IDC_CACHE_AGE_CLEANUP);
            settings.passwordListPath = ReadUtf8(hwnd_, IDC_PASSWORD_PATH, error);
            if (!error.empty())
                return false;

            settings.debugLogging = Checked(hwnd_, IDC_LOGGING_ENABLED);
            settings.debugLogPath = ReadUtf8(hwnd_, IDC_LOG_PATH, error);
            return error.empty();
        }

        preferences_page_callback::ptr callback_;
        bool display_ = false;
        bool changed_ = false;
        bool loading_ = false;
        HWND hwnd_ = nullptr;
    };

    preferences_page_instance::ptr InstantiatePage(HWND parent, preferences_page_callback::ptr callback, bool display)
    {
        auto instance = fb2k::service_new<ArchivePreferencesInstance>(callback, display);
        HMODULE module = GetModuleHandleW(L"foo_archive_stream.dll");
        const int resource = display ? IDD_ARCHIVE_DISPLAY : IDD_ARCHIVE_GENERAL;
        if (!module || !CreateDialogParamW(module, MAKEINTRESOURCEW(resource), parent, ArchivePreferencesInstance::DialogProc, reinterpret_cast<LPARAM>(instance.get_ptr())))
        {
            console::print("Archive Stream: failed to create native Preferences dialog");
            return {};
        }

        return instance;
    }

    class ArchiveGeneralPreferencesPage : public preferences_page_v3
    {
    public:
        const char* get_name() override { return "Archive Stream"; }
        GUID get_guid() override { return kGeneralPageGuid; }
        GUID get_parent_guid() override { return preferences_page::guid_tools; }
        preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr callback) override
        {
            return InstantiatePage(parent, callback, false);
        }
    };

    class ArchiveDisplayPreferencesPage : public preferences_page_v3
    {
    public:
        const char* get_name() override { return "Title Display"; }
        GUID get_guid() override { return kDisplayPageGuid; }
        GUID get_parent_guid() override { return kGeneralPageGuid; }
        preferences_page_instance::ptr instantiate(HWND parent, preferences_page_callback::ptr callback) override
        {
            return InstantiatePage(parent, callback, true);
        }
    };

    preferences_page_factory_t<ArchiveGeneralPreferencesPage> g_archiveGeneralPreferences;
    preferences_page_factory_t<ArchiveDisplayPreferencesPage> g_archiveDisplayPreferences;
}
