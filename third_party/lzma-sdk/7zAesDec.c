/* 7zAesDec.c -- 7z AES-256/SHA-256 CBC decryption over a bounded,
   seekable look stream. Uses 7-Zip public-domain Aes/Sha256 sources. */
#include "Precomp.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "7z.h"
#include "7zAesDec.h"
#include "Sha256.h"

/* Interlocked one-time initialization also links into the 32-bit plugin's
   deliberately minimal CRT/import-library configuration. */
static volatile LONG g_7zAesReady = 0;

static void Sz7zAesEnsurePrepared(void)
{
  if (InterlockedCompareExchange(&g_7zAesReady, 1, 0) == 0)
  {
    AesGenTables();
    Sha256Prepare();
    InterlockedExchange(&g_7zAesReady, 2);
  }
  else
  {
    while (InterlockedCompareExchange(&g_7zAesReady, 2, 2) != 2)
      YieldProcessor();
  }
}

static SRes Sz7zAesLook(ILookInStreamPtr pp, const void **buf, size_t *size)
{
  C7zAesLook *p = (C7zAesLook *)(void *)pp;
  size_t requested = *size;
  if (p->used == p->buffered)
  {
    size_t chunk;
    if (p->nextRead >= p->packedSize)
    {
      *size = 0;
      *buf = p->data;
      return SZ_OK;
    }
    chunk = sizeof(p->data);
    if ((UInt64)chunk > p->packedSize - p->nextRead)
      chunk = (size_t)(p->packedSize - p->nextRead);
    if ((chunk & 15) != 0)
      return SZ_ERROR_ARCHIVE;
    RINOK(LookInStream_SeekTo(p->source, p->start + p->nextRead))
    RINOK(LookInStream_Read2(p->source, p->data, chunk, SZ_ERROR_INPUT_EOF))
    g_AesCbc_Decode(p->aes, p->data, chunk / 16);
    p->nextRead += chunk;
    p->used = 0;
    p->buffered = chunk;
  }
  if (requested > p->buffered - p->used)
    requested = p->buffered - p->used;
  *buf = p->data + p->used;
  *size = requested;
  return SZ_OK;
}

static SRes Sz7zAesSkip(ILookInStreamPtr pp, size_t offset)
{
  C7zAesLook *p = (C7zAesLook *)(void *)pp;
  if (offset > p->buffered - p->used)
    return SZ_ERROR_PARAM;
  p->used += offset;
  return SZ_OK;
}

static SRes Sz7zAesRead(ILookInStreamPtr pp, void *data, size_t *size)
{
  size_t wanted = *size;
  size_t total = 0;
  while (total < wanted)
  {
    const void *source;
    size_t take = wanted - total;
    SRes res = Sz7zAesLook(pp, &source, &take);
    if (res != SZ_OK)
      return res;
    if (take == 0)
      break;
    memcpy((Byte *)data + total, source, take);
    RINOK(Sz7zAesSkip(pp, take))
    total += take;
  }
  *size = total;
  return SZ_OK;
}

static SRes Sz7zAesSeek(ILookInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
  C7zAesLook *p = (C7zAesLook *)(void *)pp;
  if ((origin == SZ_SEEK_SET && *pos == 0) ||
      (origin == SZ_SEEK_CUR && *pos == 0))
  {
    if (origin == SZ_SEEK_SET)
    {
      p->nextRead = 0;
      p->buffered = 0;
      p->used = 0;
      AesCbc_Init(p->aes, p->iv);
    }
    *pos = origin == SZ_SEEK_CUR
        ? (Int64)(p->nextRead - (UInt64)(p->buffered - p->used)) : 0;
    return SZ_OK;
  }
  return SZ_ERROR_UNSUPPORTED;
}

SRes Sz7zAes_Init(C7zAesLook *p, ILookInStreamPtr source,
    UInt64 start, UInt64 packedSize, const Byte *props, unsigned propsSize,
    const UInt16 *password)
{
  unsigned cycles;
  unsigned saltSize = 0;
  unsigned ivSize = 0;
  const Byte *salt = NULL;
  Byte key[32] = {0};
  CSha256 sha;
  size_t passwordLen = 0;
  unsigned i;
  if (!source || !props || propsSize == 0 || !password)
    return SZ_ERROR_UNSUPPORTED;
  if (packedSize % 16 != 0 || start > (UInt64)-1 - packedSize)
    return SZ_ERROR_ARCHIVE;
  cycles = props[0] & 0x3F;
  if (cycles != 0x3F && cycles > 24)
    return SZ_ERROR_UNSUPPORTED;
  if (props[0] & 0xC0)
  {
    if (propsSize < 2)
      return SZ_ERROR_ARCHIVE;
    saltSize = ((props[0] >> 7) & 1) + (props[1] >> 4);
    ivSize = ((props[0] >> 6) & 1) + (props[1] & 0x0F);
    if (saltSize > 16 || ivSize > 16 || propsSize != 2 + saltSize + ivSize)
      return SZ_ERROR_ARCHIVE;
    salt = props + 2;
  }
  else if (propsSize != 1)
    return SZ_ERROR_ARCHIVE;
  while (password[passwordLen] != 0)
  {
    if (++passwordLen > 512)
      return SZ_ERROR_UNSUPPORTED;
  }
  Sz7zAesEnsurePrepared();
  if (cycles == 0x3F)
  {
    size_t n = saltSize;
    if (saltSize)
      memcpy(key, salt, saltSize);
    for (i = 0; i < passwordLen && n + 2 <= sizeof(key); ++i)
    {
      key[n++] = (Byte)password[i];
      key[n++] = (Byte)(password[i] >> 8);
    }
  }
  else
  {
    UInt64 counter;
    UInt64 rounds = ((UInt64)1) << cycles;
    /* One SHA update per round; the buffer holds salt + UTF-16LE password
       + the 64-bit little-endian round counter. No password is logged. */
    Byte hashInput[16 + 512 * 2 + 8];
    size_t hashLength = saltSize + passwordLen * 2 + 8;
    if (saltSize)
      memcpy(hashInput, salt, saltSize);
    for (i = 0; i < passwordLen; ++i)
    {
      hashInput[saltSize + i * 2] = (Byte)password[i];
      hashInput[saltSize + i * 2 + 1] = (Byte)(password[i] >> 8);
    }
    Sha256_Init(&sha);
    for (counter = 0; counter < rounds; ++counter)
    {
      for (i = 0; i < 8; ++i)
        hashInput[hashLength - 8 + i] = (Byte)(counter >> (i * 8));
      Sha256_Update(&sha, hashInput, hashLength);
    }
    Sha256_Final(&sha, key);
    SecureZeroMemory(hashInput, sizeof(hashInput));
    SecureZeroMemory(&sha, sizeof(sha));
  }
  memset(p, 0, sizeof(*p));
  p->vt.Look = Sz7zAesLook;
  p->vt.Skip = Sz7zAesSkip;
  p->vt.Read = Sz7zAesRead;
  p->vt.Seek = Sz7zAesSeek;
  p->source = source;
  p->start = start;
  p->packedSize = packedSize;
  if (ivSize)
    memcpy(p->iv, props + 2 + saltSize, ivSize);
  AesCbc_Init(p->aes, p->iv);
  Aes_SetKey_Dec(p->aes + 4, key, 32);
  SecureZeroMemory(key, sizeof(key));
  return SZ_OK;
}

void Sz7zAes_Clear(C7zAesLook *p)
{
  if (p)
    SecureZeroMemory(p, sizeof(*p));
}
