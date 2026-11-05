#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "sdgo/all_maps.h"
#include "sdgo/sdgo_api.h"

namespace {

constexpr uint32_t kSnapshotVersion = 1;
constexpr uint8_t kAllMapSelection = 1;
constexpr size_t kTableCountOffset = 4;
constexpr size_t kTableHeadOffset = 0x10;
constexpr size_t kTableTailOffset = 0x14;
constexpr size_t kRecordNextOffset = 0xA970;

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;

using ChooseFn = uint32_t(__thiscall*)(void*, uint8_t*);
ChooseFn g_originalChoose = nullptr;
uint8_t* g_mapTable = nullptr;

std::mutex g_snapshotMutex;
SdgoAllMapsSnapshotV1 g_snapshot{};

bool CaptureEntries(std::vector<sdgo::all_maps::Entry>& entries) {
    __try {
        if (!g_mapTable) {
            return false;
        }

        const uint32_t count =
            *reinterpret_cast<uint32_t*>(g_mapTable + kTableCountOffset);
        if (!count || count > sdgo::all_maps::kMapDomainSize) {
            return false;
        }

        const auto tail =
            *reinterpret_cast<uint8_t**>(g_mapTable + kTableTailOffset);
        auto node =
            *reinterpret_cast<uint8_t**>(g_mapTable + kTableHeadOffset);
        for (uint32_t index = 0; index < count; ++index) {
            if (!node) {
                return false;
            }

            entries.push_back(sdgo::all_maps::ServerEntry(node));
            const auto next =
                *reinterpret_cast<uint8_t**>(node + kRecordNextOffset);
            if (index + 1 == count && (node != tail || next)) {
                return false;
            }
            node = next;
        }
        return !node;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t DrawUniform(const std::vector<uint32_t>& pool, uint32_t& error) {
    if (pool.empty()) {
        error = 1;
        return 0;
    }

    const uint32_t size = static_cast<uint32_t>(pool.size());
    const uint32_t threshold = (uint32_t(0) - size) % size;
    uint32_t value = 0;
    do {
        if (BCryptGenRandom(
                nullptr,
                reinterpret_cast<PUCHAR>(&value),
                sizeof(value),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            error = 2;
            return 0;
        }
    } while (value < threshold);
    return pool[value % size];
}

void RecordSnapshot(
    uint32_t mode,
    const std::vector<uint32_t>& pool,
    uint32_t chosen,
    uint32_t error) {
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_snapshot.size = sizeof(g_snapshot);
    g_snapshot.version = kSnapshotVersion;
    g_snapshot.pool_count = static_cast<uint32_t>(pool.size());
    ++g_snapshot.draws;
    g_snapshot.last_map = chosen;
    g_snapshot.mode = mode;
    g_snapshot.selection = kAllMapSelection;
    g_snapshot.error = error;
    std::memset(g_snapshot.maps, 0, sizeof(g_snapshot.maps));
    for (size_t index = 0; index < pool.size(); ++index) {
        g_snapshot.maps[index] = pool[index];
    }
}

uint32_t __fastcall Choose(void* self, void*, uint8_t* room) {
    if (!room ||
        room[9] != kAllMapSelection ||
        !sdgo::all_maps::PvpMode(room[5])) {
        return g_originalChoose(self, room);
    }

    std::vector<sdgo::all_maps::Entry> entries;
    entries.reserve(sdgo::all_maps::kMapDomainSize);
    const bool valid = CaptureEntries(entries);
    const auto pool =
        valid ? sdgo::all_maps::Pool(entries) : std::vector<uint32_t>{};

    uint32_t error = 0;
    const uint32_t chosen = DrawUniform(pool, error);
    RecordSnapshot(room[5], pool, chosen, error);

    g_api->Log(
        g_mod,
        error ? SDGO_LOG_ERROR : SDGO_LOG_INFO,
        "All Map mode=%u native_selection=1 eligible=%u chosen=%u error=%u",
        room[5],
        static_cast<uint32_t>(pool.size()),
        chosen,
        error);

    // MapNo 0 is itself a catalog row. An unavailable key makes the native
    // lookup reject start instead of silently loading that row on failure.
    return error ? UINT32_MAX : chosen;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoAllMapsGetSnapshotV1(
    SdgoAllMapsSnapshotV1* output) {
    if (!output ||
        output->size < sizeof(*output) ||
        output->version != kSnapshotVersion) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    *output = g_snapshot;
    return g_snapshot.version == kSnapshotVersion;
}

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    g_mapTable = reinterpret_cast<uint8_t*>(
        api->Symbol(mod, "all_maps.server_table"));
    return g_mapTable &&
        api->HookCode(
            mod,
            "all_maps.server_choose",
            reinterpret_cast<void*>(&Choose),
            reinterpret_cast<void**>(&g_originalChoose));
}
