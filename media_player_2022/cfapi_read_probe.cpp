// media_player_2022/cfapi_read_probe.cpp
// Bounded independent read test. Caller may terminate this process on timeout.
#include <windows.h>
#include <cstdio>
#include <cstdint>
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 1;
    const ULONGLONG begin = GetTickCount64();
    HANDLE file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        wprintf(L"READ_OPEN_FAIL error=%lu elapsed_ms=%llu\n", GetLastError(), GetTickCount64() - begin);
        return 2;
    }
    if (argc >= 3)
    {
        LARGE_INTEGER offset{};
        offset.QuadPart = _wcstoi64(argv[2], nullptr, 10);
        if (offset.QuadPart < 0 || !SetFilePointerEx(file, offset, nullptr, FILE_BEGIN))
        {
            CloseHandle(file);
            return 3;
        }
    }
    unsigned char data[4096]{};
    DWORD got=0;
    const BOOL ok = ReadFile(file, data, sizeof(data), &got, nullptr);
    const DWORD error=ok ? 0 : GetLastError();
    CloseHandle(file);
    wprintf(L"READ_RESULT ok=%d bytes=%lu elapsed_ms=%llu error=%lu\n", ok ? 1 : 0, got, GetTickCount64() - begin, error);
    return ok && got == sizeof(data) ? 0 : 4;
}
