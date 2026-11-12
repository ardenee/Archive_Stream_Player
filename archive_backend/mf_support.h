#pragma once

#include <windows.h>

// Decoder startup never shows UI; hosts decide how to report failures.
bool MfEnsureStarted();
HRESULT MfGetStartupResult();
void MfShutdownIfStarted();