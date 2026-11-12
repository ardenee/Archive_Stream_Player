#include <foobar2000/SDK/foobar2000.h>
#include "foobar_decode_test.h"
#include "foobar_archive_reader.h"

#include <algorithm>
#include <commdlg.h>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    std::string ToUtf8(const wchar_t* value)
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
        if (count <= 1)
            return {};

        std::string result(static_cast<size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), count, nullptr, nullptr) != count)
            return {};

        result.resize(static_cast<size_t>(count - 1));
        return result;
    }

    // Run SDK decoders on a foobar-managed worker thread. Previously, this
    // entire validation ran on the main UI thread with fb2k::noAbort.
    class FoobarArchiveDecodeVerifyProcess : public threaded_process_callback
    {
    public:
        explicit FoobarArchiveDecodeVerifyProcess(std::string archivePath)
            : archivePath_(std::move(archivePath))
        {
        }

        void run(threaded_process_status& status, abort_callback& abort) override
        {
            try
            {
                abort.check();
                std::vector<std::string> urls;
                std::string error;
                if (!FoobarBuildArchivePlaylistUrls(archivePath_.c_str(), urls, error))
                {
                    abort.check();
                    failure_ = error.empty() ? "Cannot list archive audio entries" : error;
                    return;
                }

                abort.check();
                if (urls.empty())
                {
                    failure_ = "No supported audio entries were found in the archive";
                    return;
                }

                for (size_t index = 0; index < urls.size(); ++index)
                {
                    abort.check();
                    status.poll_pause();
                    status.set_progress(index, urls.size());
                    status.set_progress_secondary(0);
                    status.set_item(urls[index].c_str());

                    try
                    {
                        VerifyOne(urls[index], status, abort);
                    }
                    catch (const exception_aborted&)
                    {
                        throw;
                    }
                    catch (const std::exception& ex)
                    {
                        ++failed_;
                        log_ += "Archive Stream decoder FAIL: " + urls[index] + " error=" + ex.what() + "\n";
                    }
                }

                status.set_progress(urls.size(), urls.size());
                status.set_progress_secondary_float(1.0);
                abort.check();
            }
            catch (const exception_aborted&)
            {
                // Let the SDK mark this operation as cancelled, not failed.
                throw;
            }
            catch (const std::exception& ex)
            {
                failure_ = ex.what();
            }
        }

        void on_done(ctx_t, bool wasAborted) override
        {
            if (!log_.empty())
                console::print(log_.c_str());

            if (wasAborted)
            {
                console::print("Archive Stream: decoder verification cancelled");
                return;
            }

            if (!failure_.empty())
            {
                console::print(("Archive Stream: decoder verification failed: " + failure_).c_str());
                popup_message::g_show(failure_.c_str(), "Archive Stream - Verify Audio");
                return;
            }

            const std::string message = "Archive Stream: " + std::to_string(passed_) + " decoded, " + std::to_string(failed_) + " failed, " + std::to_string(totalFrames_) + " PCM sample frames in total. Details are in the foobar2000 Console.";
            console::print(message.c_str());
            popup_message::g_show(message.c_str(), "Archive Stream - Verify Audio");
        }

    private:
        void VerifyOne(const std::string& url, threaded_process_status& status, abort_callback& abort)
        {
            abort.check();
            input_decoder::ptr decoder;
            input_entry::g_open_for_decoding(decoder, nullptr, url.c_str(), abort);

            file_info_impl info;
            decoder->get_info(0, info, abort);
            decoder->initialize(0, 0, abort);

            audio_chunk_impl chunk;
            uint64_t frames = 0;
            unsigned sampleRate = 0;
            unsigned channels = 0;
            double decodedSeconds = 0;
            const double duration = info.get_length();

            while (decoder->run(chunk, abort))
            {
                abort.check();
                status.poll_pause();
                if (chunk.is_empty())
                    continue;

                frames += chunk.get_sample_count();
                sampleRate = chunk.get_srate();
                channels = chunk.get_channels();
                if (duration > 0)
                {
                    decodedSeconds += chunk.get_duration();
                    status.set_progress_secondary_float((std::min)(1.0, decodedSeconds / duration));
                }
            }

            if (frames == 0 || sampleRate == 0 || channels == 0)
                throw exception_io_data();

            abort.check();
            bool seekVerified = false;
            if (duration > 0.5)
            {
                if (!decoder->can_seek())
                    throw exception_io_unsupported_format();

                decoder->seek(duration * 0.5, abort);
                if (!decoder->run(chunk, abort) || chunk.is_empty())
                    throw exception_io_data();

                seekVerified = true;
            }

            abort.check();
            totalFrames_ += frames;
            ++passed_;
            log_ += "Archive Stream decoder PASS: " + url + " frames=" + std::to_string(frames) + " rate=" + std::to_string(sampleRate) + " channels=" + std::to_string(channels) + " seek=" + (seekVerified ? "PASS" : "SKIPPED_SHORT_TRACK") + "\n";
        }

        std::string archivePath_;
        std::string failure_;
        std::string log_;
        uint32_t passed_ = 0;
        uint32_t failed_ = 0;
        uint64_t totalFrames_ = 0;
    };
}

void FoobarVerifyArchiveDecoding()
{
    wchar_t filename[32768] = {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = core_api::get_main_window();
    dialog.lpstrFilter = L"Archives (*.zip;*.7z;*.rar;*.zip.001;*.7z.001)\0*.zip;*.7z;*.rar;*.zip.001;*.7z.001\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = _countof(filename);
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    dialog.lpstrTitle = L"Archive Stream - Verify Audio Decoding";

    if (!GetOpenFileNameW(&dialog))
        return;

    try
    {
        const std::string archivePath = ToUtf8(filename);
        if (archivePath.empty())
            throw exception_io_data();

        auto worker = fb2k::service_new<FoobarArchiveDecodeVerifyProcess>(archivePath);
        const unsigned flags = threaded_process::flag_show_abort | threaded_process::flag_show_progress_dual | threaded_process::flag_show_item;
        threaded_process::get()->run_modeless(worker, flags, core_api::get_main_window(), "Archive Stream - Verify Audio Decoding");
    }
    catch (const std::exception& ex)
    {
        console::print((std::string("Archive Stream: could not start decoder verification: ") + ex.what()).c_str());
        popup_message::g_show(ex.what(), "Archive Stream - Verify Audio");
    }
}
