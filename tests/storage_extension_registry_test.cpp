#include "../mods/storage_claim/storage_extension_registry.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

void Check(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << "\n";
    std::exit(1);
}

struct State {
    int attach = 0;
    int detach = 0;
    int packets = 0;
    int post_packets = 0;
    int events = 0;
    int block = 0;
    void* service = nullptr;
    void* root = nullptr;
};

int __cdecl Packet(
    uint16_t opcode,
    const uint8_t* data,
    uint32_t size,
    void* user) {
    auto& state = *static_cast<State*>(user);
    ++state.packets;
    return opcode == 0x7BE && data && data[0] == 0x47 &&
           size == 0x7EA;
}

void __cdecl PostPacket(
    void* service,
    uint16_t opcode,
    const uint8_t* data,
    uint32_t size,
    void* user) {
    auto& state = *static_cast<State*>(user);
    if (service == reinterpret_cast<void*>(0x1000) &&
        opcode == 0x7BD && data && data[0] == 0x47 &&
        size == 0) {
        ++state.post_packets;
    }
}

void __cdecl Attach(void* service, void* root, void* user) {
    auto& state = *static_cast<State*>(user);
    ++state.attach;
    state.service = service;
    state.root = root;
}

void __cdecl Detach(void* service, void* user) {
    auto& state = *static_cast<State*>(user);
    ++state.detach;
    state.service = service;
}

int __cdecl Event(
    void*,
    void* service,
    uint32_t event,
    uintptr_t,
    void* control,
    uint32_t* result,
    void* user) {
    auto& state = *static_cast<State*>(user);
    ++state.events;
    if (service != reinterpret_cast<void*>(0x1000) ||
        event != 0x101 ||
        control != reinterpret_cast<void*>(0x3000)) {
        return 0;
    }
    *result = 77;
    return 1;
}

int __cdecl Block(void* service, void* user) {
    auto& state = *static_cast<State*>(user);
    ++state.block;
    return service == reinterpret_cast<void*>(0x1000);
}

SdgoStorageExtensionSinkV1 Sink(State& state) {
    return {
        sizeof(SdgoStorageExtensionSinkV1),
        SDGO_STORAGE_EXTENSION_SINK_VERSION,
        &state,
        &Packet,
        &Attach,
        &Detach,
        &Event,
        &Block,
        &PostPacket};
}

} // namespace

int main() {
    using sdgo::storage::extension::Registry;
    Registry registry;
    State first;
    auto sink = Sink(first);

    Check(registry.Register(&sink) == 1, "first sink registers");
    Check(registry.Register(&sink) == 0, "duplicate user is rejected");

    // The registry owns a value copy, not memory in the companion DLL.
    sink.packet_prefilter = nullptr;
    uint8_t packet[] = {0x47};
    Check(
        registry.PacketPrefilter(0x7BE, packet, 0x7EA) == 1 &&
            first.packets == 1,
        "registered packet callback is copied and consumes its own packet");
    Check(
        registry.PacketPrefilter(0x7BD, packet) == 0 &&
            first.packets == 2,
        "nonmatching native packet falls through");
    registry.PacketPostfilter(
        reinterpret_cast<void*>(0x1000),
        0x7BD,
        packet);
    Check(
        first.post_packets == 1,
        "postfilter observes the native packet after stock handling");

    registry.PageAttach(
        reinterpret_cast<void*>(0x1000),
        reinterpret_cast<void*>(0x2000));
    Check(
        first.attach == 1 &&
            first.service == reinterpret_cast<void*>(0x1000) &&
            first.root == reinterpret_cast<void*>(0x2000),
        "page attach reaches registered sink");

    State second;
    auto secondSink = Sink(second);
    Check(registry.Register(&secondSink) == 1, "late sink registers");
    Check(
        second.attach == 1 &&
            second.service == reinterpret_cast<void*>(0x1000) &&
            second.root == reinterpret_cast<void*>(0x2000),
        "late registration receives current page");

    uint32_t result = 0;
    Check(
        registry.UiEvent(
            nullptr,
            reinterpret_cast<void*>(0x1000),
            0x101,
            0,
            reinterpret_cast<void*>(0x3000),
            &result) == 1 &&
            result == 77 &&
            first.events == 1 &&
            second.events == 0,
        "first owning UI extension consumes before later sinks");
    Check(
        registry.BlockClaimAll(reinterpret_cast<void*>(0x1000)) == 1 &&
            first.block == 1 &&
            second.block == 0,
        "gift Claim All stops when an extension blocks it");

    Check(registry.Unregister(&first) == 1, "registered sink unregisters");
    Check(first.detach == 1, "unregister detaches current page");
    Check(registry.Unregister(&first) == 0, "missing sink cannot unregister");

    registry.PageDetach(reinterpret_cast<void*>(0x1000));
    Check(second.detach == 1, "remaining sink receives page detach");
    Check(registry.Unregister(&second) == 1, "detached sink unregisters");
    Check(second.detach == 1, "unregister after detach does not double detach");

    State legacy;
    auto legacySink = Sink(legacy);
    legacySink.size =
        SDGO_STORAGE_EXTENSION_SINK_V1_BASE_SIZE;
    Check(
        registry.Register(&legacySink) == 1,
        "base v1 sink remains ABI compatible");
    registry.PacketPostfilter(
        reinterpret_cast<void*>(0x1000),
        0x7BD,
        packet);
    Check(
        legacy.post_packets == 0,
        "base v1 sink does not read the appended callback");
    Check(
        registry.Unregister(&legacy) == 1,
        "base v1 sink unregisters");

    SdgoStorageExtensionSinkV1 invalid{};
    Check(registry.Register(&invalid) == 0, "invalid ABI is rejected");

    std::cout << "storage_extension_registry_test: PASS\n";
    return 0;
}
