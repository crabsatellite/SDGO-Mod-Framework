#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "sdgo/pvp_ai_client_observer.h"

// Consumes the read-only match_ai snapshot and accumulates conservative
// evidence from genuine GAME_MAIN frames.  It does not attribute target HP
// loss to a particular AI: a decrease is only native combat evidence observed
// while at least one validated AI reported that same target object.
class BattleCombatActions
{
public:
    enum Result : uint32_t
    {
        kWaitingForEvidence = 0,
        kAccepted = 1,

        kInvalidExpectation = 0x100,
        kInvalidSnapshot = 0x101,
        kInactiveSnapshot = 0x102,
        kForeignBattle = 0x103,
        kLuaObserverUnavailable = 0x104,
        kLuaError = 0x105,
        kWaitingForValidatedBot = 0x106,
    };

    struct ExpectedBattle
    {
        uint32_t battle_start_sequence;
        uint32_t profile_pilot_id;
        uint16_t room_mode;
        const wchar_t* room_title;
    };

    struct Observation
    {
        uint32_t result;
        uint32_t battle_start_sequence;
        uint32_t native_app_state;
        uint32_t profile_pilot_id;
        uint16_t room_mode;
        uint16_t reserved;
        uint32_t validated_frame_count;
        uint32_t validated_bot_count;
        uint32_t unique_target_count;
        uint32_t target_acquisition_events;
        uint32_t moved_identity_count;
        uint32_t hp_decrease_events;
        uint64_t total_hp_decrease;
        uint32_t last_lua_error_count;
        uint32_t target_pilot_id;
        uint32_t target_source_object_id;
        uint32_t acquired_target_object_id;
        uint32_t moved_pilot_id;
        uint32_t moved_object_id;
        uint32_t moved_npc_id;
        uint32_t damaged_target_object_id;
        int32_t damaged_target_hp_before;
        int32_t damaged_target_hp_after;
        uint8_t current_frame_valid;
        uint8_t target_acquired;
        uint8_t movement_observed;
        uint8_t native_combat_evidence;
        uint8_t complete_evidence;
        uint8_t lua_error_latched;
        uint8_t reserved2[2];
    };

    void Reset()
    {
        expectedBound_ = false;
        expectedGeneration_ = 0;
        expectedProfile_ = 0;
        expectedMode_ = 0;
        expectedTitleLength_ = 0;
        std::memset(expectedTitle_, 0, sizeof(expectedTitle_));
        ClearEvidence();
    }

    Result Observe(
        const SdgoPvpAiClientSnapshotV1& snapshot,
        const ExpectedBattle& expected)
    {
        uint16_t expectedTitleLength = 0;
        if (!ValidExpectation(expected, &expectedTitleLength)) {
            Reset();
            return Publish(kInvalidExpectation, snapshot, false);
        }

        if (!ExpectedMatches(expected, expectedTitleLength)) {
            BindExpected(expected, expectedTitleLength);
            ClearEvidence();
        }

        if (snapshot.size < sizeof(snapshot) ||
            snapshot.version != SDGO_PVP_AI_CLIENT_OBSERVER_VERSION ||
            snapshot.bot_count > kMaximumBots ||
            snapshot.room_title_length >= kRoomTitleCapacity) {
            ClearEvidence();
            return Publish(kInvalidSnapshot, snapshot, false);
        }

        if (snapshot.received_battle_start_count !=
            expectedGeneration_) {
            ClearEvidence();
            return Publish(kForeignBattle, snapshot, false);
        }

        if (snapshot.native_app_state != kGameMainState) {
            ClearContinuity();
            return Publish(kInactiveSnapshot, snapshot, false);
        }

        if (!SnapshotBelongsToExpectedRoom(snapshot)) {
            ClearEvidence();
            return Publish(kForeignBattle, snapshot, false);
        }

        if (!snapshot.lua_error_observer_available) {
            ClearContinuity();
            return Publish(
                kLuaObserverUnavailable,
                snapshot,
                false);
        }

        state_.last_lua_error_count = snapshot.lua_error_count;
        if (snapshot.lua_error_count != 0)
            state_.lua_error_latched = 1;
        if (state_.lua_error_latched) {
            ClearContinuity();
            state_.complete_evidence = 0;
            return Publish(kLuaError, snapshot, false);
        }

        FrameBot frameBots[kMaximumBots] = {};
        size_t frameBotCount = 0;
        CollectFrameBots(
            snapshot,
            frameBots,
            &frameBotCount);

        FrameTarget frameTargets[kMaximumBots] = {};
        size_t frameTargetCount = 0;
        CollectFrameTargets(
            frameBots,
            frameBotCount,
            frameTargets,
            &frameTargetCount);

        state_.validated_frame_count =
            AddSaturated(state_.validated_frame_count, 1);
        state_.validated_bot_count =
            static_cast<uint32_t>(frameBotCount);
        state_.unique_target_count =
            CountUsableTargets(frameTargets, frameTargetCount);

        ObserveMovement(frameBots, frameBotCount);
        ObserveTargets(frameTargets, frameTargetCount);

        state_.complete_evidence =
            state_.target_acquired &&
            state_.movement_observed &&
            state_.native_combat_evidence &&
            !state_.lua_error_latched;

        if (!frameBotCount)
            return Publish(
                kWaitingForValidatedBot,
                snapshot,
                true);
        if (state_.complete_evidence)
            return Publish(kAccepted, snapshot, true);
        return Publish(kWaitingForEvidence, snapshot, true);
    }

    Observation State() const
    {
        return state_;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kWaitingForEvidence:
            return "waiting-for-evidence";
        case kAccepted:
            return "accepted";
        case kInvalidExpectation:
            return "invalid-expectation";
        case kInvalidSnapshot:
            return "invalid-snapshot";
        case kInactiveSnapshot:
            return "inactive-snapshot";
        case kForeignBattle:
            return "foreign-battle";
        case kLuaObserverUnavailable:
            return "lua-observer-unavailable";
        case kLuaError:
            return "lua-error";
        case kWaitingForValidatedBot:
            return "waiting-for-validated-bot";
        default:
            return "unknown";
        }
    }

private:
    static constexpr uint32_t kGameMainState = 22;
    static constexpr size_t kMaximumBots = 12;
    static constexpr size_t kRoomTitleCapacity = 32;
    static constexpr float kMinimumMovementSquared = 1.0f;

    struct BotKey
    {
        uint32_t pilot;
        uint32_t object;
        uint32_t npc;
    };

    struct FrameBot
    {
        BotKey key;
        uint32_t targetObject;
        int32_t targetHp;
        float x;
        float y;
        float z;
        uint8_t team;
        bool usable;
    };

    struct FrameTarget
    {
        uint32_t object;
        int32_t hp;
        uint32_t sourcePilot;
        uint32_t sourceObject;
        bool conflict;
    };

    struct MotionBaseline
    {
        BotKey key;
        float x;
        float y;
        float z;
    };

    struct TargetBaseline
    {
        uint32_t object;
        int32_t hp;
    };

    static bool SameKey(const BotKey& left, const BotKey& right)
    {
        return left.pilot == right.pilot &&
            left.object == right.object &&
            left.npc == right.npc;
    }

    static bool SameFrameBot(
        const FrameBot& left,
        const FrameBot& right)
    {
        return SameKey(left.key, right.key) &&
            left.targetObject == right.targetObject &&
            left.targetHp == right.targetHp &&
            left.x == right.x &&
            left.y == right.y &&
            left.z == right.z &&
            left.team == right.team;
    }

    static bool FinitePosition(
        const SdgoPvpAiClientBotSnapshotV1& bot)
    {
        return std::isfinite(bot.position_x) &&
            std::isfinite(bot.position_y) &&
            std::isfinite(bot.position_z);
    }

    static bool ValidBot(
        const SdgoPvpAiClientBotSnapshotV1& bot)
    {
        return bot.pilot_id &&
            (bot.pilot_id & 0x80000000u) &&
            bot.object_id &&
            bot.npc_id &&
            bot.team >= 1 &&
            bot.team <= 2 &&
            bot.ai_enabled &&
            bot.valid_registered_object &&
            bot.local_control &&
            !bot.remote_copy &&
            bot.master_npc &&
            bot.position_valid &&
            FinitePosition(bot);
    }

    static uint32_t AddSaturated(
        uint32_t value,
        uint32_t amount)
    {
        const uint32_t maximum = UINT32_MAX;
        return value > maximum - amount
            ? maximum
            : value + amount;
    }

    static uint64_t AddSaturated(
        uint64_t value,
        uint64_t amount)
    {
        const uint64_t maximum = UINT64_MAX;
        return value > maximum - amount
            ? maximum
            : value + amount;
    }

    static bool ValidExpectation(
        const ExpectedBattle& expected,
        uint16_t* titleLength)
    {
        if (!titleLength ||
            !expected.battle_start_sequence ||
            !expected.profile_pilot_id ||
            !expected.room_mode ||
            !expected.room_title)
            return false;
        size_t length = 0;
        while (length < kRoomTitleCapacity &&
               expected.room_title[length])
            ++length;
        if (!length || length >= kRoomTitleCapacity)
            return false;
        *titleLength = static_cast<uint16_t>(length);
        return true;
    }

    bool ExpectedMatches(
        const ExpectedBattle& expected,
        uint16_t titleLength) const
    {
        return expectedBound_ &&
            expectedGeneration_ ==
                expected.battle_start_sequence &&
            expectedProfile_ == expected.profile_pilot_id &&
            expectedMode_ == expected.room_mode &&
            expectedTitleLength_ == titleLength &&
            std::memcmp(
                expectedTitle_,
                expected.room_title,
                titleLength * sizeof(wchar_t)) == 0;
    }

    void BindExpected(
        const ExpectedBattle& expected,
        uint16_t titleLength)
    {
        expectedBound_ = true;
        expectedGeneration_ =
            expected.battle_start_sequence;
        expectedProfile_ = expected.profile_pilot_id;
        expectedMode_ = expected.room_mode;
        expectedTitleLength_ = titleLength;
        std::memset(expectedTitle_, 0, sizeof(expectedTitle_));
        std::memcpy(
            expectedTitle_,
            expected.room_title,
            titleLength * sizeof(wchar_t));
    }

    bool SnapshotBelongsToExpectedRoom(
        const SdgoPvpAiClientSnapshotV1& snapshot) const
    {
        return snapshot.profile_pilot_id == expectedProfile_ &&
            snapshot.room_mode == expectedMode_ &&
            snapshot.room_title_length ==
                expectedTitleLength_ &&
            snapshot.room_title[expectedTitleLength_] == L'\0' &&
            std::memcmp(
                snapshot.room_title,
                expectedTitle_,
                expectedTitleLength_ * sizeof(wchar_t)) == 0;
    }

    void CollectFrameBots(
        const SdgoPvpAiClientSnapshotV1& snapshot,
        FrameBot* output,
        size_t* outputCount) const
    {
        *outputCount = 0;
        for (uint32_t index = 0;
             index < snapshot.bot_count;
             ++index) {
            const auto& native = snapshot.bots[index];
            if (!ValidBot(native))
                continue;

            FrameBot candidate = {};
            candidate.key = {
                native.pilot_id,
                native.object_id,
                native.npc_id};
            candidate.targetObject =
                native.target_object_id != native.object_id &&
                    native.target_hp >= 0
                ? native.target_object_id
                : 0;
            candidate.targetHp = native.target_hp;
            candidate.x = native.position_x;
            candidate.y = native.position_y;
            candidate.z = native.position_z;
            candidate.team = native.team;
            candidate.usable = true;

            bool duplicate = false;
            bool conflict = false;
            for (size_t other = 0;
                 other < *outputCount;
                 ++other) {
                if (output[other].key.pilot !=
                        candidate.key.pilot &&
                    output[other].key.object !=
                        candidate.key.object)
                    continue;
                if (SameFrameBot(output[other], candidate)) {
                    duplicate = true;
                    break;
                }
                output[other].usable = false;
                conflict = true;
            }
            if (duplicate)
                continue;
            if (conflict)
                candidate.usable = false;
            output[(*outputCount)++] = candidate;
        }

        size_t write = 0;
        for (size_t read = 0;
             read < *outputCount;
             ++read) {
            if (output[read].usable)
                output[write++] = output[read];
        }
        *outputCount = write;
    }

    static void CollectFrameTargets(
        const FrameBot* bots,
        size_t botCount,
        FrameTarget* targets,
        size_t* targetCount)
    {
        *targetCount = 0;
        for (size_t botIndex = 0;
             botIndex < botCount;
             ++botIndex) {
            const auto& bot = bots[botIndex];
            if (!bot.targetObject)
                continue;
            size_t targetIndex = 0;
            for (; targetIndex < *targetCount; ++targetIndex)
                if (targets[targetIndex].object ==
                    bot.targetObject)
                    break;
            if (targetIndex == *targetCount) {
                targets[targetIndex] = {
                    bot.targetObject,
                    bot.targetHp,
                    bot.key.pilot,
                    bot.key.object,
                    false};
                ++*targetCount;
            } else if (
                targets[targetIndex].hp != bot.targetHp) {
                targets[targetIndex].conflict = true;
            }
        }
    }

    static uint32_t CountUsableTargets(
        const FrameTarget* targets,
        size_t targetCount)
    {
        uint32_t count = 0;
        for (size_t index = 0;
             index < targetCount;
             ++index)
            if (!targets[index].conflict)
                ++count;
        return count;
    }

    void ObserveMovement(
        const FrameBot* bots,
        size_t botCount)
    {
        MotionBaseline next[kMaximumBots] = {};
        size_t nextCount = 0;

        for (size_t botIndex = 0;
             botIndex < botCount;
             ++botIndex) {
            const auto& bot = bots[botIndex];
            const MotionBaseline* previous = nullptr;
            for (size_t baselineIndex = 0;
                 baselineIndex < motionBaselineCount_;
                 ++baselineIndex) {
                if (SameKey(
                        motionBaselines_[baselineIndex].key,
                        bot.key)) {
                    previous = &motionBaselines_[baselineIndex];
                    break;
                }
            }

            MotionBaseline baseline = {
                bot.key,
                bot.x,
                bot.y,
                bot.z};
            if (previous) {
                baseline = *previous;
                const float dx = bot.x - previous->x;
                const float dy = bot.y - previous->y;
                const float dz = bot.z - previous->z;
                if (dx * dx + dy * dy + dz * dz >
                    kMinimumMovementSquared)
                    RecordMovement(bot.key);
            }
            next[nextCount++] = baseline;
        }

        std::memcpy(
            motionBaselines_,
            next,
            nextCount * sizeof(next[0]));
        motionBaselineCount_ = nextCount;
    }

    void RecordMovement(const BotKey& key)
    {
        for (size_t index = 0;
             index < movedKeyCount_;
             ++index)
            if (SameKey(movedKeys_[index], key))
                return;
        if (movedKeyCount_ < kMaximumBots)
            movedKeys_[movedKeyCount_++] = key;
        state_.moved_identity_count =
            AddSaturated(state_.moved_identity_count, 1);
        state_.movement_observed = 1;
        if (!state_.moved_pilot_id) {
            state_.moved_pilot_id = key.pilot;
            state_.moved_object_id = key.object;
            state_.moved_npc_id = key.npc;
        }
    }

    void ObserveTargets(
        const FrameTarget* targets,
        size_t targetCount)
    {
        TargetBaseline next[kMaximumBots] = {};
        size_t nextCount = 0;

        for (size_t targetIndex = 0;
             targetIndex < targetCount;
             ++targetIndex) {
            const auto& target = targets[targetIndex];
            if (target.conflict)
                continue;

            const TargetBaseline* previous = nullptr;
            for (size_t baselineIndex = 0;
                 baselineIndex < targetBaselineCount_;
                 ++baselineIndex) {
                if (targetBaselines_[baselineIndex].object ==
                    target.object) {
                    previous = &targetBaselines_[baselineIndex];
                    break;
                }
            }

            if (!previous) {
                state_.target_acquisition_events =
                    AddSaturated(
                        state_.target_acquisition_events,
                        1);
                state_.target_acquired = 1;
                if (!state_.acquired_target_object_id) {
                    state_.target_pilot_id =
                        target.sourcePilot;
                    state_.target_source_object_id =
                        target.sourceObject;
                    state_.acquired_target_object_id =
                        target.object;
                }
            } else if (target.hp < previous->hp) {
                const uint64_t decrease =
                    static_cast<uint64_t>(
                        static_cast<int64_t>(previous->hp) -
                        static_cast<int64_t>(target.hp));
                state_.hp_decrease_events =
                    AddSaturated(
                        state_.hp_decrease_events,
                        1);
                state_.total_hp_decrease =
                    AddSaturated(
                        state_.total_hp_decrease,
                        decrease);
                state_.native_combat_evidence = 1;
                if (!state_.damaged_target_object_id) {
                    state_.damaged_target_object_id =
                        target.object;
                    state_.damaged_target_hp_before =
                        previous->hp;
                    state_.damaged_target_hp_after =
                        target.hp;
                }
            }

            next[nextCount++] = {
                target.object,
                target.hp};
        }

        std::memcpy(
            targetBaselines_,
            next,
            nextCount * sizeof(next[0]));
        targetBaselineCount_ = nextCount;
    }

    Result Publish(
        Result result,
        const SdgoPvpAiClientSnapshotV1& snapshot,
        bool frameValid)
    {
        state_.result = result;
        state_.battle_start_sequence =
            snapshot.received_battle_start_count;
        state_.native_app_state = snapshot.native_app_state;
        state_.profile_pilot_id = snapshot.profile_pilot_id;
        state_.room_mode = snapshot.room_mode;
        state_.current_frame_valid = frameValid ? 1 : 0;
        return result;
    }

    void ClearContinuity()
    {
        motionBaselineCount_ = 0;
        targetBaselineCount_ = 0;
        state_.validated_bot_count = 0;
        state_.unique_target_count = 0;
        state_.current_frame_valid = 0;
    }

    void ClearEvidence()
    {
        state_ = {};
        motionBaselineCount_ = 0;
        targetBaselineCount_ = 0;
        movedKeyCount_ = 0;
        std::memset(
            motionBaselines_,
            0,
            sizeof(motionBaselines_));
        std::memset(
            targetBaselines_,
            0,
            sizeof(targetBaselines_));
        std::memset(movedKeys_, 0, sizeof(movedKeys_));
    }

    bool expectedBound_ = false;
    uint32_t expectedGeneration_ = 0;
    uint32_t expectedProfile_ = 0;
    uint16_t expectedMode_ = 0;
    uint16_t expectedTitleLength_ = 0;
    wchar_t expectedTitle_[kRoomTitleCapacity] = {};

    Observation state_ = {};
    MotionBaseline motionBaselines_[kMaximumBots] = {};
    size_t motionBaselineCount_ = 0;
    TargetBaseline targetBaselines_[kMaximumBots] = {};
    size_t targetBaselineCount_ = 0;
    BotKey movedKeys_[kMaximumBots] = {};
    size_t movedKeyCount_ = 0;
};
