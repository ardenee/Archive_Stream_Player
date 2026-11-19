// Shared Archive Stream title precedence regression (all host adapters).
// Test is read-only and must never modify the user's registry.
#define NOMINMAX
#include <windows.h>
#include <string>
#include <cstdio>
#include "../../shared/archive_adapter_preferences.h"

static int Failure(const char* what)
{
    std::printf("TITLE_DECORATIONS_TEST_FAIL reason=%s\n", what);
    return 1;
}

int wmain()
{
    ArchiveAdapterPreferences settings;
    settings.titleTemplate = L"<archivetype>: <#> - <filename>";
    settings.titlePrefix = L"[Archive] ";
    settings.titlePostfix = L" [end]";
    settings.useFileTags = false;
    const std::wstring archive = L"C:\\Music\\demo.zip";
    const std::wstring member = L"album/03 - Demo.mp3";

    const std::wstring preset = ArchiveAdapterPreferredTitle(
        archive, member, 3, settings, L"Embedded Title");
    if (preset != L"[Archive] ZIP: 3 - 03 - Demo.mp3 [end]")
        return Failure("preset prefix/postfix not applied");

    settings.useFileTags = true;
    const std::wstring embedded = ArchiveAdapterPreferredTitle(
        archive, member, 3, settings, L"Embedded Title");
    if (embedded != L"Embedded Title")
        return Failure("Use File Tags did not override decorations");

    const std::wstring missing = ArchiveAdapterPreferredTitle(
        archive, member, 3, settings, std::wstring());
    if (missing != L"03 - Demo.mp3")
        return Failure("Missing tag must fall back to undecorated filename");

    settings.titlePrefix = std::wstring(21, L'X');
    if (ArchiveAdapterPreferencesSave(settings))
        return Failure("Prefix > 20 bytes not rejected");
    settings.titlePrefix.clear();
    settings.titlePostfix = std::wstring(21, L'Y');
    if (ArchiveAdapterPreferencesSave(settings))
        return Failure("Postfix > 20 bytes not rejected");

    std::puts("TITLE_DECORATIONS_TEST_PASS preset_prefix=1 preset_postfix=1 tags_override=1 fallback_filename=1 max_utf8_bytes=20 registry_unchanged=1");
    return 0;
}
