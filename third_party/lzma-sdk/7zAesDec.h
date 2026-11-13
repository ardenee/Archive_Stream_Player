/* 7zAesDec.h -- AES-256/SHA-256 decoder for 7z packed streams.
   Uses Igor Pavlov's public-domain Aes.c and Sha256.c; no external DLL. */
#ifndef ZIP7_INC_7Z_AES_DEC_H
#define ZIP7_INC_7Z_AES_DEC_H

#include "7zTypes.h"
#include "Aes.h"

EXTERN_C_BEGIN

typedef struct
{
  ILookInStream vt;
  ILookInStreamPtr source;
  UInt64 start;
  UInt64 packedSize;
  UInt64 nextRead;
  size_t used;
  size_t buffered;
  Byte iv[AES_BLOCK_SIZE];
  __declspec(align(16)) UInt32 aes[AES_NUM_IVMRK_WORDS];
  __declspec(align(16)) Byte data[1 << 16];
} C7zAesLook;

SRes Sz7zAes_Init(C7zAesLook *p, ILookInStreamPtr source,
    UInt64 start, UInt64 packedSize, const Byte *props, unsigned propsSize,
    const UInt16 *password);

void Sz7zAes_Clear(C7zAesLook *p);

EXTERN_C_END
#endif
