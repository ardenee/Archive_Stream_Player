#pragma once

#define NOMINMAX
#include <windows.h>
#include <mfidl.h>
#include <mfobjects.h>

#include "archive_common.h"

class ArchiveMFByteStream :
    public IMFByteStream,
    public IMFAttributes
{
public:
    ArchiveMFByteStream();
    virtual ~ArchiveMFByteStream();

    bool Open(
        const char* archivePath,
        mz_uint index,
        mz_uint64 length,
        const wchar_t* virtualUrl,
        const wchar_t* mimeType
    );

    void CloseInternal();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override;
    STDMETHODIMP GetLength(QWORD* pqwLength) override;
    STDMETHODIMP SetLength(QWORD qwLength) override;
    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override;
    STDMETHODIMP SetCurrentPosition(QWORD qwPosition) override;
    STDMETHODIMP IsEndOfStream(BOOL* pfEndOfStream) override;
    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* pcbRead) override;
    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) override;
    STDMETHODIMP Write(const BYTE* pb, ULONG cb, ULONG* pcbWritten) override;
    STDMETHODIMP BeginWrite(const BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndWrite(IMFAsyncResult* pResult, ULONG* pcbWritten) override;
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD dwSeekFlags, QWORD* pqwCurrentPosition) override;
    STDMETHODIMP Flush() override;
    STDMETHODIMP Close() override;

    // IMFAttributes, delegated to internal attribute store
    STDMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) override;
    STDMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) override;
    STDMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) override;
    STDMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue) override;
    STDMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue) override;
    STDMETHODIMP GetDouble(REFGUID guidKey, double* pfValue) override;
    STDMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue) override;
    STDMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength) override;
    STDMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override;
    STDMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override;
    STDMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) override;
    STDMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) override;
    STDMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override;
    STDMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    STDMETHODIMP DeleteItem(REFGUID guidKey) override;
    STDMETHODIMP DeleteAllItems() override;
    STDMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    STDMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    STDMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    STDMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    STDMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
    STDMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) override;
    STDMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* pUnknown) override;
    STDMETHODIMP LockStore() override;
    STDMETHODIMP UnlockStore() override;
    STDMETHODIMP GetCount(UINT32* pcItems) override;
    STDMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) override;
    STDMETHODIMP CopyAllItems(IMFAttributes* pDest) override;

private:
    volatile LONG m_refCount;
    CRITICAL_SECTION m_lock;
    bool m_closed;

    ArchiveStreamContext m_stream;

    bool m_streamOpen;
    bool m_hardReadFailure;
    bool m_readZeroLogged;

    QWORD m_length;

    QWORD m_position;

    IMFAttributes* m_attributes;
};