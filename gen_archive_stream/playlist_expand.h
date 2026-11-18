#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct ArchivePreparedPlaylistItem
{
    std::string path;
    std::string title;
    std::string tagTitle;
    std::string tagArtist;
    std::string tagAlbum;
    std::string tagAlbumArtist;
    std::string tagGenre;
    std::string tagYear;
    unsigned tagTrackNumber = 0;
    int lengthMs = -1000;
    bool useMeta = false;
};

// Runs exclusively on a background worker. Reads tags/durations before any
// WACUP playlist entry is inserted or started.
bool PrepareArchivePlaylistForWacup(const char* archivePath, std::vector<ArchivePreparedPlaylistItem>& outItems, volatile LONG* generation = nullptr, LONG expectedGeneration = 0);
bool ExpandPreparedArchiveAtIndex(HWND hwndWinamp, const char* archivePath, int preferredIndex, const std::vector<ArchivePreparedPlaylistItem>& preparedItems, bool startPlayback);

bool ExpandSelectedOrCurrentZip(HWND hwndWinamp);
bool ExpandZipPathInPlaylist(HWND hwndWinamp, const char* zipPath, bool startPlayback);
bool ExpandZipPathInPlaylistAtIndex(HWND hwndWinamp, const char* zipPath, int preferredIndex, bool startPlayback);

