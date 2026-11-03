#pragma once

#include "sdgo/storage_extension.h"

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace sdgo::storage::extension {

class Registry {
public:
    static constexpr size_t kCapacity = 8;

    int Register(const SdgoStorageExtensionSinkV1* candidate) {
        if (!Valid(candidate)) return 0;

        SdgoStorageExtensionSinkV1 normalized{};
        std::memcpy(
            &normalized,
            candidate,
            std::min<std::size_t>(
                candidate->size,
                sizeof(normalized)));
        normalized.size = sizeof(normalized);
        SdgoStorageExtensionSinkV1 added{};
        void* service = nullptr;
        void* root = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& slot : slots_) {
                if (slot.used && slot.sink.user == candidate->user) return 0;
            }
            auto* free = FreeSlot();
            if (!free) return 0;
            free->sink = normalized;
            free->used = true;
            added = free->sink;
            service = service_;
            root = root_;
        }

        if (service && added.page_attach) {
            added.page_attach(service, root, added.user);
        }
        return 1;
    }

    int Unregister(void* user) {
        if (!user) return 0;

        SdgoStorageExtensionSinkV1 removed{};
        void* service = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& slot : slots_) {
                if (!slot.used || slot.sink.user != user) continue;
                removed = slot.sink;
                service = service_;
                slot = {};
                break;
            }
        }
        if (!removed.user) return 0;
        if (service && removed.page_detach) {
            removed.page_detach(service, removed.user);
        }
        return 1;
    }

    int PacketPrefilter(
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t fixedSize = 0) {
        const auto snapshot = Capture();
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.packet_prefilter &&
                sink.packet_prefilter(
                    opcode, packet, fixedSize, sink.user)) {
                return 1;
            }
        }
        return 0;
    }

    void PacketPostfilter(
        void* service,
        uint16_t opcode,
        const uint8_t* packet,
        uint32_t fixedSize = 0) {
        const auto snapshot = Capture();
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.packet_postfilter) {
                sink.packet_postfilter(
                    service,
                    opcode,
                    packet,
                    fixedSize,
                    sink.user);
            }
        }
    }

    void PageAttach(void* service, void* root) {
        Snapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            service_ = service;
            root_ = root;
            snapshot = SnapshotLocked();
        }
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.page_attach) sink.page_attach(service, root, sink.user);
        }
    }

    void PageDetach(void* service) {
        Snapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (service_ != service) return;
            service_ = nullptr;
            root_ = nullptr;
            snapshot = SnapshotLocked();
        }
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.page_detach) sink.page_detach(service, sink.user);
        }
    }

    int UiEvent(
        void* handler,
        void* service,
        uint32_t event,
        uintptr_t second,
        void* control,
        uint32_t* result) {
        if (!result) return 0;
        const auto snapshot = Capture();
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.ui_event &&
                sink.ui_event(
                    handler, service, event, second, control, result, sink.user)) {
                return 1;
            }
        }
        return 0;
    }

    int BlockClaimAll(void* service) {
        const auto snapshot = Capture();
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& sink = snapshot.sinks[i];
            if (sink.block_claim_all &&
                sink.block_claim_all(service, sink.user)) {
                return 1;
            }
        }
        return 0;
    }

private:
    struct Slot {
        bool used = false;
        SdgoStorageExtensionSinkV1 sink{};
    };

    struct Snapshot {
        std::array<SdgoStorageExtensionSinkV1, kCapacity> sinks{};
        size_t count = 0;
    };

    static bool Valid(const SdgoStorageExtensionSinkV1* sink) {
        if (!sink ||
            sink->size <
                SDGO_STORAGE_EXTENSION_SINK_V1_BASE_SIZE ||
            sink->version != SDGO_STORAGE_EXTENSION_SINK_VERSION ||
            !sink->user) {
            return false;
        }
        const bool hasPostfilter =
            sink->size >= sizeof(SdgoStorageExtensionSinkV1) &&
            sink->packet_postfilter;
        return sink->packet_prefilter ||
               sink->page_attach ||
               sink->page_detach ||
               sink->ui_event ||
               sink->block_claim_all ||
               hasPostfilter;
    }

    Slot* FreeSlot() {
        for (auto& slot : slots_) {
            if (!slot.used) return &slot;
        }
        return nullptr;
    }

    Snapshot SnapshotLocked() const {
        Snapshot snapshot;
        for (const auto& slot : slots_) {
            if (slot.used) snapshot.sinks[snapshot.count++] = slot.sink;
        }
        return snapshot;
    }

    Snapshot Capture() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return SnapshotLocked();
    }

    mutable std::mutex mutex_;
    std::array<Slot, kCapacity> slots_{};
    void* service_ = nullptr;
    void* root_ = nullptr;
};

} // namespace sdgo::storage::extension
