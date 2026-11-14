// Archive Stream local playback bridge: loopback-only, statically linked backend.
// Player clients: VLC, Windows Media Player, MusicBee; MediaMonkey may use Winamp API.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "archive_backend.h"
#include "archive_common.h"
#include "archive_format.h"
#include "archive_passwords.h"
#include "archive_stream_owner.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <wincred.h>
#include "archive_adapter_preferences.h"
#include "../shared/archive_adapter_logging.h"
#include "../shared/archive_unified_cache.h"
#include "../shared/archive_unified_warmer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace
{
struct Settings
{
    std::wstring archive, playlistDir;
    std::string passwordPath;
    size_t memoryLimit = 50ULL * 1024 * 1024;
    uint64_t cacheLimit = 2048ULL * 1024 * 1024;
    bool promptPasswords = true;
    bool askToSavePassword = true;
    std::wstring titleTemplate = L"<filename>";
    bool titleOverride = false;
    unsigned short port = 0;
    unsigned lifetime = 0;
#ifdef ARCHIVE_STREAM_BRIDGE_FAULT_TEST
    // Test binaries only. Never enabled in normal release packages.
    uint64_t injectStreamFailureAfterBytes = 0;
#endif
} g_settings;
std::vector<ArchiveEntry> g_entries;
std::string g_secret;
std::wstring g_work, g_output;
unsigned short g_port = 0;
std::atomic<bool> g_quit{false};
std::atomic<unsigned> g_clients{0}, g_stream{0}, g_memory{0}, g_cacheCount{0};
std::mutex g_fileMutex;
std::mutex g_decryptMutex;
std::unordered_map<uint32_t, std::wstring> g_fileCache;
std::unordered_map<uint32_t, uint32_t> g_audioDurationMs;

std::string Utf8(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string result((size_t)n, '\0');
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.c_str(), (int)w.size(), result.data(), n, nullptr, nullptr) == n
               ? result
               : std::string();
}
std::wstring Wide(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring result((size_t)n, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), result.data(), n) == n ? result
                                                                                                              : std::wstring();
}
std::string Extension(const ArchiveEntry &e)
{
    auto slash = e.path.find_last_of("/\\");
    auto dot = e.path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "bin";
    std::string value = e.path.substr(dot + 1);
    if (value.empty() || value.size() > 10)
        return "bin";
    for (char &c : value)
    {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9'))
            return "bin";
    }
    return value;
}
std::string Type(const ArchiveEntry &e)
{
    auto x = Extension(e);
    if (x == "mp3")
        return "audio/mpeg";
    if (x == "flac")
        return "audio/flac";
    if (x == "wav")
        return "audio/wav";
    if (x == "ogg" || x == "opus")
        return "audio/ogg";
    if (x == "aac" || x == "m4a")
        return "audio/mp4";
    if (x == "wma")
        return "audio/x-ms-wma";
    return "application/octet-stream";
}
std::string Xml(const std::string &s)
{
    std::string o;
    for (unsigned char c : s)
    {
        if (c == '&')
            o += "&amp;";
        else if (c == '<')
            o += "&lt;";
        else if (c == '>')
            o += "&gt;";
        else if (c == '"')
            o += "&quot;";
        else if (c == '\'')
            o += "&apos;";
        else if (c >= 32 || c == 9 || c == 10)
            o += char(c);
    }
    return o;
}
std::string Name(const ArchiveEntry &e)
{
    auto slash = e.path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
    // A filename with line breaks must not inject new M3U playlist records.
    for (char &c : name)
    {
        if (c == '\r' || c == '\n' || c == '\0')
            c = ' ';
    }
    return name;
}
std::string Title(const ArchiveEntry& e, const std::wstring& titleTemplate)
{
    const auto value = ArchiveAdapterFormatTitle(g_settings.archive, Wide(e.path), e.index + 1, titleTemplate);
    return Utf8(value);
}
uint64_t OggLittleEndian(const unsigned char* source, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value |= (uint64_t)source[i] << (8 * i);
    return value;
}
bool OggReadExactly(ArchiveStreamOwner& reader, unsigned char* output, size_t length)
{
    size_t read = 0;
    while (read < length)
    {
        auto result = reader.ReadDetailed(output + read, length - read);
        if (result.bytesRead == 0 || result.errorKind != ArchiveStreamErrorKind::None)
            return false;
        read += result.bytesRead;
    }
    return true;
}
uint32_t OggVorbisDurationMs(const ArchiveEntry& entry)
{
    // Read only bounded prefix and suffix windows from a streamable, unencrypted
    // Ogg/Vorbis entry. The shared archive stream backend handles ZIP/7z/RAR;
    // never extract an album or prompt for a password while making playlists.
    constexpr uint64_t maxProbeEntry = 64ULL * 1024 * 1024;
    if (Extension(entry) != "ogg" || entry.isFolder || entry.isEncrypted || entry.isSolid ||
        entry.size < 64 || entry.size > maxProbeEntry)
        return 0;

    ArchiveStreamOwner reader;
    std::string error;
    if (!reader.Open(g_settings.archive.c_str(), entry.index, error))
        return 0;

    std::array<unsigned char, 2048> prefix{};
    const size_t prefixBytes = static_cast<size_t>((std::min)(entry.size, static_cast<uint64_t>(prefix.size())));
    if (!OggReadExactly(reader, prefix.data(), prefixBytes))
        return 0;
    if (std::memcmp(prefix.data(), "OggS", 4) != 0 || prefix[4] != 0 || !(prefix[5] & 0x02))
        return 0;
    const uint32_t serial = static_cast<uint32_t>(OggLittleEndian(prefix.data() + 14, 4));
    const size_t segmentCount = prefix[26];
    const size_t payloadAt = 27 + segmentCount;
    if (payloadAt + 16 > prefixBytes || std::memcmp(prefix.data() + payloadAt, "\x01vorbis", 7) != 0)
        return 0;
    if (OggLittleEndian(prefix.data() + payloadAt + 7, 4) != 0)
        return 0; // Unsupported Vorbis stream header version
    const uint32_t sampleRate = static_cast<uint32_t>(OggLittleEndian(prefix.data() + payloadAt + 12, 4));
    if (sampleRate < 8000 || sampleRate > 384000 || prefix[payloadAt + 11] == 0)
        return 0;

    constexpr uint64_t tailWindow = 65536;
    const size_t tailBytes = static_cast<size_t>((std::min)(entry.size, tailWindow));
    if (!reader.Seek(entry.size - tailBytes))
        return 0;
    std::vector<unsigned char> tail(tailBytes);
    if (!OggReadExactly(reader, tail.data(), tailBytes))
        return 0;
    // A complete final EOS page is at most 65307 bytes. Require its serial
    // to match the first Vorbis page: chained/multiplexed streams are not
    // assigned an incorrect partial-chain duration.
    for (size_t at = 0; at + 27 <= tail.size(); ++at)
    {
        if (std::memcmp(tail.data() + at, "OggS", 4) != 0 || tail[at + 4] != 0 || !(tail[at + 5] & 0x04))
            continue;
        const size_t segments = tail[at + 26];
        if (at + 27 + segments > tail.size())
            continue;
        size_t payload = 0;
        for (size_t i = 0; i < segments; ++i)
            payload += tail[at + 27 + i];
        if (at + 27 + segments + payload != tail.size())
            continue;
        if (OggLittleEndian(tail.data() + at + 14, 4) != serial)
            continue;
        const uint64_t granule = OggLittleEndian(tail.data() + at + 6, 8);
        if (granule == 0 || granule > static_cast<uint64_t>(sampleRate) * 86400)
            continue;
        const uint64_t milliseconds = (granule * 1000 + sampleRate / 2) / sampleRate;
        return static_cast<uint32_t>(milliseconds);
    }
    return 0;
}
std::string Link(const ArchiveEntry &e)
{
    return "http://127.0.0.1:" + std::to_string(g_port) + "/" + g_secret + "/entry/" + std::to_string(e.index) + "." + Extension(e);
}
std::string Playlist(bool asx)
{
    // The bridge may outlive the settings window. An HTTP playlist re-fetch
    // always uses the latest shared title template unless CLI explicitly set it.
    const auto titleTemplate = g_settings.titleOverride ? g_settings.titleTemplate : ArchiveAdapterPreferencesLoad().titleTemplate;
    std::string text = asx ? "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<asx version=\"3.0\">\r\n" : "#EXTM3U\r\n";
    for (auto &e : g_entries)
    {
        if (e.isFolder || !e.isSupportedAudio)
            continue;
        const std::string title = Title(e, titleTemplate);
        const auto duration = g_audioDurationMs.find(e.index);
        const int seconds = duration == g_audioDurationMs.end() ? -1 : (std::max)(1, static_cast<int>((duration->second + 500) / 1000));
        text += asx ? "<entry><title>" + Xml(title) + "</title><ref href=\"" + Link(e) + "\"/></entry>\r\n"
                    : "#EXTINF:" + std::to_string(seconds) + "," + title + "\r\n" + Link(e) + "\r\n";
    }
    if (asx)
        text += "</asx>\r\n";
    return text;
}
bool Send(SOCKET s, const void *buffer, size_t bytes)
{
    const char *p = (const char *)buffer;
    while (bytes)
    {
        int n = send(s, p, (int)(std::min)(bytes, (size_t)65536), 0);
        if (n <= 0)
            return false;
        p += n;
        bytes -= (size_t)n;
    }
    return true;
}
bool Header(SOCKET s, int status, const std::string &mime, uint64_t length, const std::string &extra = {})
{
    const char *msg = status == 200   ? "OK"
                      : status == 206 ? "Partial Content"
                      : status == 403 ? "Forbidden"
                      : status == 404 ? "Not Found"
                      : status == 405 ? "Method Not Allowed"
                      : status == 416 ? "Range Not Satisfiable"
                                      : "Internal Server Error";
    std::string text = "HTTP/1.1 " + std::to_string(status) + " " + msg + "\r\nContent-Type: " + mime +
                       "\r\nContent-Length: " + std::to_string(length) +
                       "\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\nConnection: close\r\n" +
                       extra + "\r\n";
    return Send(s, text.data(), text.size());
}
bool ParseNumber(std::string_view s, uint64_t &v)
{
    if (s.empty())
        return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
bool Range(const std::string &request, uint64_t total, uint64_t &start, uint64_t &end, bool &partial)
{
    start = 0;
    end = total ? total - 1 : 0;
    partial = false;
    size_t pos = request.find("\r\nRange:");
    if (pos == std::string::npos)
        pos = request.find("\r\nrange:");
    if (pos == std::string::npos)
        return true;
    size_t begin = pos + 8, stop = request.find("\r\n", begin);
    if (stop == std::string::npos)
        return false;
    std::string_view line(request.data() + begin, stop - begin);
    while (!line.empty() && line.front() == ' ')
        line.remove_prefix(1);
    if (line.substr(0, 6) != "bytes=")
        return false;
    line.remove_prefix(6);
    if (line.find(',') != std::string::npos)
        return false;
    size_t dash = line.find('-');
    if (dash == std::string::npos)
        return false;
    partial = true;
    if (dash == 0)
    {
        uint64_t suffix;
        if (!ParseNumber(line.substr(1), suffix) || suffix == 0 || total == 0)
            return false;
        start = suffix >= total ? 0 : total - suffix;
        end = total - 1;
    }
    else
    {
        if (!ParseNumber(line.substr(0, dash), start) || total == 0 || start >= total)
            return false;
        if (dash + 1 < line.size())
        {
            if (!ParseNumber(line.substr(dash + 1), end) || end < start)
                return false;
            end = (std::min)(end, total - 1);
        }
        else
            end = total - 1;
    }
    return true;
}

std::wstring Cache(const ArchiveEntry &e)
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (auto it = g_fileCache.find(e.index); it != g_fileCache.end())
        return it->second;

    // Command-line limits remain authoritative for bridge test/session overrides.
    if (g_settings.cacheLimit == 0 || e.size > g_settings.cacheLimit)
    {
        std::fprintf(stderr, "CACHE_LIMIT index=%u\r\n", e.index);
        DebugLogWrite("WARN", "BRIDGE", "CACHE_LIMIT", "index=%u | limit=%llu | entry=%llu", e.index, (unsigned long long)g_settings.cacheLimit, (unsigned long long)e.size);
        return {};
    }
    auto options = ArchiveAdapterPreferencesLoad();
    options.cacheLimitMb = static_cast<unsigned>(g_settings.cacheLimit / ArchiveUnifiedCache::kMb);
    std::wstring path;
    std::string error;
    if (!ArchiveUnifiedCache::Prepare(g_settings.archive, e, options, path, error))
    {
        std::fprintf(stderr, "CACHE_FAIL entry=%u reason=%s\r\n", e.index, error.c_str());
        DebugLogWrite("WARN", "BRIDGE", "CACHE_FAIL", "index=%u | error=%s", e.index, error.c_str());
        return {};
    }
    g_fileCache[e.index] = path;
    DebugLogWrite("INFO", "BRIDGE", "CACHE_READY", "index=%u | bytes=%llu", e.index, (unsigned long long)e.size);
    ArchiveUnifiedWarmer::Queue(g_settings.archive, e, options);
    return path;
}
// The HTTP status and Content-Length have already been sent. Recover only
// remaining audio bytes; emitting another HTTP header would corrupt playback.
bool SendRecoveredAudio(SOCKET s, const ArchiveEntry& e, uint64_t resumeAt, uint64_t remaining, std::unique_lock<std::mutex>& passwordLock)
{
    if (!remaining)
        return true;

    if (!passwordLock.owns_lock())
        passwordLock.lock();

    std::string error;
    if (e.size <= g_settings.memoryLimit && e.size <= SIZE_MAX)
    {
        std::vector<unsigned char> bytes;
        if (ArchiveExtractEntryToMemory(g_settings.archive.c_str(), e.index, bytes, error) && bytes.size() == e.size)
        {
            ++g_memory;
            std::fprintf(stderr, "STREAM_RECOVERY route=memory entry=%u offset=%llu remaining=%llu\r\n",
                         e.index, (unsigned long long)resumeAt, (unsigned long long)remaining);
            passwordLock.unlock();
            return Send(s, bytes.data() + static_cast<size_t>(resumeAt), static_cast<size_t>(remaining));
        }
    }

    const std::wstring path = Cache(e);
    if (path.empty())
        return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(resumeAt);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
    {
        CloseHandle(file);
        return false;
    }

    ++g_cacheCount;
    std::fprintf(stderr, "STREAM_RECOVERY route=cache entry=%u offset=%llu remaining=%llu\r\n",
                 e.index, (unsigned long long)resumeAt, (unsigned long long)remaining);
    passwordLock.unlock();

    bool ok = true;
    std::array<char, 65536> buffer{};
    while (ok && remaining)
    {
        const DWORD want = static_cast<DWORD>((std::min)(remaining, (uint64_t)buffer.size()));
        DWORD got = 0;
        if (!ReadFile(file, buffer.data(), want, &got, nullptr) || got == 0)
            ok = false;
        else
        {
            ok = Send(s, buffer.data(), got);
            remaining -= got;
        }
    }

    CloseHandle(file);
    return ok && remaining == 0;
}
bool ServeAudio(SOCKET s, const std::string &request, const ArchiveEntry &e, bool head)
{
    uint64_t start = 0, end = 0;
    bool partial = false;
    if (!Range(request, e.size, start, end, partial))
    {
        DebugLogWrite("WARN", "BRIDGE", "RANGE_INVALID", "index=%u | entry_size=%llu", e.index, (unsigned long long)e.size);
        Header(s, 416, "text/plain", 0, "Content-Range: bytes */" + std::to_string(e.size) + "\r\n");
        return false;
    }
    const uint64_t length = e.size ? end - start + 1 : 0;
    const std::string rangeHeader =
        partial ? "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(e.size) + "\r\n"
                : "";
    const int status = partial ? 206 : 200;
    if (head)
        return Header(s, status, Type(e), length, rangeHeader);
    // Serialize archive opening / password resolution across HTTP requests.
    // Credential UI is allowed on the interactive worker thread, not during
    // an unbounded archive-wide startup scan.
    std::unique_lock<std::mutex> passwordLock(g_decryptMutex);
    ArchivePasswordSetPromptAllowedForCurrentThread(g_settings.promptPasswords);

    // Probe decoded data before sending success. Solid 7z/RAR cannot stream.
    // A failed direct stream falls through to RAM, then temporary disk cache.
    ArchiveStreamOwner stream;
    std::string error;
    bool streamReady = false;
    ArchiveFormat format = DetectArchiveFormatFromPath(g_settings.archive.c_str());
    if (!(e.isSolid && (format == ArchiveFormat::SevenZip || format == ArchiveFormat::Rar)))
    {
        if (stream.Open(g_settings.archive.c_str(), e.index, error))
        {
            std::array<char, 4096> sample{};
            const size_t want = (size_t)(std::min)(e.size, (uint64_t)sample.size());
            const size_t got = want ? stream.Read(sample.data(), want) : 0;
            streamReady = got == want && stream.Seek(start);
            if (!streamReady)
                stream.Close();
        }
    }
    if (streamReady)
    {
        ++g_stream;
        DebugLogWrite("INFO", "BRIDGE", "ROUTE_STREAM", "index=%u | start=%llu | bytes=%llu", e.index, (unsigned long long)start, (unsigned long long)length);
        passwordLock.unlock();
        if (!Header(s, status, Type(e), length, rangeHeader))
            return false;
        uint64_t left = length;
        std::array<char, 65536> buffer{};
        while (left)
        {
            const size_t want = (size_t)(std::min)(left, (uint64_t)buffer.size());
            ArchiveStreamReadResult result;
#ifdef ARCHIVE_STREAM_BRIDGE_FAULT_TEST
            // Inject a backend failure into the real production error branch.
            if (g_settings.injectStreamFailureAfterBytes != 0 &&
                length - left >= g_settings.injectStreamFailureAfterBytes)
            {
                result.errorKind = ArchiveStreamErrorKind::BackendFailure;
                result.message = "Test-injected decoder failure after partial response";
            }
            else
#endif
            {
                result = stream.ReadDetailed(buffer.data(), want);
            }
            if (result.errorKind != ArchiveStreamErrorKind::None || result.bytesRead == 0)
            {
                // Never duplicate partial bytes from a failed read. Resume at
                // the last successfully transmitted offset via RAM or cache.
                if (result.errorKind != ArchiveStreamErrorKind::BackendFailure &&
                    result.errorKind != ArchiveStreamErrorKind::TruncatedEntry)
                    return false;

                stream.Close();
                return SendRecoveredAudio(s, e, start + (length - left), left, passwordLock);
            }
            if (!Send(s, buffer.data(), result.bytesRead))
                return false;
            left -= result.bytesRead;
        }
        return true;
    }
    if (e.size <= g_settings.memoryLimit && e.size <= SIZE_MAX)
    {
        std::vector<unsigned char> bytes;
        if (ArchiveExtractEntryToMemory(g_settings.archive.c_str(), e.index, bytes, error) && bytes.size() == e.size)
        {
            ++g_memory;
            DebugLogWrite("INFO", "BRIDGE", "ROUTE_MEMORY", "index=%u | start=%llu | bytes=%llu", e.index, (unsigned long long)start, (unsigned long long)length);
            passwordLock.unlock();
            if (!Header(s, status, Type(e), length, rangeHeader))
                return false;
            return length == 0 || Send(s, bytes.data() + (size_t)start, (size_t)length);
        }
    }
    std::wstring filePath = Cache(e);
    if (filePath.empty())
    {
        Header(s, 500, "text/plain", 0);
        return false;
    }
    HANDLE file =
        CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        Header(s, 500, "text/plain", 0);
        return false;
    }
    LARGE_INTEGER offset{};
    offset.QuadPart = (LONGLONG)start;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN))
    {
        CloseHandle(file);
        Header(s, 500, "text/plain", 0);
        return false;
    }
    ++g_cacheCount;
    DebugLogWrite("INFO", "BRIDGE", "ROUTE_CACHE", "index=%u | start=%llu | bytes=%llu", e.index, (unsigned long long)start, (unsigned long long)length);
    passwordLock.unlock();
    bool ok = Header(s, status, Type(e), length, rangeHeader);
    uint64_t left = length;
    std::array<char, 65536> buffer{};
    while (ok && left)
    {
        DWORD got = 0;
        const DWORD want = (DWORD)(std::min)(left, (uint64_t)buffer.size());
        if (!ReadFile(file, buffer.data(), want, &got, nullptr) || got == 0)
            ok = false;
        else
        {
            ok = Send(s, buffer.data(), got);
            left -= got;
        }
    }
    CloseHandle(file);
    return ok;
}
void Client(SOCKET s)
{
    struct Guard
    {
        SOCKET s;
        ~Guard()
        {
            shutdown(s, SD_BOTH);
            closesocket(s);
            --g_clients;
        }
    } guard{s};
    DWORD timeoutMs = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeoutMs, sizeof(timeoutMs));
    std::string request;
    std::array<char, 2048> buffer{};
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
    {
        const int got = recv(s, buffer.data(), (int)buffer.size(), 0);
        if (got <= 0)
            return;
        request.append(buffer.data(), (size_t)got);
    }
    const size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos)
        return;
    const std::string line = request.substr(0, lineEnd);
    const bool head = line.rfind("HEAD ", 0) == 0;
    const bool get = line.rfind("GET ", 0) == 0;
    const bool post = line.rfind("POST ", 0) == 0;
    if (!head && !get && !post)
    {
        Header(s, 405, "text/plain", 0);
        return;
    }
    const size_t start = head ? 5 : (post ? 5 : 4);
    const size_t space = line.find(' ', start);
    if (space == std::string::npos)
    {
        Header(s, 404, "text/plain", 0);
        return;
    }
    const std::string path = line.substr(start, space - start);
    const std::string prefix = "/" + g_secret + "/";
    if (path.rfind(prefix, 0) != 0)
    {
        Header(s, 403, "text/plain", 0);
        return;
    }
    const std::string endpoint = path.substr(prefix.size());
    if (post)
    {
        if (endpoint != "shutdown")
        {
            Header(s, 405, "text/plain", 0);
            return;
        }
        g_quit = true;
        Header(s, 200, "text/plain", 0);
        return;
    }
    if (endpoint == "playlist.m3u8" || endpoint == "playlist.asx")
    {
        const bool asx = endpoint == "playlist.asx";
        const std::string body = Playlist(asx);
        if (Header(s, 200, asx ? "video/x-ms-asf" : "audio/x-mpegurl", body.size()) && !head)
            Send(s, body.data(), body.size());
        return;
    }
    if (endpoint.rfind("entry/", 0) != 0)
    {
        Header(s, 404, "text/plain", 0);
        return;
    }
    const std::string id = endpoint.substr(6);
    const size_t dot = id.find('.');
    if (dot == std::string::npos)
    {
        Header(s, 404, "text/plain", 0);
        return;
    }
    uint32_t index = 0;
    auto parsed = std::from_chars(id.data(), id.data() + dot, index);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + dot)
    {
        Header(s, 404, "text/plain", 0);
        return;
    }
    const auto entry = std::find_if(g_entries.begin(), g_entries.end(), [index](const ArchiveEntry &e) { return e.index == index; });
    if (entry == g_entries.end() || entry->isFolder || !entry->isSupportedAudio || id.substr(dot + 1) != Extension(*entry))
    {
        Header(s, 404, "text/plain", 0);
        return;
    }
    ServeAudio(s, request, *entry, head);
}
BOOL WINAPI OnCtrl(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        g_quit = true;
        return TRUE;
    }
    return FALSE;
}
bool Arguments(int argc, wchar_t **argv)
{
    for (int i = 1; i < argc; i++)
    {
        std::wstring key = argv[i];
        if (key == L"--help" || i + 1 >= argc)
            return false;
        std::wstring val = argv[++i];
        try
        {
            if (key == L"--archive")
                g_settings.archive = val;
            else if (key == L"--playlist-dir")
                g_settings.playlistDir = val;
            else if (key == L"--password-list")
                g_settings.passwordPath = Utf8(val);
            else if (key == L"--prompt-password")
            {
                if (val != L"0" && val != L"1")
                    return false;
                g_settings.promptPasswords = val == L"1";
            }
            else if (key == L"--title-template")
            {
                if (val != L"<filename>" && val != L"<archivetype>: <filename>" &&
                    val != L"<#>. <filename>" && val != L"<filename> [<filetype>]" &&
                    val != L"<archivetype>: <#> - <filename>" &&
                    val != L"[<archive>] <#> - <filename>")
                    return false;
                g_settings.titleTemplate = val;
                g_settings.titleOverride = true;
            }
            else if (key == L"--cache-limit-mb")
            {
                const auto limit = std::stoull(val);
                if (limit > 20480)
                    return false;
                g_settings.cacheLimit = limit * 1024 * 1024;
            }
            else if (key == L"--port")
            {
                unsigned long p = std::stoul(val);
                if (p > 65535)
                    return false;
                g_settings.port = (unsigned short)p;
            }
            else if (key == L"--memory-mb")
            {
                unsigned long long m = std::stoull(val);
                if (m > 512)
                    return false;
                g_settings.memoryLimit = (size_t)m * 1024 * 1024;
            }
            else if (key == L"--lifetime")
            {
                unsigned long t = std::stoul(val);
                if (t > 86400)
                    return false;
                g_settings.lifetime = t;
            }
#ifdef ARCHIVE_STREAM_BRIDGE_FAULT_TEST
            else if (key == L"--test-stream-failure-after")
            {
                const unsigned long long count = std::stoull(val);
                if (count == 0 || count > 1048576)
                    return false;
                g_settings.injectStreamFailureAfterBytes = count;
            }
#endif
            else
                return false;
        }
        catch (...)
        {
            return false;
        }
    }
    return !g_settings.archive.empty();
}
bool PromptPassword(const char* archivePath, const char*, char* output, size_t capacity, bool* save)
{
    if (!output || capacity == 0)
        return false;
    output[0] = '\0';
    if (save)
        *save = false;
    CREDUI_INFOW ui{};
    ui.cbSize = sizeof(ui);
    ui.pszCaptionText = L"Archive Stream Player - Password Required";
    ui.pszMessageText = L"Enter the archive password. Your password is not saved by Windows.";
    const std::wstring target = Wide(archivePath ? archivePath : "");
    wchar_t username[CREDUI_MAX_USERNAME_LENGTH + 1]{};
    wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
    BOOL persisted = FALSE;
    const DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST;
    const DWORD response = CredUIPromptForCredentialsW(&ui, target.empty() ? L"Archive Stream" : target.c_str(),
        nullptr, 0, username, CREDUI_MAX_USERNAME_LENGTH + 1,
        password, CREDUI_MAX_PASSWORD_LENGTH + 1, &persisted, flags);
    if (response != NO_ERROR)
    {
        SecureZeroMemory(password, sizeof(password));
        return false;
    }
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, nullptr, 0, nullptr, nullptr);
    const bool valid = needed > 1 && static_cast<size_t>(needed) <= capacity &&
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, output, static_cast<int>(capacity), nullptr, nullptr) == needed;
    SecureZeroMemory(password, sizeof(password));
    if (!valid)
        output[0] = '\0';
    if (valid && save && g_settings.askToSavePassword)
        *save = MessageBoxW(nullptr,
            L"Save this password in the configured plaintext password list after verification?",
            L"Archive Stream Player", MB_YESNO | MB_ICONQUESTION) == IDYES;
    return valid;
}

bool WriteText(const std::filesystem::path &p, const std::string &text)
{
    std::ofstream output(p, std::ios::binary | std::ios::trunc);
    output.write(text.data(), (std::streamsize)text.size());
    return output.good();
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    const auto preferences = ArchiveAdapterPreferencesLoad();
    g_settings.memoryLimit = static_cast<size_t>(preferences.memoryMb) * 1024 * 1024;
    g_settings.cacheLimit = static_cast<uint64_t>(preferences.cacheLimitMb) * 1024 * 1024;
    g_settings.promptPasswords = preferences.promptPasswords;
    g_settings.askToSavePassword = preferences.askToSavePassword;
    g_settings.titleTemplate = preferences.titleTemplate;
    if (!preferences.passwordListPath.empty())
        g_settings.passwordPath = Utf8(preferences.passwordListPath);
    if (!Arguments(argc, argv))
    {
        std::fputs("Usage: archive_player_bridge.exe --archive FILE.zip|7z|rar [--password-list PATH] [--memory-mb 50] "
                   "[--playlist-dir DIR] [--lifetime SECONDS] [--port PORT] [--cache-limit-mb N] "
                   "[--title-template TEMPLATE] [--prompt-password 0|1]\r\n",
                   stderr);
        return 2;
    }
    ArchiveSetDataDirectoryName("ArchiveBridgeStream");
    char debugSwitch[8]{};
    const bool debugOverride = GetEnvironmentVariableA("ARCHIVE_STREAM_ADAPTER_DEBUG", debugSwitch, sizeof(debugSwitch)) > 0 && debugSwitch[0] == '1';
    ArchiveAdapterLogging::Configure(L"bridge", preferences.debugLogging || debugOverride);
    if (!ArchiveUnifiedCache::Maintain(preferences))
        DebugLogWrite("WARN", "BRIDGE", "CACHE_MAINTENANCE_PARTIAL", "reason=cache_file_in_use");
    DebugLogWrite("INFO", "BRIDGE", "SESSION_OPEN", "entries_pending=1 | cache_limit=%llu | memory_limit=%zu", (unsigned long long)g_settings.cacheLimit, g_settings.memoryLimit);
    if (g_settings.passwordPath.empty())
    {
        wchar_t local[MAX_PATH * 4]{};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, (DWORD)std::size(local));
        if (len && len < std::size(local))
            g_settings.passwordPath = Utf8(std::wstring(local) + L"\\WinampArchiveStream\\keeps.txt");
    }
    if (!g_settings.passwordPath.empty())
        ArchivePasswordSetListPath(g_settings.passwordPath.c_str());
    ArchivePasswordSetPromptCallback(g_settings.promptPasswords ? PromptPassword : nullptr);
    ArchivePasswordSetPromptAllowedForCurrentThread(g_settings.promptPasswords);
    std::string error;
    if (!ArchiveListEntries(g_settings.archive.c_str(), g_entries, error))
    {
        std::fprintf(stderr, "ARCHIVE_OPEN_FAIL: %s\r\n", error.c_str());
        return 3;
    }
    size_t audio = 0;
    for (const auto &e : g_entries)
        if (e.isSupportedAudio && !e.isFolder)
            ++audio;
    if (!audio)
    {
        std::fputs("ARCHIVE_NO_AUDIO\r\n", stderr);
        return 4;
    }
    // Encrypted entry credentials are resolved only when playback requests
    // decoded data. Serialized HTTP worker opens avoid duplicate prompts.
    ArchivePasswordSetPromptAllowedForCurrentThread(false);
    for (const auto& entry : g_entries)
    {
        const uint32_t duration = OggVorbisDurationMs(entry);
        if (duration)
        {
            g_audioDurationMs.emplace(entry.index, duration);
            DebugLogWrite("INFO", "BRIDGE", "OGG_DURATION", "index=%u | milliseconds=%u", entry.index, duration);
        }
    }
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), (ULONG)random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return 5;
    static constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : random)
    {
        g_secret += digits[c >> 4];
        g_secret += digits[c & 15];
    }
    SecureZeroMemory(random.data(), random.size());
    wchar_t temp[MAX_PATH * 4]{};
    if (!GetTempPathW((DWORD)std::size(temp), temp))
        return 6;
    g_work = std::wstring(temp) + L"ArchiveBridge-" + Wide(g_secret);
    if (!CreateDirectoryW(g_work.c_str(), nullptr))
        return 6;
    g_output = g_settings.playlistDir.empty() ? g_work : g_settings.playlistDir;
    std::error_code ec;
    std::filesystem::create_directories(g_output, ec);
    if (ec)
    {
        RemoveDirectoryW(g_work.c_str());
        return 6;
    }

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data))
        return 7;
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET)
    {
        WSACleanup();
        return 7;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(g_settings.port);
    if (bind(server, (sockaddr *)&addr, sizeof(addr)) || listen(server, SOMAXCONN))
    {
        std::fprintf(stderr, "BIND_FAIL code=%d\r\n", WSAGetLastError());
        closesocket(server);
        WSACleanup();
        return 8;
    }
    int size = sizeof(addr);
    if (getsockname(server, (sockaddr *)&addr, &size))
        return 8;
    g_port = ntohs(addr.sin_port);
    const std::wstring m3u = g_output + L"\\archive-stream.m3u8";
    const std::wstring asx = g_output + L"\\archive-stream.asx";
    if (!WriteText(m3u, Playlist(false)) || !WriteText(asx, Playlist(true)))
    {
        std::fputs("PLAYLIST_WRITE_FAIL\r\n", stderr);
        closesocket(server);
        WSACleanup();
        return 9;
    }
    std::printf("ARCHIVE_AUDIO_ENTRIES=%zu\r\n", audio);
    std::printf("PLAYLIST_M3U8=%s\r\n", Utf8(m3u).c_str());
    std::printf("PLAYLIST_ASX=%s\r\n", Utf8(asx).c_str());
    std::printf("URL_M3U8=http://127.0.0.1:%hu/%s/playlist.m3u8\r\n", g_port, g_secret.c_str());
    std::printf("URL_ASX=http://127.0.0.1:%hu/%s/playlist.asx\r\n", g_port, g_secret.c_str());
    std::printf("BRIDGE_READY=1\r\n");
    std::fflush(stdout);
    SetConsoleCtrlHandler(OnCtrl, TRUE);
    const auto began = std::chrono::steady_clock::now();
    auto lastTitleTemplate = g_settings.titleTemplate;
    auto lastTitleCheck = began;

    while (!g_quit)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!g_settings.titleOverride && now - lastTitleCheck >= std::chrono::seconds(1))
        {
            lastTitleCheck = now;
            const auto current = ArchiveAdapterPreferencesLoad().titleTemplate;
            if (current != lastTitleTemplate)
            {
                // Replace playlist files atomically; clients already using the
                // old playlist keep their current URLs until they reload it.
                const auto refresh = [&](const std::wstring& file, bool asxFormat)
                {
                    const auto temporary = file + L".partial";
                    if (!WriteText(temporary, Playlist(asxFormat)) ||
                        !MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    {
                        DeleteFileW(temporary.c_str());
                        return false;
                    }
                    return true;
                };
                if (refresh(m3u, false) && refresh(asx, true))
                {
                    lastTitleTemplate = current;
                    DebugLogWrite("INFO", "BRIDGE", "PLAYLIST_TITLES_REFRESHED", "tracks=%zu", audio);
                }
                else
                    DebugLogWrite("WARN", "BRIDGE", "PLAYLIST_TITLES_REFRESH_FAIL", "winerr=%lu", GetLastError());
            }
        }
        if (g_settings.lifetime && std::chrono::steady_clock::now() - began >= std::chrono::seconds(g_settings.lifetime))
            break;
        fd_set ready{};
        FD_ZERO(&ready);
        FD_SET(server, &ready);
        timeval timeout{};
        timeout.tv_usec = 250000;
        if (select(0, &ready, nullptr, nullptr, &timeout) <= 0)
            continue;
        SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        if (g_clients.fetch_add(1) >= 16)
        {
            --g_clients;
            closesocket(client);
            continue;
        }
        try
        {
            std::thread(
                [client]()
                {
                    // Client owns socket closing and client-count decrement via
                    // its RAII guard. Catching an exception here must not double
                    // close the socket or decrement the client count again.
                    try
                    {
                        Client(client);
                    }
                    catch (...)
                    {
                        std::fputs("CLIENT_HANDLER_EXCEPTION\r\n", stderr);
                    }
                })
                .detach();
        }
        catch (...)
        {
            closesocket(client);
            --g_clients;
        }
    }
    closesocket(server);
    while (g_clients.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("ROUTE_COUNTS stream=%u memory=%u cache=%u\r\n", g_stream.load(), g_memory.load(), g_cacheCount.load());
    if (g_output == g_work)
    {
        DeleteFileW(m3u.c_str());
        DeleteFileW(asx.c_str());
    }
    for (const auto &[index, path] : g_fileCache)
        DeleteFileW(path.c_str());
    RemoveDirectoryW(g_work.c_str());
    WSACleanup();
    return 0;
}
