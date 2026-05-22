#include "archive_format.h"

#include <cwchar>
#include <string>
#include <algorithm>

static std::wstring ToLowerWide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        if (c >= L'A' && c <= L'Z')
            return static_cast<wchar_t>(c - L'A' + L'a');

        return c;
        });

    return value;
}

static bool EndsWith(const std::wstring& value, const wchar_t* suffix)
{
    if (!suffix)
        return false;

    std::wstring suffixText = suffix;

    if (value.length() < suffixText.length())
        return false;

    return value.compare(value.length() - suffixText.length(), suffixText.length(), suffixText) == 0;
}

ArchiveFormat DetectArchiveFormatFromPath(const wchar_t* archivePath)
{
    if (!archivePath || !*archivePath)
        return ArchiveFormat::Unknown;

    std::wstring lower = ToLowerWide(archivePath);

    /*
        Detect normal and first-volume multipart 7z archives.

        7-Zip split archives commonly use:
            album.7z.001
            album.7z.002
            ...

        Only the first volume should be opened directly. The 7z SDK can then read the
        remaining volumes through its normal volume handling. Treating every .001 file
        as 7z would be too broad, so require the stronger ".7z.001" suffix.
    */
    if (EndsWith(lower, L".7z") || EndsWith(lower, L".7z.001"))

        return ArchiveFormat::SevenZip;

    /*
        Detect normal ZIP archives and first-volume split ZIP archives.

        Split ZIP sets commonly use:
            album.zip.001
            album.zip.002
            ...

        Only the first volume should be treated as the archive input. Do not treat
        arbitrary .001 files as ZIP.
    */
    if (EndsWith(lower, L".zip") || EndsWith(lower, L".zip.001"))

        return ArchiveFormat::Zip;

    if (EndsWith(lower, L".rar"))
        return ArchiveFormat::Rar;

    return ArchiveFormat::Unknown;
}

const char* ArchiveFormatToString(ArchiveFormat format)
{
    switch (format)
    {
    case ArchiveFormat::Zip:
        return "ZIP";

    case ArchiveFormat::SevenZip:
        return "7Z";

    case ArchiveFormat::Rar:
        return "RAR";

    case ArchiveFormat::Unknown:
    default:
        return "Unknown";
    }
}