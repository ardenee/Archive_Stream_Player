#pragma once

#include <string>
#include <vector>

// Build native foobar2000 unpack:// URLs for supported audio entries in the
// selected local archive. No extraction, CRC recalculation or codec decoding.
bool FoobarBuildArchivePlaylistUrls(const char* archivePathUtf8, std::vector<std::string>& urls, std::string& error);
#ifdef ARCHIVE_STREAM_FOOBAR_FAULT_TEST
bool FoobarRunNativeRecoveryFaultSelftest(std::string& report);
#endif
