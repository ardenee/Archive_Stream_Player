#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Send an archive-expansion request to the Winamp general-purpose plugin.
// Preserves the existing index|path WM_COPYDATA protocol.
bool WinampRequestArchiveExpansion(HWND hwndWinamp, const char* archivePath);
