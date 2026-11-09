// Exercise the production x86 DLL with a native calling-convention stand-in.
// This fixture does not replace separate in-client native acceptance.
#include <windows.h>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "sdgo/sdgo_api.h"
#include "sdgo/special_target_guard_observer.h"

namespace {

static_assert(sizeof(void*) == 4, "special-target fixture requires x86");

struct Vec3 {
    float x;
    float y;
    float z;
};

using Detour = Vec3* (__fastcall*)(void*, void*, Vec3*, bool);

constexpr std::size_t kCountOffset = 0x9C0;
constexpr std::uint32_t kInvalidPattern = 0xA6A5A4A3u;

std::array<std::uint8_t, 32> body = {
    0x55, 0x8B, 0xEC, 0x0F, 0x57, 0xC0, 0x83, 0xEC,
    0x0C, 0x80, 0x7D, 0x0C, 0x00, 0x53, 0x56, 0x57,
    0x8B, 0x7D, 0x08, 0x8B, 0xF1, 0xF3, 0x0F, 0x11,
    0x07, 0xF3, 0x0F, 0x11, 0x47, 0x04, 0xF3, 0x0F
};
std::array<std::uint8_t, 0xA00> owner{};
Detour detour = nullptr;
unsigned checks = 0;
unsigned hookCalls = 0;
unsigned originalCalls = 0;

struct ObservedCall {
    void* owner;
    Vec3* out;
    bool randomSelection;
    std::int32_t count;
} observed{};

void Check(bool value, const char* message) {
    ++checks;
    if (!value) {
        throw std::runtime_error(message);
    }
    std::printf("PASS %s\n", message);
}

void SetCount(std::int32_t count) {
    std::memcpy(owner.data() + kCountOffset, &count, sizeof(count));
}

std::int32_t ReadCount(const void* value) {
    std::int32_t count = 0;
    std::memcpy(
        &count,
        static_cast<const std::uint8_t*>(value) + kCountOffset,
        sizeof(count));
    return count;
}

bool PositiveZero(float value) {
    std::uint32_t bits = 1;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits == 0;
}

void __cdecl Log(SdgoMod*, int, const char*, ...) {}

void* __cdecl Symbol(SdgoMod*, const char* name) {
    return !std::strcmp(
        name,
        "special_target_guard.special_target_pos")
        ? body.data()
        : nullptr;
}

Vec3* __fastcall Original(
    void* self,
    void*,
    Vec3* out,
    bool randomSelection) {
    ++originalCalls;
    observed = {self, out, randomSelection, ReadCount(self)};
    out->x = 17.25f;
    out->y = -33.5f;
    out->z = 4096.0f;
    SetLastError(0x6BC4B0);
    return out;
}

int __cdecl Hook(
    SdgoMod*,
    const char* name,
    void* replacement,
    void** original) {
    if (std::strcmp(name, "special_target_guard.special_target_pos")) {
        return 0;
    }
    ++hookCalls;
    detour = reinterpret_cast<Detour>(replacement);
    *original = reinterpret_cast<void*>(&Original);
    return 1;
}

void ExpectForwarded(bool randomSelection, std::int32_t count) {
    SetCount(count);
    const auto beforeOwner = owner;
    const unsigned beforeCalls = originalCalls;
    Vec3 out{-1.0f, -2.0f, -3.0f};
    SetLastError(0x12345678);

    Vec3* result = detour(
        owner.data(),
        nullptr,
        &out,
        randomSelection);

    Check(result == &out, "forwarded call returns the native output pointer");
    Check(originalCalls == beforeCalls + 1,
          "forwarded call invokes the original exactly once");
    Check(observed.owner == owner.data() &&
              observed.out == &out &&
              observed.randomSelection == randomSelection &&
              observed.count == count,
          "this pointer, output pointer, flag, and signed count retain their ABI");
    Check(out.x == 17.25f && out.y == -33.5f && out.z == 4096.0f,
          "forwarded call retains the original Vec3 result");
    Check(GetLastError() == 0x6BC4B0,
          "forwarded call retains the original last-error result");
    Check(owner == beforeOwner,
          "forwarded call does not modify the target owner in the fixture");
}

void ExpectGuarded(std::int32_t count) {
    SetCount(count);
    const auto beforeOwner = owner;
    const unsigned beforeCalls = originalCalls;
    Vec3 out{1.0f, -2.0f, 3.0f};
    SetLastError(0x76543210);

    Vec3* result = detour(owner.data(), nullptr, &out, true);

    Check(result == &out,
          "invalid random count returns the native output pointer");
    Check(originalCalls == beforeCalls,
          "invalid random count never invokes the original");
    Check(PositiveZero(out.x) && PositiveZero(out.y) && PositiveZero(out.z),
          "invalid random count returns exact positive-zero XYZ");
    Check(owner == beforeOwner && ReadCount(owner.data()) == count,
          "invalid random count leaves the count and owner bytes untouched");
    Check(GetLastError() == 0x76543210,
          "invalid random count does not replace caller last-error state");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("pass the built special-target guard DLL");
        }

        HMODULE module = LoadLibraryW(argv[1]);
        Check(module != nullptr, "actual production DLL loads");
        auto entry = reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(module, "SdgoModLoad"));
        auto status = reinterpret_cast<SdgoSpecialTargetGuardGetStatusV1Fn>(
            GetProcAddress(module, SDGO_SPECIAL_TARGET_GUARD_STATUS_EXPORT));
        Check(entry && status, "loader and passive status export are present");

        SdgoSpecialTargetGuardStatusV1 inactive{
            sizeof(SdgoSpecialTargetGuardStatusV1), 1, 0, 0, 0};
        Check(!status(&inactive),
              "an unbound DLL does not advertise an active guard");

        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Log = &Log;
        api.Symbol = &Symbol;
        api.HookCode = &Hook;
        SdgoMod* mod = reinterpret_cast<SdgoMod*>(0x13572468);

        const std::uint8_t originalByte = body.back();
        body.back() ^= 1;
        Check(!entry(&api, mod) && hookCalls == 0,
              "a modified exact entry signature is refused before hooking");
        body.back() = originalByte;
        Check(entry(&api, mod) && hookCalls == 1 && detour,
              "the exact production signature binds one code hook");
        Check(!entry(&api, mod) && hookCalls == 1,
              "a second load cannot duplicate the code hook");

        for (std::int32_t count : {
                 INT_MIN,
                 static_cast<std::int32_t>(kInvalidPattern),
                 -1,
                 0,
                 1,
                 12,
                 13,
                 INT_MAX}) {
            ExpectForwarded(false, count);
        }

        for (std::int32_t count = 0; count <= 12; ++count) {
            ExpectForwarded(true, count);
        }

        ExpectGuarded(static_cast<std::int32_t>(kInvalidPattern));
        ExpectGuarded(INT_MIN);
        ExpectGuarded(13);
        ExpectGuarded(INT_MAX);

        SdgoSpecialTargetGuardStatusV1 snapshot{
            sizeof(SdgoSpecialTargetGuardStatusV1), 1, 0, 0, 0};
        Check(status(&snapshot) &&
                  snapshot.guarded_invalid_counts == 4 &&
                  snapshot.forwarded_calls == originalCalls &&
                  snapshot.last_invalid_count == INT_MAX,
              "passive status reports forwarded and guarded signed counts");
        snapshot.version = 2;
        Check(!status(&snapshot), "unknown observer ABI is refused");

        FreeLibrary(module);
        std::printf(
            "%u production DLL / special-target ABI checks passed; "
            "no game process started\n",
            checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
