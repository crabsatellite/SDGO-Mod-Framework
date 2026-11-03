#pragma once

#include <cstddef>
#include <cstdint>

#define SDGO_STORAGE_EXTENSION_API_EXPORT "SdgoStorageGetExtensionApiV1"
#define SDGO_STORAGE_EXTENSION_API_VERSION 1u
#define SDGO_STORAGE_EXTENSION_SINK_VERSION 1u

struct SdgoStorageExtensionSinkV1 {
    uint32_t size;
    uint32_t version;
    void* user;

    // Return nonzero only after positively identifying and consuming a packet.
    // The native callback does not expose a packet length, so fixedSize is zero
    // unless the hook owner can prove a fixed size for that opcode.
    int(__cdecl* packet_prefilter)(
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t fixedSize,
        void* user);

    void(__cdecl* page_attach)(void* service, void* root, void* user);
    void(__cdecl* page_detach)(void* service, void* user);

    // Return nonzero when the event belongs to the extension. result receives
    // the native callback result that the hook owner must return to the game.
    int(__cdecl* ui_event)(
        void* handler,
        void* service,
        uint32_t event,
        uintptr_t second,
        void* control,
        uint32_t* result,
        void* user);

    // Return nonzero while native Claim All must not process the gift tab.
    int(__cdecl* block_claim_all)(void* service, void* user);

    // Called after a packet that was not consumed has passed through the
    // native storage handler. Extensions can re-assert presentation state
    // that the stock asynchronous refresh may have changed.
    void(__cdecl* packet_postfilter)(
        void* service,
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t fixedSize,
        void* user);
};

inline constexpr std::size_t
    SDGO_STORAGE_EXTENSION_SINK_V1_BASE_SIZE =
        offsetof(
            SdgoStorageExtensionSinkV1,
            packet_postfilter);

struct SdgoStorageExtensionApiV1 {
    uint32_t size;
    uint32_t version;
    int(__cdecl* register_sink)(const SdgoStorageExtensionSinkV1* sink);
    int(__cdecl* unregister_sink)(void* user);
};

using SdgoStorageGetExtensionApiV1Fn =
    const SdgoStorageExtensionApiV1*(__cdecl*)();
