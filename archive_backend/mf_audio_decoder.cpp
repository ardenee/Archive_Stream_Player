#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmreg.h>
#include <propvarutil.h>
#include <strsafe.h>

#include <vector>
#include <string>
#include <new>
#include <cstring>

#include "mf_audio_decoder.h"
#include "mf_support.h"
#include "mf_archive_bytestream.h"
#include "debug_log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")

struct MfAudioDecoder
{
    IMFSourceReader* reader = nullptr;
    ArchiveMFByteStream* byteStream = nullptr;

    WAVEFORMATEX* waveFormat = nullptr;
    UINT32 waveFormatSize = 0;

    std::vector<unsigned char> pendingPcm;
    size_t pendingOffset = 0;

    bool eof = false;
    int lengthMs = -1000;
    AudioFormat format = AUDIO_FORMAT_UNKNOWN;
};

static const char* MfAudioFormatName(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_M4A: return "M4A";
    case AUDIO_FORMAT_AAC: return "AAC";
    case AUDIO_FORMAT_WMA: return "WMA";
    case AUDIO_FORMAT_MP3: return "MP3";
    case AUDIO_FORMAT_WAV: return "WAV";
    case AUDIO_FORMAT_FLAC: return "FLAC";
    case AUDIO_FORMAT_OGG: return "OGG";
    default: return "UNKNOWN";
    }
}

bool MfIsNativeMediaFormat(AudioFormat format)
{
    return format == AUDIO_FORMAT_M4A || format == AUDIO_FORMAT_AAC || format == AUDIO_FORMAT_WMA;
}

const wchar_t* MfMimeTypeForFormat(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_M4A:
        return L"audio/mp4";

    case AUDIO_FORMAT_AAC:
        return L"audio/aac";

    case AUDIO_FORMAT_WMA:
        return L"audio/x-ms-wma";

    default:
        return L"application/octet-stream";
    }
}

const wchar_t* MfVirtualUrlExtensionForFormat(AudioFormat format)
{
    switch (format)
    {
    case AUDIO_FORMAT_M4A:
        return L".m4a";

    case AUDIO_FORMAT_AAC:
        return L".aac";

    case AUDIO_FORMAT_WMA:
        return L".wma";

    default:
        return L".bin";
    }
}

static void MfReleaseSampleObjects(IMFSample* sample, IMFMediaBuffer* buffer)
{
    if (buffer)
        buffer->Release();

    if (sample)
        sample->Release();
}

static bool MfBuildVirtualUrl(AudioFormat format, wchar_t* outUrl, size_t outUrlCount)
{
    if (!outUrl || outUrlCount == 0)
        return false;

    outUrl[0] = L'\0';

    const wchar_t* ext = MfVirtualUrlExtensionForFormat(format);
    HRESULT hr = StringCchPrintfW(outUrl, outUrlCount, L"archive_stream_virtual%s", ext);

    return SUCCEEDED(hr);
}

static bool MfConfigurePcmOutput(IMFSourceReader* reader)
{
    if (!reader)
    {
        DebugLogWrite("ERROR", "MF", "CONFIGURE_PCM_FAIL", "reason=null_reader");
        return false;
    }

    HRESULT hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MF", "CONFIGURE_PCM_FAIL", "hr=0x%08lX | reason=disable_all_streams_failed", (unsigned long)hr);
        return false;
    }

    hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MF", "CONFIGURE_PCM_FAIL", "hr=0x%08lX | reason=select_audio_stream_failed", (unsigned long)hr);
        return false;
    }

    IMFMediaType* partialType = nullptr;
    hr = MFCreateMediaType(&partialType);

    if (FAILED(hr) || !partialType)
    {
        DebugLogWrite("ERROR", "MF", "CONFIGURE_PCM_FAIL", "hr=0x%08lX | reason=create_media_type_failed", (unsigned long)hr);
        return false;
    }

    hr = partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);

    if (SUCCEEDED(hr))
        hr = partialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);

    if (SUCCEEDED(hr))
        hr = partialType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

    if (SUCCEEDED(hr))
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, partialType);

    partialType->Release();

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MF", "CONFIGURE_PCM_FAIL", "hr=0x%08lX | reason=set_pcm_media_type_failed", (unsigned long)hr);
        return false;
    }

    return true;
}

static bool MfLoadCurrentWaveFormat(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->reader)
    {
        DebugLogWrite("ERROR", "MF", "WAVE_FORMAT_FAIL", "reason=invalid_decoder");
        return false;
    }

    IMFMediaType* currentType = nullptr;
    HRESULT hr = decoder->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &currentType);

    if (FAILED(hr) || !currentType)
    {
        DebugLogWrite("ERROR", "MF", "WAVE_FORMAT_FAIL", "hr=0x%08lX | reason=get_current_media_type_failed", (unsigned long)hr);
        return false;
    }

    WAVEFORMATEX* wf = nullptr;
    UINT32 wfSize = 0;

    hr = MFCreateWaveFormatExFromMFMediaType(currentType, &wf, &wfSize);
    currentType->Release();

    if (FAILED(hr) || !wf)
    {
        DebugLogWrite("ERROR", "MF", "WAVE_FORMAT_FAIL", "hr=0x%08lX | reason=create_wave_format_failed", (unsigned long)hr);
        return false;
    }

    if (decoder->waveFormat)
    {
        CoTaskMemFree(decoder->waveFormat);
        decoder->waveFormat = nullptr;
        decoder->waveFormatSize = 0;
    }

    decoder->waveFormat = wf;
    decoder->waveFormatSize = wfSize;

    return true;
}

static void MfLoadDuration(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->reader)
        return;

    decoder->lengthMs = -1000;

    PROPVARIANT var;
    PropVariantInit(&var);

    HRESULT hr = decoder->reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var);

    if (SUCCEEDED(hr) && var.vt == VT_UI8)
    {
        ULONGLONG duration100ns = var.uhVal.QuadPart;
        decoder->lengthMs = (int)(duration100ns / 10000ULL);
    }

    PropVariantClear(&var);
}

bool MfOpenArchiveDecoder(const char* archivePath, mz_uint index, mz_uint64 length, AudioFormat format, MfAudioDecoder** outDecoder)
{
    if (outDecoder)
        *outDecoder = nullptr;

    if (!outDecoder || !archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index, MfAudioFormatName(format));
        return false;
    }

    if (!MfIsNativeMediaFormat(format))
    {
        DebugLogWrite("WARN", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=unsupported_format", archivePath, (unsigned int)index, MfAudioFormatName(format));
        return false;
    }

    if (!MfEnsureStarted())
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=mf_startup_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        return false;
    }

    MfAudioDecoder* decoder = new (std::nothrow) MfAudioDecoder();

    if (!decoder)
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=decoder_alloc_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        return false;
    }

    decoder->format = format;

    ArchiveMFByteStream* byteStream = new (std::nothrow) ArchiveMFByteStream();

    if (!byteStream)
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=bytestream_alloc_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        MfCloseDecoder(decoder);
        return false;
    }

    wchar_t virtualUrl[260];

    if (!MfBuildVirtualUrl(format, virtualUrl, sizeof(virtualUrl) / sizeof(virtualUrl[0])))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=virtual_url_build_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        byteStream->Release();
        MfCloseDecoder(decoder);
        return false;
    }

    if (!byteStream->Open(archivePath, index, length, virtualUrl, MfMimeTypeForFormat(format)))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | length=%llu | reason=bytestream_open_failed", archivePath, (unsigned int)index, MfAudioFormatName(format), (unsigned long long)length);
        byteStream->Release();
        MfCloseDecoder(decoder);
        return false;
    }

    decoder->byteStream = byteStream;

    IMFAttributes* readerAttrs = nullptr;
    HRESULT hr = MFCreateAttributes(&readerAttrs, 2);

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | hr=0x%08lX | reason=create_reader_attributes_failed", archivePath, (unsigned int)index, MfAudioFormatName(format), (unsigned long)hr);
        MfCloseDecoder(decoder);
        return false;
    }

    /*
        Keep converter support enabled so MF can decode AAC/WMA to PCM.
    */
    readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    hr = MFCreateSourceReaderFromByteStream(decoder->byteStream, readerAttrs, &decoder->reader);
    readerAttrs->Release();

    if (FAILED(hr) || !decoder->reader)
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | hr=0x%08lX | reason=create_source_reader_from_bytestream_failed", archivePath, (unsigned int)index, MfAudioFormatName(format), (unsigned long)hr);
        MfCloseDecoder(decoder);
        return false;
    }

    if (!MfConfigurePcmOutput(decoder->reader))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=configure_pcm_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        MfCloseDecoder(decoder);
        return false;
    }

    if (!MfLoadCurrentWaveFormat(decoder))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_ARCHIVE_FAIL", "archive=%s | index=%u | format=%s | reason=load_wave_format_failed", archivePath, (unsigned int)index, MfAudioFormatName(format));
        MfCloseDecoder(decoder);
        return false;
    }

    MfLoadDuration(decoder);

    *outDecoder = decoder;
    return true;
}

static bool MfCharPathToWidePath(const char* path, wchar_t* outWidePath, size_t outWidePathCount)
{
    if (!path || !path[0] || !outWidePath || outWidePathCount == 0)
        return false;

    outWidePath[0] = L'\0';

    int needed = MultiByteToWideChar(CP_UTF8, 0, path, -1, outWidePath, (int)outWidePathCount);

    if (needed > 0)
        return true;

    needed = MultiByteToWideChar(CP_ACP, 0, path, -1, outWidePath, (int)outWidePathCount);

    return needed > 0;
}

bool MfOpenCacheFileDecoder(const char* cachePath, AudioFormat format, MfAudioDecoder** outDecoder)
{
    if (outDecoder)
        *outDecoder = nullptr;

    if (!outDecoder || !cachePath || !cachePath[0])
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=invalid_input", cachePath ? cachePath : "", MfAudioFormatName(format));
        return false;
    }

    if (!MfIsNativeMediaFormat(format))
    {
        DebugLogWrite("WARN", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=unsupported_format", cachePath, MfAudioFormatName(format));
        return false;
    }

    if (!MfEnsureStarted())
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=mf_startup_failed", cachePath, MfAudioFormatName(format));
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!MfCharPathToWidePath(cachePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=path_convert_failed", cachePath, MfAudioFormatName(format));
        return false;
    }

    MfAudioDecoder* decoder = new (std::nothrow) MfAudioDecoder();

    if (!decoder)
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=decoder_alloc_failed", cachePath, MfAudioFormatName(format));
        return false;
    }

    decoder->format = format;

    IMFAttributes* readerAttrs = nullptr;
    HRESULT hr = MFCreateAttributes(&readerAttrs, 2);

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | hr=0x%08lX | reason=create_reader_attributes_failed", cachePath, MfAudioFormatName(format), (unsigned long)hr);
        MfCloseDecoder(decoder);
        return false;
    }

    readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

    hr = MFCreateSourceReaderFromURL(widePath, readerAttrs, &decoder->reader);
    readerAttrs->Release();

    if (FAILED(hr) || !decoder->reader)
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | hr=0x%08lX | reason=create_source_reader_from_url_failed", cachePath, MfAudioFormatName(format), (unsigned long)hr);
        MfCloseDecoder(decoder);
        return false;
    }

    if (!MfConfigurePcmOutput(decoder->reader))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=configure_pcm_failed", cachePath, MfAudioFormatName(format));
        MfCloseDecoder(decoder);
        return false;
    }

    if (!MfLoadCurrentWaveFormat(decoder))
    {
        DebugLogWrite("ERROR", "MF", "OPEN_CACHE_FAIL", "cache=%s | format=%s | reason=load_wave_format_failed", cachePath, MfAudioFormatName(format));
        MfCloseDecoder(decoder);
        return false;
    }

    MfLoadDuration(decoder);

    *outDecoder = decoder;
    return true;
}

static size_t MfConsumePending(MfAudioDecoder* decoder, unsigned char* out, size_t outCapacity)
{
    if (!decoder || !out || outCapacity == 0)
        return 0;

    if (decoder->pendingOffset >= decoder->pendingPcm.size())
    {
        decoder->pendingPcm.clear();
        decoder->pendingOffset = 0;
        return 0;
    }

    size_t available = decoder->pendingPcm.size() - decoder->pendingOffset;
    size_t toCopy = available < outCapacity ? available : outCapacity;

    memcpy(out, decoder->pendingPcm.data() + decoder->pendingOffset, toCopy);

    decoder->pendingOffset += toCopy;

    if (decoder->pendingOffset >= decoder->pendingPcm.size())
    {
        decoder->pendingPcm.clear();
        decoder->pendingOffset = 0;
    }

    return toCopy;
}

static bool MfReadNextSampleToPending(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->reader || decoder->eof)
        return false;

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = nullptr;

    HRESULT hr = decoder->reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample);

    if (FAILED(hr))
    {
        decoder->eof = true;
        DebugLogWrite("ERROR", "MF", "READ_SAMPLE_FAIL", "format=%s | hr=0x%08lX | reason=read_sample_failed", MfAudioFormatName(decoder->format), (unsigned long)hr);
        return false;
    }

    if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
    {
        decoder->eof = true;

        if (sample)
            sample->Release();

        return false;
    }

    if (!sample)
        return true;

    IMFMediaBuffer* buffer = nullptr;

    hr = sample->ConvertToContiguousBuffer(&buffer);

    if (FAILED(hr) || !buffer)
    {
        DebugLogWrite("ERROR", "MF", "READ_SAMPLE_FAIL", "format=%s | hr=0x%08lX | reason=convert_to_contiguous_buffer_failed", MfAudioFormatName(decoder->format), (unsigned long)hr);
        MfReleaseSampleObjects(sample, buffer);
        return false;
    }

    BYTE* data = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;

    hr = buffer->Lock(&data, &maxLength, &currentLength);

    if (FAILED(hr) || !data || currentLength == 0)
    {
        if (SUCCEEDED(hr) && buffer)
            buffer->Unlock();

        DebugLogWrite("ERROR", "MF", "READ_SAMPLE_FAIL", "format=%s | hr=0x%08lX | current_length=%lu | reason=buffer_lock_failed_or_empty", MfAudioFormatName(decoder->format), (unsigned long)hr, (unsigned long)currentLength);
        MfReleaseSampleObjects(sample, buffer);
        return false;
    }

    try
    {
        decoder->pendingPcm.assign(data, data + currentLength);
        decoder->pendingOffset = 0;
    }
    catch (...)
    {
        DebugLogWrite("ERROR", "MF", "READ_SAMPLE_FAIL", "format=%s | bytes=%lu | reason=pending_pcm_assign_failed", MfAudioFormatName(decoder->format), (unsigned long)currentLength);
        buffer->Unlock();
        MfReleaseSampleObjects(sample, buffer);
        return false;
    }

    buffer->Unlock();
    MfReleaseSampleObjects(sample, buffer);

    return true;
}

size_t MfReadPcm(MfAudioDecoder* decoder, void* outBuffer, size_t outBufferBytes)
{
    if (!decoder || !outBuffer || outBufferBytes == 0)
        return 0;

    unsigned char* out = static_cast<unsigned char*>(outBuffer);
    size_t total = 0;

    while (total < outBufferBytes)
    {
        size_t copied = MfConsumePending(decoder, out + total, outBufferBytes - total);

        if (copied > 0)
        {
            total += copied;
            continue;
        }

        if (decoder->eof)
            break;

        if (!MfReadNextSampleToPending(decoder))
            break;
    }

    return total;
}

bool MfSeekMs(MfAudioDecoder* decoder, int positionMs)
{
    if (!decoder || !decoder->reader)
    {
        DebugLogWrite("WARN", "MF", "SEEK_FAIL", "position_ms=%d | reason=invalid_decoder", positionMs);
        return false;
    }

    if (positionMs < 0)
        positionMs = 0;

    PROPVARIANT var;
    PropVariantInit(&var);

    LONGLONG target100ns = (LONGLONG)positionMs * 10000LL;
    HRESULT hr = InitPropVariantFromInt64(target100ns, &var);

    if (FAILED(hr))
    {
        DebugLogWrite("WARN", "MF", "SEEK_FAIL", "position_ms=%d | hr=0x%08lX | reason=init_prop_variant_failed", positionMs, (unsigned long)hr);
        return false;
    }

    hr = decoder->reader->SetCurrentPosition(GUID_NULL, var);

    PropVariantClear(&var);

    if (FAILED(hr))
    {
        DebugLogWrite("WARN", "MF", "SEEK_FAIL", "position_ms=%d | hr=0x%08lX | reason=set_current_position_failed", positionMs, (unsigned long)hr);
        return false;
    }

    decoder->pendingPcm.clear();
    decoder->pendingOffset = 0;
    decoder->eof = false;

    return true;
}

void MfCloseDecoder(MfAudioDecoder* decoder)
{
    if (!decoder)
        return;

    if (decoder->reader)
    {
        decoder->reader->Release();
        decoder->reader = nullptr;
    }

    if (decoder->byteStream)
    {
        decoder->byteStream->Close();
        decoder->byteStream->Release();
        decoder->byteStream = nullptr;
    }

    if (decoder->waveFormat)
    {
        CoTaskMemFree(decoder->waveFormat);
        decoder->waveFormat = nullptr;
        decoder->waveFormatSize = 0;
    }

    delete decoder;
}

int MfGetSampleRate(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->waveFormat)
        return 0;

    return (int)decoder->waveFormat->nSamplesPerSec;
}

int MfGetChannels(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->waveFormat)
        return 0;

    return (int)decoder->waveFormat->nChannels;
}

int MfGetBitsPerSample(MfAudioDecoder* decoder)
{
    if (!decoder || !decoder->waveFormat)
        return 0;

    return (int)decoder->waveFormat->wBitsPerSample;
}

int MfGetLengthMs(MfAudioDecoder* decoder)
{
    if (!decoder)
        return -1000;

    return decoder->lengthMs;
}