#pragma once
#include <windows.h>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "sdgo/game_test.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/special_target_guard_observer.h"

// Development-only acceptance for the production hook's invalid-count path.
// It calls the patched shipped entry with a task-owned synthetic FSM and never
// enters a valid path that could resolve a target, actor, world or inventory.
class SpecialTargetGuardActions {
    struct Vec3 {
        float x;
        float y;
        float z;
    };

    struct OutputFrame {
        std::uint32_t before;
        Vec3 value;
        std::uint32_t after;
    };

    using SpecialTargetPos = Vec3* (__thiscall*)(void*, Vec3*, bool);

    static constexpr std::size_t kOwnerSize = 0xEF8;
    static constexpr std::size_t kCountOffset = 0x9C0;
    static constexpr std::uint32_t kInvalidPattern = 0xA6A5A4A3u;
    static constexpr std::uint32_t kBeforeSentinel = 0x13579BDFu;
    static constexpr std::uint32_t kAfterSentinel = 0x2468ACE0u;

    static_assert(sizeof(void*) == 4, "special-target native acceptance requires x86");
    static_assert(sizeof(Vec3) == 12, "special-target output is three floats");
    static_assert(kCountOffset + sizeof(std::int32_t) <= kOwnerSize,
                  "synthetic owner contains the native count field");

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    SpecialTargetPos position_ = nullptr;
    SdgoSpecialTargetGuardGetStatusV1Fn status_ = nullptr;
    bool ran_ = false;
    int result_ = SDGO_GAME_TEST_FAIL;

    static bool PositiveZero(float value) {
        std::uint32_t bits = 1;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits == 0;
    }

    bool Status(SdgoSpecialTargetGuardStatusV1& out) const {
        out = {};
        out.size = sizeof(out);
        out.version = 1;
        return status_ && status_(&out) != 0;
    }

    static void SetCount(
        std::array<std::uint8_t, kOwnerSize>& owner,
        std::int32_t count) {
        std::memcpy(owner.data() + kCountOffset, &count, sizeof(count));
    }

    static std::int32_t ReadCount(
        const std::array<std::uint8_t, kOwnerSize>& owner) {
        std::int32_t count = 0;
        std::memcpy(&count, owner.data() + kCountOffset, sizeof(count));
        return count;
    }

    int Fail(
        const SdgoGameTestContextV1* context,
        const char* message,
        std::int32_t count,
        unsigned index,
        const SdgoSpecialTargetGuardStatusV1* before = nullptr,
        const SdgoSpecialTargetGuardStatusV1* after = nullptr) {
        char details[384]{};
        if (before && after) {
            sprintf_s(
                details,
                "synthetic special-target ABI failure: case=%u count=%ld "
                "guard=%u->%u forwarded=%u->%u last=%ld",
                index,
                static_cast<long>(count),
                before->guarded_invalid_counts,
                after->guarded_invalid_counts,
                before->forwarded_calls,
                after->forwarded_calls,
                static_cast<long>(after->last_invalid_count));
        } else {
            sprintf_s(
                details,
                "synthetic special-target ABI failure: case=%u count=%ld",
                index,
                static_cast<long>(count));
        }
        context->Observe(details);
        context->Assert(0, message);
        result_ = SDGO_GAME_TEST_FAIL;
        return result_;
    }

public:
    bool Bind(const SdgoApi* api, SdgoMod* mod) {
        api_ = api;
        mod_ = mod;
        if (!api_ || !mod_ || !api_->Symbol) {
            return false;
        }

        position_ = reinterpret_cast<SpecialTargetPos>(
            api_->Symbol(mod_, "special_target_guard.special_target_pos"));
        HMODULE guard = GetModuleHandleW(L"special_target_guard.dll");
        status_ = guard
            ? reinterpret_cast<SdgoSpecialTargetGuardGetStatusV1Fn>(
                  GetProcAddress(
                      guard,
                      SDGO_SPECIAL_TARGET_GUARD_STATUS_EXPORT))
            : nullptr;

        SdgoSpecialTargetGuardStatusV1 active{};
        return position_ && Status(active);
    }

    int Poll(const SdgoGameTestContextV1* context) {
        if (ran_) {
            return result_;
        }
        ran_ = true;

        context->Observe(
            "starting synthetic native ABI/error-path proof for the hooked "
            "special-target consumer; this does not reproduce the original "
            "count producer and is not real-battle proof");

        if (!position_ || !status_) {
            return Fail(
                context,
                "production special-target hook and observer are bound",
                0,
                0);
        }

        SdgoSpecialTargetGuardStatusV1 baseline{};
        if (!Status(baseline)) {
            return Fail(
                context,
                "production special-target observer is active",
                0,
                0);
        }

        std::array<std::uint8_t, kOwnerSize> owner{};
        for (std::size_t index = 0; index < owner.size(); ++index) {
            owner[index] = static_cast<std::uint8_t>(
                (index * 37u + 0x5Bu) & 0xFFu);
        }

        const std::array<std::int32_t, 5> counts = {
            static_cast<std::int32_t>(kInvalidPattern),
            INT_MIN,
            -1,
            13,
            INT_MAX
        };

        for (unsigned index = 0; index < counts.size(); ++index) {
            const std::int32_t count = counts[index];
            SetCount(owner, count);
            const auto ownerBefore = owner;
            OutputFrame output{
                kBeforeSentinel,
                {1.0f, -2.0f, 3.0f},
                kAfterSentinel
            };
            const DWORD error = 0x53000000u + index;
            SetLastError(error);

            Vec3* returned = position_(owner.data(), &output.value, true);

            if (returned != &output.value) {
                return Fail(
                    context,
                    "invalid count returns the native output pointer",
                    count,
                    index);
            }
            if (!PositiveZero(output.value.x) ||
                !PositiveZero(output.value.y) ||
                !PositiveZero(output.value.z)) {
                return Fail(
                    context,
                    "invalid count returns exact positive-zero XYZ",
                    count,
                    index);
            }
            if (output.before != kBeforeSentinel ||
                output.after != kAfterSentinel) {
                return Fail(
                    context,
                    "invalid count preserves output-adjacent sentinels",
                    count,
                    index);
            }
            if (std::memcmp(
                    owner.data(),
                    ownerBefore.data(),
                    owner.size()) != 0 ||
                ReadCount(owner) != count) {
                return Fail(
                    context,
                    "invalid count preserves every synthetic owner byte",
                    count,
                    index);
            }
            if (GetLastError() != error) {
                return Fail(
                    context,
                    "invalid count preserves last-error state",
                    count,
                    index);
            }

            SdgoSpecialTargetGuardStatusV1 observed{};
            if (!Status(observed)) {
                return Fail(
                    context,
                    "production special-target observer remains readable",
                    count,
                    index);
            }
            if (observed.guarded_invalid_counts -
                        baseline.guarded_invalid_counts !=
                    index + 1 ||
                observed.forwarded_calls != baseline.forwarded_calls ||
                observed.last_invalid_count != count) {
                return Fail(
                    context,
                    "each synthetic invalid call advances only the production guard",
                    count,
                    index,
                    &baseline,
                    &observed);
            }
        }

        SdgoSpecialTargetGuardStatusV1 final{};
        if (!Status(final) ||
            final.guarded_invalid_counts -
                    baseline.guarded_invalid_counts != 5 ||
            final.forwarded_calls != baseline.forwarded_calls ||
            final.last_invalid_count != INT_MAX) {
            return Fail(
                context,
                "five invalid counts produce the exact observer delta",
                INT_MAX,
                5,
                &baseline,
                &final);
        }

        char receipt[320]{};
        sprintf_s(
            receipt,
            "synthetic native ABI/error-path proof passed: guard=%u->%u "
            "forwarded=%u last=%ld; no producer, actor, world, inventory, "
            "or real-battle mutation was exercised",
            baseline.guarded_invalid_counts,
            final.guarded_invalid_counts,
            final.forwarded_calls,
            static_cast<long>(final.last_invalid_count));
        context->Observe(receipt);
        context->Assert(
            1,
            "hooked shipped special-target consumer rejects five signed "
            "out-of-range counts with exact native zero results");
        result_ = SDGO_GAME_TEST_PASS;
        return result_;
    }
};
