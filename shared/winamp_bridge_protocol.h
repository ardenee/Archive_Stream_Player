#pragma once

// Winamp-only protocol shared by the input and general-purpose adapters.
// Archive backends must not depend on these messages or window identities.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef WM_WA_MPEG_EOF
#define WM_WA_MPEG_EOF (WM_USER + 2)
#endif

static constexpr char ARCHIVE_STREAM_HIDDEN_CLASS[] = "ArchiveStreamExpanderHiddenWindow";
static constexpr ULONG_PTR ARCHIVE_STREAM_COPYDATA_EXPAND_ZIP = 0x41535A50; // "ASZP"
