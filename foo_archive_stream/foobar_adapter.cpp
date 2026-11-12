#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <foobar2000/SDK/foobar2000.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "foobar_settings.h"
#include "debug_log.h"
#include <commdlg.h>
#include <string>
#include <vector>

DECLARE_COMPONENT_VERSION("Archive Stream Player - foobar2000", "0.1.0", "Archive inspection preview; audio playback is not yet implemented.");
VALIDATE_COMPONENT_FILENAME("foo_archive_stream.dll");

namespace
{
    const GUID kInspectGuid = { 0xdd17ac76, 0x958d, 0x4fc5, { 0xaa, 0x6b, 0x78, 0x9c, 0x70, 0xde, 0x0d, 0x45 } };

    std::string WideToUtf8(const wchar_t* input)
    {
        if (!input || !input[0])
            return {};

        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1)
            return {};

        std::string result(static_cast<size_t>(length), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, result.data(), length, nullptr, nullptr) != length)
            return {};

        result.resize(static_cast<size_t>(length - 1));
        return result;
    }

    class ArchiveStreamInit : public initquit
    {
    public:
        void on_init() override
        {
            if (!ArchiveSetDataDirectoryName("FoobarArchiveStream"))
                console::print("Archive Stream: could not configure data folder");
            else
            {
                FoobarArchiveSettingsApplyBackend(FoobarArchiveSettingsLoad());
                console::print("Archive Stream: foobar2000 inspection component initialized");
            }
        }
        void on_quit() override
        {
            DebugLogShutdown();
        }
    };
    FB2K_SERVICE_FACTORY(ArchiveStreamInit);

    void InspectArchive()
    {
        wchar_t filename[32768] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = core_api::get_main_window();
        dialog.lpstrFilter = L"Archives (*.zip;*.7z;*.rar;*.zip.001;*.7z.001)\0*.zip;*.7z;*.rar;*.zip.001;*.7z.001\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = _countof(filename);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        dialog.lpstrTitle = L"Archive Stream - Inspect Archive";

        if (!GetOpenFileNameW(&dialog))
            return;

        std::vector<ArchiveEntry> entries;
        std::string error;
        if (!ArchiveListEntries(filename, entries, error))
        {
            std::string message = "Archive Stream: failed to inspect archive: " + error;
            console::print(message.c_str());
            popup_message::g_show(message.c_str(), "Archive Stream");
            return;
        }

        size_t supported = 0;
        console::print(("Archive Stream: " + WideToUtf8(filename)).c_str());
        for (const ArchiveEntry& entry : entries)
        {
            if (entry.isFolder || !entry.isSupportedAudio)
                continue;

            ++supported;
            console::print(("  [" + std::to_string(entry.index) + "] " + entry.path + " (" + std::to_string(entry.size) + " bytes)").c_str());
        }

        std::string message = "Archive Stream: " + std::to_string(entries.size()) + " entries, " + std::to_string(supported) + " supported audio files logged in the foobar2000 Console. Playback is not yet implemented.";
        console::print(message.c_str());
        popup_message::g_show(message.c_str(), "Archive Stream");
    }

    class ArchiveStreamMenu : public mainmenu_commands
    {
    public:
        t_uint32 get_command_count() override { return 1; }
        GUID get_command(t_uint32 index) override { (void)index; return kInspectGuid; }
        void get_name(t_uint32 index, pfc::string_base& out) override { (void)index; out = "Archive Stream: Inspect archive..."; }
        bool get_description(t_uint32 index, pfc::string_base& out) override
        {
            (void)index;
            out = "Lists archived audio in the foobar2000 Console; no audio decoder is registered yet.";
            return true;
        }
        GUID get_parent() override { return mainmenu_groups::file; }
        void execute(t_uint32 index, service_ptr_t<service_base> callback) override
        {
            (void)index;
            (void)callback;
            InspectArchive();
        }
    };
    mainmenu_commands_factory_t<ArchiveStreamMenu> g_archiveStreamMenu;
}
