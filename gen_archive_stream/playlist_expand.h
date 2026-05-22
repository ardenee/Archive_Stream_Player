#pragma once

#include <windows.h>

bool ExpandSelectedOrCurrentZip(HWND hwndWinamp);
bool ExpandZipPathInPlaylist(HWND hwndWinamp, const char* zipPath, bool startPlayback);
bool ExpandZipPathInPlaylistAtIndex(HWND hwndWinamp, const char* zipPath, int preferredIndex, bool startPlayback);

