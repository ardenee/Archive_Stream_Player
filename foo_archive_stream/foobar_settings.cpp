#include "foobar_settings.h"

#include <foobar2000/SDK/foobar2000.h>
#include "archive_common.h"
#include "archive_cache.h"
#include "archive_passwords.h"
#include "archive_rar_reader.h"
#include "debug_log.h"
#include "../shared/archive_adapter_preferences.h"
#include "../shared/archive_adapter_logging.h"

#include <algorithm>
#include <string>

namespace
{
    // Distinct persistent GUIDs; these are exclusively foobar2000 cfg variables.
    cfg_int cfgBuffer({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x01} }, 50);
    cfg_int cfgCacheSize({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x02} }, 2048);
    cfg_int cfgSizeCleanup({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x03} }, 1);
    cfg_int cfgAgeCleanup({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x04} }, 1);
    cfg_int cfgCacheDays({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x05} }, 30);
    cfg_string cfgPrefix({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x06} }, "");
    cfg_string cfgPostfix({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x07} }, "");
    cfg_string cfgTemplate({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x08} }, "<filename>");
    cfg_string cfgPasswords({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x09} }, "");
    cfg_int cfgDebugEnabled({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x0a} }, 0);
    cfg_string cfgDebugPath({ 0x25e4857c, 0x58b2, 0x4fe6, {0xb5,0x43,0xce,0x0d,0x5d,0xb4,0x00,0x0b} }, "");

    std::string ReadString(cfg_string& value)
    {
        auto contents = value.get();
        return contents.get_ptr();
    }

    std::string ResolveDataPath(const std::string& customPath, const char* defaultName)
    {
        if (!customPath.empty())
            return customPath;

        char path[MAX_PATH * 4] = {};
        if (!ArchiveBuildPluginDataFilePath(defaultName, path, sizeof(path)))
            return {};

        return path;
    }
}

FoobarArchiveSettings FoobarArchiveSettingsDefaults()
{
    return {};
}

FoobarArchiveSettings FoobarArchiveSettingsLoad()
{
    FoobarArchiveSettings settings;
    settings.bufferSizeMb = static_cast<int>(cfgBuffer.get());
    settings.cacheSizeMb = static_cast<int>(cfgCacheSize.get());
    settings.cacheSizeCleanup = cfgSizeCleanup.get() != 0;
    settings.cacheAgeCleanup = cfgAgeCleanup.get() != 0;
    settings.cacheAgeDays = static_cast<int>(cfgCacheDays.get());
    settings.displayPrefix = ReadString(cfgPrefix);
    settings.displayPostfix = ReadString(cfgPostfix);
    settings.displayTemplate = ReadString(cfgTemplate);
    settings.passwordListPath = ReadString(cfgPasswords);
    settings.debugLogging = cfgDebugEnabled.get() != 0;
    settings.debugLogPath = ReadString(cfgDebugPath);

    // The first shared settings save becomes authoritative for every player;
    // native cfg_vars remain in place for legacy profiles and host-only fields.
    if (ArchiveAdapterPreferencesConfigured())
    {
        const auto common = ArchiveAdapterPreferencesLoad();
        settings.bufferSizeMb = static_cast<int>(common.memoryMb);
        settings.cacheSizeMb = static_cast<int>(common.cacheLimitMb);
        settings.cacheAgeDays = static_cast<int>(common.cacheAgeDays);
        settings.passwordListPath = ArchiveAdapterWideToUtf8(common.passwordListPath);
        settings.displayTemplate = ArchiveAdapterWideToUtf8(common.titleTemplate);
        settings.debugLogging = common.debugLogging;
    }

    settings.bufferSizeMb = std::clamp(settings.bufferSizeMb, static_cast<int>(MIN_BUFFER_SIZE_MB), static_cast<int>(MAX_BUFFER_SIZE_MB));
    settings.cacheSizeMb = std::clamp(settings.cacheSizeMb, static_cast<int>(MIN_CACHE_SIZE_MB), static_cast<int>(MAX_CACHE_SIZE_MB));
    settings.cacheAgeDays = std::clamp(settings.cacheAgeDays, static_cast<int>(MIN_CACHE_AGE_DAYS), static_cast<int>(MAX_CACHE_AGE_DAYS));

    // Corrupt or older stored values must never be forwarded as oversized
    // paths to legacy native readers that have bounded MAX_PATH*4 buffers.
    std::string error;
    if (!FoobarArchiveSettingsValidate(settings, error))
        return FoobarArchiveSettingsDefaults();

    return settings;
}

bool FoobarArchiveSettingsValidate(FoobarArchiveSettings& settings, std::string& error)
{
    error.clear();
    settings.bufferSizeMb = std::clamp(settings.bufferSizeMb, static_cast<int>(MIN_BUFFER_SIZE_MB), static_cast<int>(MAX_BUFFER_SIZE_MB));
    settings.cacheSizeMb = std::clamp(settings.cacheSizeMb, static_cast<int>(MIN_CACHE_SIZE_MB), static_cast<int>(MAX_CACHE_SIZE_MB));
    settings.cacheAgeDays = std::clamp(settings.cacheAgeDays, static_cast<int>(MIN_CACHE_AGE_DAYS), static_cast<int>(MAX_CACHE_AGE_DAYS));

    if (settings.displayPrefix.size() > 20 || settings.displayPostfix.size() > 20)
    {
        error = "Title prefix and postfix are limited to 20 UTF-8 bytes each";
        return false;
    }

    if (settings.displayTemplate.size() >= 160)
    {
        error = "Title template exceeds the 159-byte limit";
        return false;
    }

    if (settings.passwordListPath.size() >= MAX_PATH * 4 || settings.debugLogPath.size() >= MAX_PATH * 4)
    {
        error = "Password and log paths must be shorter than MAX_PATH * 4 bytes";
        return false;
    }

    if (settings.displayTemplate.empty())
        settings.displayTemplate = "<filename>";

    return true;
}

bool FoobarArchiveSettingsSave(const FoobarArchiveSettings& requested, std::string& error)
{
    FoobarArchiveSettings settings = requested;
    if (!FoobarArchiveSettingsValidate(settings, error))
        return false;

    cfgBuffer = settings.bufferSizeMb;
    cfgCacheSize = settings.cacheSizeMb;
    cfgSizeCleanup = settings.cacheSizeCleanup ? 1 : 0;
    cfgAgeCleanup = settings.cacheAgeCleanup ? 1 : 0;
    cfgCacheDays = settings.cacheAgeDays;
    cfgPrefix = settings.displayPrefix.c_str();
    cfgPostfix = settings.displayPostfix.c_str();
    cfgTemplate = settings.displayTemplate.c_str();
    cfgPasswords = settings.passwordListPath.c_str();
    cfgDebugEnabled = settings.debugLogging ? 1 : 0;
    cfgDebugPath = settings.debugLogPath.c_str();

    auto common = ArchiveAdapterPreferencesLoad();
    common.memoryMb = static_cast<unsigned>(settings.bufferSizeMb);
    common.cacheLimitMb = static_cast<unsigned>(settings.cacheSizeMb);
    common.cacheAgeDays = static_cast<unsigned>(settings.cacheAgeDays);
    common.debugLogging = settings.debugLogging;
    common.passwordListPath = ArchiveAdapterUtf8ToWide(settings.passwordListPath);
    if (!settings.displayTemplate.empty())
        common.titleTemplate = ArchiveAdapterUtf8ToWide(settings.displayTemplate);
    if (!ArchiveAdapterPreferencesSave(common))
    {
        error = "Unable to save shared Archive Stream Player preferences";
        return false;
    }

    FoobarArchiveSettingsApplyBackend(settings);
    return true;
}

void FoobarArchiveSettingsApplyBackend(const FoobarArchiveSettings& settings)
{
    // Cache sizing/age and password settings are shared backend functionality.
    // Buffer sizing and Winamp-style title formatting are stored for the
    // eventual foobar2000 decoder/playlist adapter; no decoder is registered.
    ArchiveCacheSetCleanupSettings(settings.cacheAgeCleanup,
        static_cast<uint32_t>(settings.cacheAgeDays),
        settings.cacheSizeCleanup,
        static_cast<size_t>(settings.cacheSizeMb));

    const std::string passwords = ResolveDataPath(settings.passwordListPath, "keeps.txt");
    ArchivePasswordSetListPath(passwords.c_str());
    RarSetPasswordListPath(passwords.c_str());

    if (ArchiveAdapterPreferencesConfigured())
        ArchiveAdapterLogging::ConfigureShared(L"foobar2000");
    else
    {
        const std::string logPath = ResolveDataPath(settings.debugLogPath, "foo_archive_stream.log");
        DebugLogConfigure(settings.debugLogging, logPath.c_str(), false);
    }
}
