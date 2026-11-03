#pragma once
#include <cstdint>

struct SdgoWaitroomWindowSnapshotV1 {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t process_id;
    std::uint32_t guarded_icons;
    std::uint32_t forwarded_messages;
    std::uint32_t last_guard_thread;
};
using SdgoWaitroomWindowSnapshotFn = int (__cdecl*)(SdgoWaitroomWindowSnapshotV1*);
#define SDGO_WAITROOM_WINDOW_SNAPSHOT_EXPORT "SdgoWaitroomWindowSnapshotV1"
