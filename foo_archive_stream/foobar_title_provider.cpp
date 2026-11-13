// User-configurable archive titles in foobar2000's titleformat pipeline.
// The host owns standard tags (%title%, etc); expose only a unique field.
// Every template change dispatches the SDK-required metadb refresh.
#include <foobar2000/SDK/foobar2000.h>
#include "../shared/archive_adapter_preferences.h"
#include "foobar_title_provider.h"
#include "foobar_settings.h"
#include "debug_log.h"

#include <atomic>
#include <charconv>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace
{
    std::mutex g_templateMutex;
    std::wstring g_template = L"<filename>";
    std::string g_prefix;
    std::string g_postfix;
    UINT_PTR g_refreshTimer = 0;
    bool g_reportedInitial = false;

    std::wstring CurrentTitleTemplate()
    {
        // Optional, process-local test fixture for live field refreshing.
        // It avoids writing to the real user's shared registry during QA.
        wchar_t file[32768]{};
        const DWORD count = GetEnvironmentVariableW(L"AST_FOOBAR_TITLE_TEST_INPUT", file, _countof(file));
        if (count != 0 && count < _countof(file))
        {
            std::wifstream stream{std::filesystem::path(file)};
            std::wstring line;
            if (stream && std::getline(stream, line))
            {
                if (line == L"<filename>" || line == L"<archivetype>: <filename>" ||
                    line == L"<#>. <filename>" || line == L"<filename> [<filetype>]" ||
                    line == L"<archivetype>: <#> - <filename>" ||
                    line == L"[<archive>] <#> - <filename>")
                    return line;
            }
        }
        return ArchiveAdapterPreferencesLoad().titleTemplate;
    }

    bool ArchiveVirtualDetails(const char* location, std::wstring& archive,
        std::wstring& name, unsigned& number)
    {
        if (!location || std::strncmp(location, "unpack://ast", 12) != 0)
            return false;

        pfc::string8 native, entry, type;
        if (!archive_impl::g_parse_unpack_path_ex(location, native, entry, type))
            return false;
        if (std::strcmp(type.c_str(), "astzip") != 0 &&
            std::strcmp(type.c_str(), "ast7z") != 0 &&
            std::strcmp(type.c_str(), "astrar") != 0)
            return false;

        const char* value = entry.c_str();
        const char* slash = std::strchr(value, '/');
        if (!slash || slash == value || !slash[1] || std::strchr(slash + 1, '/'))
            return false;
        uint32_t index = 0;
        const auto parsed = std::from_chars(value, slash, index);
        if (parsed.ec != std::errc() || parsed.ptr != slash || index == UINT32_MAX)
            return false;

        archive = ArchiveAdapterUtf8ToWide(native.c_str());
        name = ArchiveAdapterUtf8ToWide(slash + 1);
        number = index + 1;
        return !archive.empty() && !name.empty();
    }

    bool IsArchiveHandle(metadb_handle* handle)
    {
        if (!handle)
            return false;
        const char* path = handle->get_path();
        return path && (std::strncmp(path, "unpack://astzip|", 16) == 0 ||
            std::strncmp(path, "unpack://ast7z|", 15) == 0 ||
            std::strncmp(path, "unpack://astrar|", 16) == 0);
    }

    class ArchiveTitleProvider : public metadb_display_field_provider
    {
    public:
        t_uint32 get_field_count() override
        {
            return 1;
        }

        void get_field_name(t_uint32 field, pfc::string_base& out) override
        {
            out = field == 0 ? "archive_stream_title" : "";
        }

        bool process_field(t_uint32 field, metadb_handle* handle, titleformat_text_out* out) override
        {
            if (field != 0 || !out || !IsArchiveHandle(handle))
                return false;

            std::wstring archive, internal;
            unsigned number = 0;
            if (!ArchiveVirtualDetails(handle->get_path(), archive, internal, number))
                return false;

            std::wstring format;
            std::string prefix, postfix;
            {
                std::lock_guard<std::mutex> guard(g_templateMutex);
                format = g_template;
                prefix = g_prefix;
                postfix = g_postfix;
            }
            const std::wstring title = ArchiveAdapterFormatTitle(archive, internal, number, format);
            const std::string utf8 = prefix + ArchiveAdapterWideToUtf8(title) + postfix;
            if (utf8.empty())
                return false;

            out->write(titleformat_inputtypes::meta, utf8.c_str(), utf8.size());
            return true;
        }
    };
    FB2K_SERVICE_FACTORY(ArchiveTitleProvider);

    size_t RefreshAffectedPlaylists()
    {
        const auto playlists = playlist_manager::get();
        pfc::list_t<metadb_handle_ptr> affected;
        const t_size count = playlists->get_playlist_count();
        for (t_size playlist = 0; playlist < count; ++playlist)
        {
            pfc::list_t<metadb_handle_ptr> items;
            playlists->playlist_get_all_items(playlist, items);
            for (t_size index = 0; index < items.get_count(); ++index)
            {
                const auto& handle = items[index];
                if (IsArchiveHandle(handle.get_ptr()))
                    affected.add_item(handle);
            }
        }

        if (affected.get_count() != 0)
            metadb_io::get()->dispatch_refresh(affected);
        DebugLogWrite("INFO", "FOOBAR", "ARCHIVE_TITLE_REFRESH", "affected=%zu | playlist_count=%zu",
            static_cast<size_t>(affected.get_count()), static_cast<size_t>(count));

        // QA-only native-host report: verify the public foobar titleformat
        // pipeline, not just the local string formatter. No user tag edits.
        wchar_t reportFile[32768]{};
        const DWORD reportLength = GetEnvironmentVariableW(L"AST_FOOBAR_TITLE_TEST_REPORT", reportFile, _countof(reportFile));
        if (affected.get_count() && reportLength && reportLength < _countof(reportFile))
        {
            wchar_t fixture[32768]{};
            const DWORD filterLength = GetEnvironmentVariableW(L"AST_FOOBAR_TITLE_TEST_ARCHIVE_FILTER", fixture, _countof(fixture));
            const std::string filter = filterLength && filterLength < _countof(fixture)
                ? ArchiveAdapterWideToUtf8(fixture) : std::string{};
            metadb_handle_ptr selected;
            for (t_size i = 0; i < affected.get_count(); ++i)
            {
                if (filter.empty() || std::strstr(affected[i]->get_path(), filter.c_str()))
                {
                    selected = affected[i];
                    break;
                }
            }
            if (!selected.is_valid())
                return 0;
            pfc::string8 display;
            titleformat_object::ptr script;
            titleformat_compiler::get()->compile_force(script, "%archive_stream_title%");
            const bool found = selected->format_title(nullptr, display, script, nullptr);
            std::ofstream report(std::filesystem::path(reportFile), std::ios::app | std::ios::binary);
            report << "TITLE_REFRESH found=" << (found ? 1 : 0) << " count=" << affected.get_count()
                << " title=" << display.c_str() << "\n";
        }
        return static_cast<size_t>(affected.get_count());
    }

    VOID CALLBACK RefreshTimer(HWND, UINT, UINT_PTR, DWORD)
    {
        FoobarArchiveTitlesRefresh(false);
        wchar_t reportFile[32768]{};
        if (!g_reportedInitial && GetEnvironmentVariableW(L"AST_FOOBAR_TITLE_TEST_REPORT", reportFile, _countof(reportFile)) != 0)
            g_reportedInitial = RefreshAffectedPlaylists() != 0;
    }
}

void FoobarArchiveTitlesInitialize()
{
    {
        std::lock_guard<std::mutex> guard(g_templateMutex);
        g_template = CurrentTitleTemplate();
        const auto display = FoobarArchiveSettingsLoad();
        g_prefix = display.displayPrefix;
        g_postfix = display.displayPostfix;
    }
    if (!g_refreshTimer)
        g_refreshTimer = SetTimer(nullptr, 0, 1000, RefreshTimer);
    DebugLogWrite(g_refreshTimer ? "INFO" : "WARN", "FOOBAR", "ARCHIVE_TITLE_WATCH",
        "timer=%llu | field=archive_stream_title", static_cast<unsigned long long>(g_refreshTimer));
}

void FoobarArchiveTitlesRefresh(bool force)
{
    const auto current = CurrentTitleTemplate();
    const auto display = FoobarArchiveSettingsLoad();
    bool changed = false;
    {
        std::lock_guard<std::mutex> guard(g_templateMutex);
        if (current != g_template ||
            display.displayPrefix != g_prefix || display.displayPostfix != g_postfix)
        {
            g_template = current;
            g_prefix = display.displayPrefix;
            g_postfix = display.displayPostfix;
            changed = true;
        }
    }
    if (changed || force)
        RefreshAffectedPlaylists();
}

void FoobarArchiveTitlesShutdown()
{
    g_reportedInitial = false;
    if (g_refreshTimer)
    {
        KillTimer(nullptr, g_refreshTimer);
        g_refreshTimer = 0;
    }
}
