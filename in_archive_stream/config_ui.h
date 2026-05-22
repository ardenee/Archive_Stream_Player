#pragma once

#include <windows.h>
#include <cstddef>

void Config(HWND hwndParent);
void About(HWND hwndParent);
void LoadConfig();
void SaveConfig();
void ReallocateAudioBuffer(size_t newSizeMb);