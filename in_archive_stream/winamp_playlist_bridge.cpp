#include <windows.h>
#include <cstdio>
#include <cstring>

#include "wa_ipc.h"
#include "winamp_bridge_protocol.h"
#include "winamp_playlist_bridge.h"
#include "debug_log.h"

bool WinampRequestArchiveExpansion(HWND hwndWinamp, const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "reason=empty_path");
        return false;
    }

    HWND hwnd = FindWindowA(ARCHIVE_STREAM_HIDDEN_CLASS, NULL);

    if (!hwnd)
    {
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "archive=%s | reason=hidden_window_not_found", archivePath);
        return false;
    }

    int playlistIndex = -1;

    if (hwndWinamp)
        playlistIndex = (int)SendMessage(hwndWinamp, WM_WA_IPC, 0, IPC_GETLISTPOS);

    char payload[MAX_PATH * 4 + 64];

    if (sprintf_s(payload, sizeof(payload), "%d|%s", playlistIndex, archivePath) < 0)
    {
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "reason=payload_too_long");
        return false;
    }

    DebugLogWrite("INFO", "EXPAND", "REQUEST_SENT", "archive=%s | playlist_index=%d | payload=%s", archivePath, playlistIndex, payload);

    COPYDATASTRUCT cds = {};
    cds.dwData = ARCHIVE_STREAM_COPYDATA_EXPAND_ZIP;
    cds.lpData = payload;
    cds.cbData = (DWORD)strlen(payload) + 1;

    LRESULT result = SendMessageA(hwnd, WM_COPYDATA, 0, (LPARAM)&cds);

    if (result == 0)
        DebugLogWrite("ERROR", "EXPAND", "REQUEST_FAIL", "archive=%s | index=%d | reason=copydata_rejected", archivePath, playlistIndex);

    return result != 0;
}
