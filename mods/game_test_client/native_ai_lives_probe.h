#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include "sdgo/game_test.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

// Read the original TAB life-cache input, not an added production observer.
// This also diagnoses older match_ai DLLs whose native NPC filter skips it.
class NativeAiLivesProbe {
    using FindObject = uint8_t*(__thiscall*)(void*, uint32_t);
    using PlayerSlot = uint8_t*(__thiscall*)(void*, int);
    using LifeCost = uint32_t(__cdecl*)(uint32_t);
    struct Value { int power, cost, lives; };
    struct Sample {
        uint32_t pilot = 0, object = 0;
        int power = 0, cost = 0, expected = 0, lastLives = -1;
        uint64_t changedAt = 0;
        bool verified = false, reduced = false;
    };
    bool enabled_ = false, scoped_ = false;
    uint32_t sequence_ = 0, expectedBots_ = 0, decreases_ = 0;
    void** manager_ = nullptr;
    FindObject find_ = nullptr;
    PlayerSlot slot_ = nullptr;
    LifeCost cost_ = nullptr;
    std::array<Sample, 12> samples_{};

    bool Read(uint32_t pilot, uint32_t object, Value& value) const {
        __try {
            void* manager = manager_ ? *manager_ : nullptr;
            uint8_t* unit = manager ? find_(manager, object) : nullptr;
            uint8_t* slot = manager ? slot_(manager, static_cast<int>(object - 1)) : nullptr;
            if (!unit || !slot || *reinterpret_cast<uint32_t*>(unit + 4) != object ||
                *reinterpret_cast<uint32_t*>(unit + 0x2B68) != pilot ||
                *reinterpret_cast<uint32_t*>(slot + 4) != object)
                return false;
            const uint8_t* record = *reinterpret_cast<const uint8_t**>(slot + 0x44);
            const uint8_t* data = *reinterpret_cast<const uint8_t**>(unit + 0x9C4);
            if (!record || *reinterpret_cast<const uint32_t*>(record) != pilot || !data)
                return false;
            const uint32_t serverCost = cost_(pilot);
            value.cost = serverCost ? static_cast<int>(serverCost) :
                static_cast<int>(*reinterpret_cast<const int16_t*>(data + 0x137));
            value.power = *reinterpret_cast<int32_t*>(slot + 0x48);
            // Original roster row 0x532E64 passes this exact value to its number renderer.
            value.lives = *reinterpret_cast<int32_t*>(unit + 0x2F9C);
            return value.cost > 0 && value.power >= 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

public:
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        const int setting = api->SettingInt(mod, "BattleAiLives", 0);
        if (setting != 0 && setting != 1) return false;
        enabled_ = setting != 0;
        if (!enabled_) return true;
        manager_ = static_cast<void**>(api->Symbol(mod, "game.unit_manager"));
        find_ = reinterpret_cast<FindObject>(api->Symbol(mod, "um.find_object"));
        slot_ = reinterpret_cast<PlayerSlot>(api->Symbol(mod, "um.player_slot"));
        cost_ = reinterpret_cast<LifeCost>(GetProcAddress(GetModuleHandleW(L"match_ai.dll"), "SdgoMatchAiGetLifeCostV1"));
        return manager_ && find_ && slot_ && cost_;
    }

    bool Observe(const SdgoGameTestContextV1* context,
                 const SdgoPvpAiClientSnapshotV1& battle,
                 const SdgoPvpAiClientScoreboardStatusV1& roster, uint32_t mode) {
        if (!enabled_) return true;
        scoped_ = mode == 3 || mode == 14;
        if (!scoped_) return true;
        if (sequence_ != battle.received_battle_start_count) {
            sequence_ = battle.received_battle_start_count;
            expectedBots_ = battle.bot_count; decreases_ = 0; samples_ = {};
        }
        if (!roster.panel_visible || roster.panel_state != 2) return true;
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < roster.bot_count && i < samples_.size(); ++i) {
            const auto& row = roster.bots[i];
            if (!row.render_count) continue;
            Value value{};
            if (!Read(row.pilot_id, row.object_id, value)) continue;
            const int expected = value.power / value.cost + (value.power % value.cost != 0);
            auto& sample = samples_[i];
            const bool fresh = sample.pilot != row.pilot_id || sample.object != row.object_id;
            if (fresh || sample.power != value.power || sample.cost != value.cost) {
                const bool reduced = !fresh && value.power < sample.power;
                if (fresh) sample = {};
                sample.pilot = row.pilot_id; sample.object = row.object_id;
                sample.power = value.power; sample.cost = value.cost;
                sample.expected = expected; sample.changedAt = now;
                sample.reduced = sample.reduced || reduced;
            }
            if (value.lives != expected) {
                // Packet consumption and the ordinary UI pass may straddle a frame.
                // This is an assertion grace period, never a write or repair timer.
                if (now - sample.changedAt <= 750) continue;
                char message[192];
                sprintf_s(message, "native TAB lives stale: mode=%u AI=%08X power=%d cost=%d expected=%d drawn_cache=%d",
                    mode, row.pilot_id, value.power, value.cost, expected, value.lives);
                context->Assert(0, message); return false;
            }
            if (!sample.verified || sample.lastLives != value.lives) {
                if (sample.verified && sample.reduced && value.lives < sample.lastLives) ++decreases_;
                char message[192];
                sprintf_s(message, "native TAB lives mode=%u AI=%08X team=%u power=%d cost=%d lives=%d previous=%d draws=%u",
                    mode, row.pilot_id, row.team, value.power, value.cost, value.lives, sample.lastLives, row.render_count);
                context->Observe(message);
            }
            sample.verified = true; sample.lastLives = value.lives;
        }
        return true;
    }

    void Finish(const SdgoGameTestContextV1* context) const {
        if (!enabled_ || !scoped_) return;
        uint32_t verified = 0;
        for (const auto& sample : samples_) if (sample.verified) ++verified;
        context->Assert(expectedBots_ > 0 && verified == expectedBots_,
            "every AI's native TAB life value matched its live server-cost and player-slot power");
        context->Assert(decreases_ > 0,
            "a genuine AI power loss decreased the life value consumed by the original TAB renderer");
    }
};
