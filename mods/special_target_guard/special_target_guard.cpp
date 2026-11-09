#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/special_target_guard_observer.h"

namespace {

struct Vec3 {
    float x;
    float y;
    float z;
};

static_assert(sizeof(void*) == 4, "the admitted special-target ABI is Win32");
static_assert(sizeof(Vec3) == 12, "the native special-target result is three floats");
static_assert(sizeof(bool) == 1, "the native random-selection flag is one byte");

using SpecialTargetPosFn = Vec3* (__thiscall*)(void*, Vec3*, bool);

constexpr std::size_t kTargetCountOffset = 0x9C0;
constexpr std::int32_t kTargetCapacity = 12;
constexpr std::uint8_t kExpectedEntry[] = {
    0x55, 0x8B, 0xEC, 0x0F, 0x57, 0xC0, 0x83, 0xEC,
    0x0C, 0x80, 0x7D, 0x0C, 0x00, 0x53, 0x56, 0x57,
    0x8B, 0x7D, 0x08, 0x8B, 0xF1, 0xF3, 0x0F, 0x11,
    0x07, 0xF3, 0x0F, 0x11, 0x47, 0x04, 0xF3, 0x0F
};

SpecialTargetPosFn g_original = nullptr;
volatile LONG g_guarded = 0;
volatile LONG g_forwarded = 0;
volatile LONG g_lastInvalidCount = 0;

std::int32_t TargetCount(const void* owner) {
    std::int32_t count = 0;
    std::memcpy(
        &count,
        static_cast<const std::uint8_t*>(owner) + kTargetCountOffset,
        sizeof(count));
    return count;
}

Vec3* __fastcall SpecialTargetPos(
    void* owner,
    void*,
    Vec3* out,
    bool randomSelection) {
    // The non-random path does not consult the target list count in the
    // original function, so preserve that behavior for every count value.
    if (!randomSelection) {
        Vec3* result = g_original(owner, out, randomSelection);
        InterlockedIncrement(&g_forwarded);
        return result;
    }

    const std::int32_t count = TargetCount(owner);
    if (count >= 0 && count <= kTargetCapacity) {
        Vec3* result = g_original(owner, out, randomSelection);
        InterlockedIncrement(&g_forwarded);
        return result;
    }

    // The native function initializes the result to positive zero before
    // target resolution. Return that same failure value without dividing by
    // or indexing with the corrupted count.
    out->x = 0.0f;
    out->y = 0.0f;
    out->z = 0.0f;
    InterlockedExchange(&g_lastInvalidCount, static_cast<LONG>(count));
    InterlockedIncrement(&g_guarded);
    return out;
}

bool ExpectedEntry(const void* body) {
    return body &&
        std::memcmp(body, kExpectedEntry, sizeof(kExpectedEntry)) == 0;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoSpecialTargetGuardGetStatusV1(
    SdgoSpecialTargetGuardStatusV1* out) {
    if (!out || out->size != sizeof(*out) || out->version != 1 || !g_original) {
        return 0;
    }
    *out = {
        sizeof(*out),
        1,
        static_cast<std::uint32_t>(
            InterlockedCompareExchange(&g_guarded, 0, 0)),
        static_cast<std::uint32_t>(
            InterlockedCompareExchange(&g_forwarded, 0, 0)),
        static_cast<std::int32_t>(
            InterlockedCompareExchange(&g_lastInvalidCount, 0, 0))
    };
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api ||
        api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookCode) + sizeof(api->HookCode) ||
        !api->Symbol ||
        !api->HookCode ||
        !api->Log ||
        g_original) {
        return 0;
    }

    const void* body = api->Symbol(mod, "special_target_guard.special_target_pos");
    if (!ExpectedEntry(body)) {
        api->Log(
            mod,
            SDGO_LOG_ERROR,
            "special-target function differs; no target-count guard installed");
        return 0;
    }

    if (!api->HookCode(
            mod,
            "special_target_guard.special_target_pos",
            reinterpret_cast<void*>(&SpecialTargetPos),
            reinterpret_cast<void**>(&g_original))) {
        return 0;
    }

    api->Log(
        mod,
        SDGO_LOG_INFO,
        "random special-target selection rejects counts outside the native 12-entry list");
    return 1;
}
