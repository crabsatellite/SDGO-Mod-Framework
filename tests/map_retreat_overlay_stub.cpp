#include "support/native_resource_overlay/native_resource_overlay.h"

namespace {
int __cdecl RegisterBundle(
    const wchar_t*, const wchar_t*, const char*, char*, std::uint32_t) {
    return 1;
}

const SdgoResourceOverlayBrokerApiV1 kApi{
    sizeof(SdgoResourceOverlayBrokerApiV1), 1, &RegisterBundle};
}

extern "C" __declspec(dllexport)
const SdgoResourceOverlayBrokerApiV1* __cdecl SdgoGetResourceOverlayBrokerV1() {
    return &kApi;
}
