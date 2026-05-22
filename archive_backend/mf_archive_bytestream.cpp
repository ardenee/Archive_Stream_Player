#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <propvarutil.h>
#include <strsafe.h>
#include <new>

#include "mf_archive_bytestream.h"
#include "archive_reader.h"
#include "debug_log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class ArchiveMFAsyncReadState : public IUnknown
{
public:
    ArchiveMFAsyncReadState(ULONG bytesRead) :
        m_refCount(1),
        m_bytesRead(bytesRead)
    {
    }

    ULONG GetBytesRead() const
    {
        return m_bytesRead;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;

        *ppv = nullptr;

        if (riid == IID_IUnknown)
        {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG count = (ULONG)InterlockedDecrement(&m_refCount);

        if (count == 0)
            delete this;

        return count;
    }

private:
    volatile LONG m_refCount;
    ULONG m_bytesRead;
};

ArchiveMFByteStream::ArchiveMFByteStream() :
    m_refCount(1),
    m_closed(false),
    m_streamOpen(false),
    m_hardReadFailure(false),
    m_readZeroLogged(false),
    m_length(0),
    m_position(0),
    m_attributes(nullptr)
{
    InitializeCriticalSection(&m_lock);

    memset(&m_stream, 0, sizeof(m_stream));

    HRESULT hr = MFCreateAttributes(&m_attributes, 4);

    if (FAILED(hr))
    {
        m_attributes = nullptr;
        DebugLogWrite("ERROR", "MFSTREAM", "ATTRIBUTES_CREATE_FAIL", "hr=0x%08lX", (unsigned long)hr);
    }
}

ArchiveMFByteStream::~ArchiveMFByteStream()
{
    CloseInternal();

    if (m_attributes)
    {
        m_attributes->Release();
        m_attributes = nullptr;
    }

    DeleteCriticalSection(&m_lock);
}

bool ArchiveMFByteStream::Open(const char* archivePath, mz_uint index, mz_uint64 length, const wchar_t* virtualUrl, const wchar_t* mimeType)
{
    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "MFSTREAM", "OPEN_FAIL", "archive=%s | index=%u | length=%llu | reason=invalid_archive_path", archivePath ? archivePath : "", (unsigned int)index, (unsigned long long)length);
        return false;
    }

    EnterCriticalSection(&m_lock);

    CloseInternal();

    m_closed = false;
    m_hardReadFailure = false;
    m_readZeroLogged = false;
    m_length = static_cast<QWORD>(length);
    m_position = 0;

    bool ok = ArchiveOpenEntryStream(&m_stream, archivePath, index);

    if (ok)
    {
        m_streamOpen = true;

        if (m_attributes)
        {
            if (virtualUrl && virtualUrl[0])
                m_attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME, virtualUrl);

            if (mimeType && mimeType[0])
                m_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, mimeType);
        }
    }
    else
    {
        DebugLogWrite("ERROR", "MFSTREAM", "OPEN_FAIL", "archive=%s | index=%u | length=%llu | reason=archive_open_entry_stream_failed", archivePath ? archivePath : "", (unsigned int)index, (unsigned long long)m_length);
    }

    LeaveCriticalSection(&m_lock);
    return ok;
}


void ArchiveMFByteStream::CloseInternal()
{
    if (m_streamOpen)
    {
        ArchiveCloseEntryStream(&m_stream);
        m_streamOpen = false;
    }

    m_closed = true;
    m_position = 0;
}


STDMETHODIMP ArchiveMFByteStream::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;

    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == __uuidof(IMFByteStream))
    {
        *ppv = static_cast<IMFByteStream*>(this);
    }
    else if (riid == __uuidof(IMFAttributes))
    {
        *ppv = static_cast<IMFAttributes*>(this);
    }
    else
    {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ArchiveMFByteStream::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) ArchiveMFByteStream::Release()
{
    ULONG count = (ULONG)InterlockedDecrement(&m_refCount);

    if (count == 0)
        delete this;

    return count;
}

STDMETHODIMP ArchiveMFByteStream::GetCapabilities(DWORD* pdwCapabilities)
{
    if (!pdwCapabilities)
        return E_POINTER;

    *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;

    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::GetLength(QWORD* pqwLength)
{
    if (!pqwLength)
        return E_POINTER;

    *pqwLength = m_length;
    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::SetLength(QWORD)
{
    return E_NOTIMPL;
}

STDMETHODIMP ArchiveMFByteStream::GetCurrentPosition(QWORD* pqwPosition)
{
    if (!pqwPosition)
        return E_POINTER;

    EnterCriticalSection(&m_lock);
    *pqwPosition = m_position;
    LeaveCriticalSection(&m_lock);

    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::SetCurrentPosition(QWORD qwPosition)
{
    return Seek(msoBegin, (LONGLONG)qwPosition, 0, nullptr);
}

STDMETHODIMP ArchiveMFByteStream::IsEndOfStream(BOOL* pfEndOfStream)
{
    if (!pfEndOfStream)
        return E_POINTER;

    EnterCriticalSection(&m_lock);
    *pfEndOfStream = (m_position >= m_length) ? TRUE : FALSE;
    LeaveCriticalSection(&m_lock);

    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead)
{
    if (!pb)
    {
        return E_POINTER;
    }

    if (pcbRead)
    {
        *pcbRead = 0;
    }

    EnterCriticalSection(&m_lock);

    if (m_closed || !m_streamOpen)
    {
        LeaveCriticalSection(&m_lock);
        return MF_E_SHUTDOWN;
    }

    if (m_hardReadFailure)
    {
        LeaveCriticalSection(&m_lock);
        return E_FAIL;
    }

    if (m_position >= m_length)
    {
        LeaveCriticalSection(&m_lock);
        return S_OK;
    }

    QWORD remaining = m_length - m_position;

    if ((QWORD)cb > remaining)
    {
        cb = (ULONG)remaining;
    }

    size_t got = ArchiveReadEntryStream(&m_stream, pb, cb);

    if (got == 0 && cb > 0 && m_position < m_length)
    {
        m_hardReadFailure = true;

        if (!m_readZeroLogged)
        {
            m_readZeroLogged = true;
            DebugLogWrite("WARN", "MFSTREAM", "READ_ZERO", "position=%llu | length=%llu | requested=%lu | result=hard_failure", (unsigned long long)m_position, (unsigned long long)m_length, (unsigned long)cb);
        }

        LeaveCriticalSection(&m_lock);
        return E_FAIL;
    }

    m_position += got;

    if (pcbRead)
    {
        *pcbRead = (ULONG)got;
    }

    LeaveCriticalSection(&m_lock);
    return S_OK;
}


STDMETHODIMP ArchiveMFByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    if (!pb)
        return E_POINTER;

    if (!pCallback)
        return E_INVALIDARG;

    ULONG bytesRead = 0;

    HRESULT readHr = Read(pb, cb, &bytesRead);

    ArchiveMFAsyncReadState* readState = new (std::nothrow) ArchiveMFAsyncReadState(bytesRead);

    if (!readState)
    {
        DebugLogWrite("ERROR", "MFSTREAM", "BEGIN_READ_FAIL", "requested=%lu | read=%lu | reason=alloc_read_state_failed", (unsigned long)cb, (unsigned long)bytesRead);
        return E_OUTOFMEMORY;
    }

    IMFAsyncResult* result = nullptr;

    HRESULT hr = MFCreateAsyncResult(readState, pCallback, punkState, &result);

    readState->Release();

    if (FAILED(hr))
    {
        DebugLogWrite("ERROR", "MFSTREAM", "BEGIN_READ_FAIL", "requested=%lu | read=%lu | hr=0x%08lX | reason=create_async_result_failed", (unsigned long)cb, (unsigned long)bytesRead, (unsigned long)hr);
        return hr;
    }

    result->SetStatus(readHr);

    hr = MFInvokeCallback(result);

    if (FAILED(hr))
        DebugLogWrite("ERROR", "MFSTREAM", "BEGIN_READ_FAIL", "requested=%lu | read=%lu | read_hr=0x%08lX | hr=0x%08lX | reason=invoke_callback_failed", (unsigned long)cb, (unsigned long)bytesRead, (unsigned long)readHr, (unsigned long)hr);

    result->Release();

    return hr;
}

STDMETHODIMP ArchiveMFByteStream::EndRead(IMFAsyncResult* pResult, ULONG* pcbRead)
{
    if (!pResult)
        return E_POINTER;

    if (!pcbRead)
        return E_POINTER;

    *pcbRead = 0;

    HRESULT status = pResult->GetStatus();

    IUnknown* object = nullptr;

    HRESULT hr = pResult->GetObject(&object);

    if (FAILED(hr) || !object)
    {
        if (FAILED(hr))
            DebugLogWrite("ERROR", "MFSTREAM", "END_READ_FAIL", "status=0x%08lX | hr=0x%08lX | reason=get_object_failed", (unsigned long)status, (unsigned long)hr);
        else
            DebugLogWrite("ERROR", "MFSTREAM", "END_READ_FAIL", "status=0x%08lX | reason=missing_object", (unsigned long)status);

        return FAILED(status) ? status : hr;
    }

    ArchiveMFAsyncReadState* typedState = static_cast<ArchiveMFAsyncReadState*>(object);

    *pcbRead = typedState->GetBytesRead();

    object->Release();

    if (FAILED(status))
    {
        DebugLogWrite("ERROR", "MFSTREAM", "END_READ_FAIL", "status=0x%08lX | bytes=%lu | reason=async_status_failed", (unsigned long)status, (unsigned long)*pcbRead);
        return status;
    }

    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::Write(const BYTE*, ULONG, ULONG*)
{
    return E_NOTIMPL;
}

STDMETHODIMP ArchiveMFByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*)
{
    return E_NOTIMPL;
}

STDMETHODIMP ArchiveMFByteStream::EndWrite(IMFAsyncResult*, ULONG*)
{
    return E_NOTIMPL;
}

STDMETHODIMP ArchiveMFByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD, QWORD* pqwCurrentPosition)
{
    EnterCriticalSection(&m_lock);

    if (m_closed || !m_streamOpen)
    {
        LeaveCriticalSection(&m_lock);
        return MF_E_SHUTDOWN;
    }

    LONGLONG base = 0;

    if (SeekOrigin == msoCurrent)
        base = (LONGLONG)m_position;

    LONGLONG target = base + llSeekOffset;

    if (target < 0)
    {
        DebugLogWrite("WARN", "MFSTREAM", "SEEK_FAIL", "target=%lld | reason=negative_target", (long long)target);
        LeaveCriticalSection(&m_lock);
        return E_INVALIDARG;
    }

    if ((QWORD)target > m_length)
    {
        DebugLogWrite("WARN", "MFSTREAM", "SEEK_FAIL", "target=%llu | length=%llu | reason=beyond_end", (unsigned long long)target, (unsigned long long)m_length);
        LeaveCriticalSection(&m_lock);
        return E_INVALIDARG;
    }

    bool ok = ArchiveSeekEntryStream(&m_stream, (mz_uint64)target);

    if (!ok)
    {
        DebugLogWrite("WARN", "MFSTREAM", "SEEK_FAIL", "target=%llu | current=%llu | length=%llu | reason=archive_seek_failed", (unsigned long long)target, (unsigned long long)m_position, (unsigned long long)m_length);
        LeaveCriticalSection(&m_lock);
        return E_FAIL;
    }

    m_position = (QWORD)target;

    if (pqwCurrentPosition)
        *pqwCurrentPosition = m_position;

    LeaveCriticalSection(&m_lock);
    return S_OK;
}


STDMETHODIMP ArchiveMFByteStream::Flush()
{
    return S_OK;
}

STDMETHODIMP ArchiveMFByteStream::Close()
{
    EnterCriticalSection(&m_lock);
    CloseInternal();
    LeaveCriticalSection(&m_lock);

    return S_OK;
}

// IMFAttributes delegation
STDMETHODIMP ArchiveMFByteStream::GetItem(REFGUID guidKey, PROPVARIANT* pValue)
{
    return m_attributes ? m_attributes->GetItem(guidKey, pValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType)
{
    return m_attributes ? m_attributes->GetItemType(guidKey, pType) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult)
{
    return m_attributes ? m_attributes->CompareItem(guidKey, Value, pbResult) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult)
{
    return m_attributes ? m_attributes->Compare(pTheirs, MatchType, pbResult) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetUINT32(REFGUID guidKey, UINT32* punValue)
{
    return m_attributes ? m_attributes->GetUINT32(guidKey, punValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetUINT64(REFGUID guidKey, UINT64* punValue)
{
    return m_attributes ? m_attributes->GetUINT64(guidKey, punValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetDouble(REFGUID guidKey, double* pfValue)
{
    return m_attributes ? m_attributes->GetDouble(guidKey, pfValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetGUID(REFGUID guidKey, GUID* pguidValue)
{
    return m_attributes ? m_attributes->GetGUID(guidKey, pguidValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetStringLength(REFGUID guidKey, UINT32* pcchLength)
{
    return m_attributes ? m_attributes->GetStringLength(guidKey, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength)
{
    return m_attributes ? m_attributes->GetString(guidKey, pwszValue, cchBufSize, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength)
{
    return m_attributes ? m_attributes->GetAllocatedString(guidKey, ppwszValue, pcchLength) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize)
{
    return m_attributes ? m_attributes->GetBlobSize(guidKey, pcbBlobSize) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize)
{
    return m_attributes ? m_attributes->GetBlob(guidKey, pBuf, cbBufSize, pcbBlobSize) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize)
{
    return m_attributes ? m_attributes->GetAllocatedBlob(guidKey, ppBuf, pcbSize) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv)
{
    return m_attributes ? m_attributes->GetUnknown(guidKey, riid, ppv) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetItem(REFGUID guidKey, REFPROPVARIANT Value)
{
    return m_attributes ? m_attributes->SetItem(guidKey, Value) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::DeleteItem(REFGUID guidKey)
{
    return m_attributes ? m_attributes->DeleteItem(guidKey) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::DeleteAllItems()
{
    return m_attributes ? m_attributes->DeleteAllItems() : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetUINT32(REFGUID guidKey, UINT32 unValue)
{
    return m_attributes ? m_attributes->SetUINT32(guidKey, unValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetUINT64(REFGUID guidKey, UINT64 unValue)
{
    return m_attributes ? m_attributes->SetUINT64(guidKey, unValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetDouble(REFGUID guidKey, double fValue)
{
    return m_attributes ? m_attributes->SetDouble(guidKey, fValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetGUID(REFGUID guidKey, REFGUID guidValue)
{
    return m_attributes ? m_attributes->SetGUID(guidKey, guidValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetString(REFGUID guidKey, LPCWSTR wszValue)
{
    return m_attributes ? m_attributes->SetString(guidKey, wszValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize)
{
    return m_attributes ? m_attributes->SetBlob(guidKey, pBuf, cbBufSize) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::SetUnknown(REFGUID guidKey, IUnknown* pUnknown)
{
    return m_attributes ? m_attributes->SetUnknown(guidKey, pUnknown) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::LockStore()
{
    return m_attributes ? m_attributes->LockStore() : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::UnlockStore()
{
    return m_attributes ? m_attributes->UnlockStore() : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetCount(UINT32* pcItems)
{
    return m_attributes ? m_attributes->GetCount(pcItems) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue)
{
    return m_attributes ? m_attributes->GetItemByIndex(unIndex, pguidKey, pValue) : E_UNEXPECTED;
}

STDMETHODIMP ArchiveMFByteStream::CopyAllItems(IMFAttributes* pDest)
{
    return m_attributes ? m_attributes->CopyAllItems(pDest) : E_UNEXPECTED;
}