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
#include <utility>

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

    enum class ArchiveListAction
    {
        Inspect,
        AddToPlaylist
    };

    class FoobarArchiveListProcess : public threaded_process_callback
    {
    public:
        FoobarArchiveListProcess(std::wstring path, ArchiveListAction action)
            : path_(std::move(path)), action_(action)
        {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            try
            {
                abort.check();
                status.set_item("Reading archive headers");
                status.set_progress(0, 1);

                if (action_ == ArchiveListAction::Inspect)
                {
                    if (!ArchiveListEntries(path_.c_str(), entries_, error_))
                        return;
                }
                else
                {
                    const std::string pathUtf8 = WideToUtf8(path_.c_str());
                    if (pathUtf8.empty() || !FoobarBuildArchivePlaylistUrls(pathUtf8.c_str(), urls_, error_))
                    {
                        if (error_.empty())
                            error_ = "Archive path cannot be converted to UTF-8";
                        return;
                    }
                }

                abort.check();
                status.set_progress(1, 1);
            }
            catch (const exception_aborted&)
            {
                throw;
            }
            catch (const std::exception& ex)
            {
                error_ = ex.what();
            }
        }

        void on_done(ctx_t, bool aborted) override
        {
            if (aborted)
            {
                console::print("Archive Stream: archive enumeration cancelled");
                return;
            }

            if (!error_.empty())
            {
                const std::string message = "Archive Stream: archive enumeration failed: " + error_;
                console::print(message.c_str());
                popup_message::g_show(message.c_str(), "Archive Stream");
                return;
            }

            if (action_ == ArchiveListAction::Inspect)
            {
                size_t supported = 0;
                console::print(("Archive Stream: " + WideToUtf8(path_.c_str())).c_str());
                for (const ArchiveEntry& entry : entries_)
                {
                    if (entry.isFolder || !entry.isSupportedAudio)
                        continue;

                    ++supported;
                    console::print(("  [" + std::to_string(entry.index) + "] " + entry.path + " (" + std::to_string(entry.size) + " bytes)").c_str());
                }

                const std::string message = "Archive Stream: " + std::to_string(entries_.size()) + " entries, " + std::to_string(supported) + " supported audio files logged in the foobar2000 Console. Use File > Archive Stream: Add archive audio... to add playable tracks.";
                console::print(message.c_str());
                popup_message::g_show(message.c_str(), "Archive Stream");
                return;
            }

            if (urls_.empty())
            {
                popup_message::g_show("No supported audio entries were found in this archive.", "Archive Stream");
                return;
            }

            pfc::list_t<const char*> locations;
            for (const auto& url : urls_)
                locations.add_item(url.c_str());

            try
            {
                // The host playlist manager is called only on the main thread.
                if (!playlist_manager::get()->activeplaylist_add_locations(locations, true, core_api::get_main_window()))
                {
                    popup_message::g_show("foobar2000 could not add the archive entries to the active playlist.", "Archive Stream");
                    return;
                }

                console::print(("Archive Stream: requested " + std::to_string(urls_.size()) + " archive audio tracks for the active playlist").c_str());
            }
            catch (const std::exception& ex)
            {
                console::print((std::string("Archive Stream: playlist add failed: ") + ex.what()).c_str());
                popup_message::g_show(ex.what(), "Archive Stream - Playlist Error");
            }
        }

    private:
        std::wstring path_;
        ArchiveListAction action_;
        std::string error_;
        std::vector<ArchiveEntry> entries_;
        std::vector<std::string> urls_;
    };

    void StartArchiveListProcess(ArchiveListAction action)
    {
        wchar_t filename[32768] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = core_api::get_main_window();
        dialog.lpstrFilter = L"Archives (*.zip;*.7z;*.rar;*.zip.001;*.7z.001)\0*.zip;*.7z;*.rar;*.zip.001;*.7z.001\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = _countof(filename);
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        dialog.lpstrTitle = action == ArchiveListAction::Inspect ? L"Archive Stream - Inspect Archive" : L"Archive Stream - Add Archive Audio";

        if (!GetOpenFileNameW(&dialog))
            return;

        try
        {
            auto worker = fb2k::service_new<FoobarArchiveListProcess>(std::wstring(filename), action);
            const unsigned flags = threaded_process::flag_show_abort | threaded_process::flag_show_progress | threaded_process::flag_show_item;
            threaded_process::get()->run_modeless(worker, flags, core_api::get_main_window(), "Archive Stream - Enumerating Archive");
        }
        catch (const std::exception& ex)
        {
            const std::string message = std::string("Archive Stream: unable to begin archive enumeration: ") + ex.what();
            console::print(message.c_str());
            popup_message::g_show(message.c_str(), "Archive Stream");
        }
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
                StartArchiveListProcess(ArchiveListAction::Inspect);
            else if (index == 1)
                StartArchiveListProcess(ArchiveListAction::AddToPlaylist);
            else
                FoobarVerifyArchiveDecoding();
        }
    };
    mainmenu_commands_factory_t<ArchiveStreamMenu> g_archiveStreamMenu;
}
