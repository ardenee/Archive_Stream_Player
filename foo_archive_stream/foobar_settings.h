#pragma once

#include <string>

// Host-local preferences, persisted by foobar2000's cfg_var mechanism.
// Winamp settings and its existing INI file are never read or modified.
struct FoobarArchiveSettings
{
    int bufferSizeMb = 50;
    int cacheSizeMb = 2048;
    bool cacheSizeCleanup = true;
    bool cacheAgeCleanup = true;
    int cacheAgeDays = 30;
    std::string displayPrefix;
    std::string displayPostfix;
    std::string displayTemplate = "<filename>";
    std::string passwordListPath; // Empty = FoobarArchiveStream/keeps.txt.
    bool debugLogging = false;
    std::string debugLogPath; // Empty = FoobarArchiveStream/foo_archive_stream.log.
};

FoobarArchiveSettings FoobarArchiveSettingsLoad();
FoobarArchiveSettings FoobarArchiveSettingsDefaults();
bool FoobarArchiveSettingsValidate(FoobarArchiveSettings& settings, std::string& error);
bool FoobarArchiveSettingsSave(const FoobarArchiveSettings& settings, std::string& error);
void FoobarArchiveSettingsApplyBackend(const FoobarArchiveSettings& settings);
