#pragma once
#include <stdint.h>
#include <wchar.h>
// A consumer returns a borrowed Unicode string for its own exact wire marker,
// or nullptr to retain the original conversion. Register during Mod load.
#define SDGO_NATIVE_TEXT_DECODER_EXPORT "SdgoRegisterNativeTextDecoderV1"
struct SdgoNativeTextDecoderV1 {
    uint32_t size,version;
    const wchar_t* (__cdecl* decode)(unsigned,const char*,void*);
    void* user;
};
typedef int(__cdecl* SdgoRegisterNativeTextDecoderV1Fn)(const SdgoNativeTextDecoderV1*);
