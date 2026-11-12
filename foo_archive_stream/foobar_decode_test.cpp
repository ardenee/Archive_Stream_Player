#include <foobar2000/SDK/foobar2000.h>
#include "foobar_decode_test.h"
#include "foobar_archive_reader.h"

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

    std::vector<std::string> urls;
    std::string error;
    const std::string archivePath = ToUtf8(filename);
    if (!FoobarBuildArchivePlaylistUrls(archivePath.c_str(), urls, error))
    {
        popup_message::g_show(error.c_str(), "Archive Stream - Verify Audio");
        return;
    }

    uint32_t passed = 0;
    uint32_t failed = 0;
    uint64_t totalFrames = 0;

    for (const auto& url : urls)
    {
        try
        {
            input_decoder::ptr decoder;
            input_entry::g_open_for_decoding(decoder, nullptr, url.c_str(), fb2k::noAbort);

            file_info_impl info;
            decoder->get_info(0, info, fb2k::noAbort);
            decoder->initialize(0, 0, fb2k::noAbort);

            audio_chunk_impl chunk;
            uint64_t decodedFrames = 0;
            unsigned sampleRate = 0;
            unsigned channels = 0;

            while (decoder->run(chunk, fb2k::noAbort))
            {
                if (!chunk.is_empty())
                {
                    decodedFrames += chunk.get_sample_count();
                    sampleRate = chunk.get_srate();
                    channels = chunk.get_channels();
                }
            }

            if (decodedFrames == 0 || sampleRate == 0 || channels == 0)
                throw exception_io_data();

            if (decoder->can_seek() && info.get_length() > 0.5)
            {
                decoder->seek(info.get_length() * 0.5, fb2k::noAbort);
                if (!decoder->run(chunk, fb2k::noAbort) || chunk.is_empty())
                    throw exception_io_data();
            }

            totalFrames += decodedFrames;
            ++passed;
            console::print(("Archive Stream decoder PASS: " + url + " frames=" + std::to_string(decodedFrames) + " rate=" + std::to_string(sampleRate) + " channels=" + std::to_string(channels)).c_str());
        }
        catch (const std::exception& ex)
        {
            ++failed;
            console::print(("Archive Stream decoder FAIL: " + url + " error=" + ex.what()).c_str());
        }
    }

    const std::string message = "Archive Stream: " + std::to_string(passed) + " decoded, " + std::to_string(failed) + " failed, " + std::to_string(totalFrames) + " PCM sample frames in total. Details are in the foobar2000 Console.";
    console::print(message.c_str());
    popup_message::g_show(message.c_str(), "Archive Stream - Verify Audio");
}
