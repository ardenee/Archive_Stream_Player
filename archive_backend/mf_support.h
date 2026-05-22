#pragma once

#include <windows.h>

bool MfEnsureStarted(HWND hwndParent);
void MfShutdownIfStarted();
void MfShowUnavailableMessage(HWND hwndParent);