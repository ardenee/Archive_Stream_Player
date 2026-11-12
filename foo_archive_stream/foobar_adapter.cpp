#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <foobar2000/SDK/foobar2000.h>
#include "archive_backend.h"
#include "archive_common.h"
#include "foobar_settings.h"
#include "foobar_archive_reader.h"
#include "foobar_decode_test.h"
#include "debug_log.h"
#include <commdlg.h>
#include <string>
#include <vector>

DECLARE_COMPONENT_VERSION("Archive Stream Player - foobar2000", "0.2.0", "Native ZIP, 7z and RAR virtual entry streams for foobar2000 audio decoders.");
VALIDATE_COMPONENT_FILENAME("foo_archive_stream.dll");

namespace
{
    const GUID kInspectGuid = { 0xdd17ac76, 0x958d, 0x4fc5, { 0xaa, 0x6b, 0x78, 0x9c, 0x70, 0xde, 0x0d, 0x45 } };
    const GUID kAddAudioGuid = { 0x4fa2f27b, 0x4a32, 0x49dd, { 0xb2, 0xb1, 0x21, 0x64, 0x84, 0x25, 0x80, 0xc0 } };
    const GUID kVerifyGuid = { 0xb1bf17d8, 0x4fcf, 0x4cfa, { 0x93, 0x07, 0xea, 0x5b, 0x38, 0xd4, 0xc2, 0x19 } };

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
                console::print("Archive Stream: foobar2000 archive virtual-file adapter initialized");
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

        std::string message = "Archive Stream: " + std::to_string(entries.size()) + " entries, " + std::to_string(supported) + " supported audio files logged in the foobar2000 Console. Use File > Archive Stream: Add archive audio... to add playable tracks.";
        console::print(message.c_str());
        popup_message::g_show(message.c_str(), "Archive Stream");
    }

    void AddArchiveAudio()
    {
        wchar_t filename[32768] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = core_api::get_main_window();
        dialog.lpstrFilter = L"Archives (*.zip;*.7z;*.rar;*.zip.001;*.7z.001)\0*.zip;*.7z;*.rar;*.zip.001;*.7z.001\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = _countof(filename);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        dialog.lpstrTitle = L"Archive Stream - Add Archive Audio";
        if (!GetOpenFileNameW(&dialog))
            return;

        std::vector<std::string> urls;
        std::string error;
        const std::string utf8 = WideToUtf8(filename);
        if (!FoobarBuildArchivePlaylistUrls(utf8.c_str(), urls, error))
        {
            const std::string message = "Archive Stream: cannot enumerate playable archive entries: " + error;
            console::print(message.c_str());
            popup_message::g_show(message.c_str(), "Archive Stream");
            return;
        }

        if (urls.empty())
        {
            popup_message::g_show("No supported audio entries were found in this archive.", "Archive Stream");
            return;
        }

        pfc::list_t<const char*> locations;
        for (const auto& url : urls)
            locations.add_item(url.c_str());

        try
        {
            if (!playlist_manager::get()->activeplaylist_add_locations(locations, true, core_api::get_main_window()))
            {
                popup_message::g_show("foobar2000 could not add the archive entries to the active playlist.", "Archive Stream");
                return;
            }
        }
        catch (const std::exception& ex)
        {
            console::print((std::string("Archive Stream: playlist add failed: ") + ex.what()).c_str());
            popup_message::g_show(ex.what(), "Archive Stream - Playlist Error");
            return;
        }

        console::print(("Archive Stream: requested " + std::to_string(urls.size()) + " archive audio tracks for the active playlist").c_str());
    }

    class ArchiveStreamMenu : public mainmenu_commands
    {
    public:
        t_uint32 get_command_count() override { return 3; }
        GUID get_command(t_uint32 index) override { return index == 0 ? kInspectGuid : index == 1 ? kAddAudioGuid : kVerifyGuid; }
        void get_name(t_uint32 index, pfc::string_base& out) override { out = index == 0 ? "Archive Stream: Inspect archive..." : index == 1 ? "Archive Stream: Add archive audio..." : "Archive Stream: Verify archive decoding..."; }
        bool get_description(t_uint32 index, pfc::string_base& out) override
        {
            out = index == 0 ? "Lists supported ZIP, 7z and RAR audio in the Console." : index == 1 ? "Adds archive entries as native foobar2000 virtual tracks." : "Fully decodes archived audio with foobar2000 codecs and checks seeking, without opening an audio output device.";
            return true;
        }
        GUID get_parent() override { return mainmenu_groups::file; }
        void execute(t_uint32 index, service_ptr_t<service_base> callback) override
        {
            (void)callback;
            if (index == 0)
                InspectArchive();
            else if (index == 1)
                AddArchiveAudio();
            else
                FoobarVerifyArchiveDecoding();
        }
    };
    mainmenu_commands_factory_t<ArchiveStreamMenu> g_archiveStreamMenu;
}
