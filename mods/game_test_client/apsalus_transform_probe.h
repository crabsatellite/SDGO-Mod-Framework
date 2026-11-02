#pragma once
#include <cmath>
#include "scoreboard_actions.h"

// Opt-in native GameTest scenario, sealed in the copied observer manifest.
// Uses the normal, owned unit selected by
// --play-unit-uid and the original transform admission/request business.
// No keyboard, profile, inventory, model, FSM or server state is fabricated.
namespace apsalus_probe {
inline int Poll(const SdgoGameTestContextV1* context, const SdgoApi* api, SdgoMod* mod, ScoreboardActions& input) {
    static const bool enabled = api->SettingInt(mod, "ApsalusRoundtrip", 0) != 0;
    if (!enabled) return SDGO_GAME_TEST_PASS;
    using Find = unsigned char*(__thiscall*)(void*, unsigned);
    using Request = unsigned(__thiscall*)(void*);
    // Both original input consumers take the real InputSlotSystem on stack
    // and return with RET 4; their CUnit owner is ECX.
    using InputBusiness = void(__thiscall*)(unsigned char*, void*);
    using NativeTick = void(__thiscall*)(unsigned char*, void*);
    static auto manager = reinterpret_cast<void**>(api->Symbol(mod, "game.unit_manager"));
    static auto find = reinterpret_cast<Find>(api->Symbol(mod, "um.find_object"));
    static auto request = reinterpret_cast<Request>(api->Symbol(mod, "test.apsalus_transform_request"));
    static auto moveInput = reinterpret_cast<InputBusiness>(api->Symbol(mod, "test.apsalus_move_input"));
    static auto fireInput = reinterpret_cast<InputBusiness>(api->Symbol(mod, "test.apsalus_primary_input"));
    static unsigned issued = 0, completed = 0, initialId = 0, lastMode = ~0u, lastId = 0;
    static uint64_t issuedAt = 0;
    static unsigned controlsVerified = 0;
    static uint64_t controlAt = 0, settleAt = 0;
    static float baseline[3] = {};
    static bool moved = false, attacked = false;
    static uint32_t moveQueries = 0, fireQueries = 0;
    if (!manager || !find || !request || !moveInput || !fireInput || !*manager) {
        context->Assert(0, "Apsalus probe requires verified native request and unit-manager bindings");
        return SDGO_GAME_TEST_FAIL;
    }
    auto* unit = find(*manager, 1);
    if (!unit) return SDGO_GAME_TEST_WAIT;
    // Native unit command 0x1d reads unit[0x272] before calling 006D8B90.
    // unit+9CC is the attack FSM; the movement/transform FSM is unit+9C8.
    auto* move = *reinterpret_cast<unsigned char**>(unit + 0x9c8);
    auto* spec = *reinterpret_cast<unsigned char**>(unit + 0x9c4);
    if (!move || !spec || *reinterpret_cast<unsigned char**>(move + 0x21c) != unit) {
        static bool reported=false;
        if(!reported) {
            char message[160];sprintf_s(message,"apsalus native object readiness: unit=%p move=%p spec=%p owner=%p",unit,move,spec,move?*reinterpret_cast<void**>(move+0x21c):nullptr);
            context->Observe(message);reported=true;
        }
        if(context->elapsed_ms<10000)return SDGO_GAME_TEST_WAIT;
        context->Assert(0, "Apsalus probe observed the actual local movement-FSM owner");
        return SDGO_GAME_TEST_FAIL;
    }
    const auto id = *reinterpret_cast<unsigned*>(spec);
    const auto mode = *reinterpret_cast<unsigned*>(move + 0x2c0);
    const auto handle = *reinterpret_cast<unsigned*>(move + 0x1ac);
    if (id != 24787 && id != 24788) {
        context->Assert(0, "Apsalus probe must start with a genuinely owned Apsalus III");
        return SDGO_GAME_TEST_FAIL;
    }
    if (!initialId) initialId = id;
    if (lastMode != mode || lastId != id) {
        char message[192];
        sprintf_s(message, "apsalus native probe unit=%u bank=%u mode=%u handle=%u issued=%u completed=%u", id, unit[0xe34], mode, handle, issued, completed);
        context->Observe(message); lastMode = mode; lastId = id;
    }
    if (completed > controlsVerified) {
        // 00670E10 consumes held actions 0..3 as native movement commands;
        // 00672FD0 consumes action 9 in MouseLButtonControl. Supply mapped
        // inputs only: actual control, motion, attack and rendering run normally.
        auto* node = *reinterpret_cast<unsigned char**>(unit + 8);
        auto* attack = *reinterpret_cast<unsigned char**>(unit + 0x9cc);
        void* nativeInput = input.NativeInput();
        if (!node || !attack || !nativeInput) return SDGO_GAME_TEST_WAIT;
        const auto* position = reinterpret_cast<const float*>(node + 0x88);
        if (!controlAt) {
            for (unsigned i = 0; i < 3; ++i) baseline[i] = position[i];
            moved = attacked = false; controlAt = context->elapsed_ms;
            moveQueries = input.GameplayQueries(0); fireQueries = input.GameplayQueries(9);
        }
        const uint64_t elapsed = context->elapsed_ms - controlAt;
        input.HoldGameplay(0, elapsed < 1500);
        input.HoldGameplay(9, elapsed < 1500);
        // A hidden client does not receive foreground keyboard polling. Drive
        // the same native keyboard/left-button consumers and the same FSM
        // dispatch used by CUnit's native command-0x1d path, on its real control
        // context (unit[0x370]). No position, state, eligibility or ammo write.
        moveInput(unit, nativeInput); fireInput(unit, nativeInput);
        auto dispatchMove = reinterpret_cast<NativeTick>((*reinterpret_cast<void***>(move))[1]);
        auto dispatchAttack = reinterpret_cast<NativeTick>((*reinterpret_cast<void***>(attack))[1]);
        dispatchMove(move, unit + 0xdc0);
        dispatchAttack(attack, unit + 0xdc0);
        const float dx=position[0]-baseline[0],dy=position[1]-baseline[1],dz=position[2]-baseline[2];
        moved = moved || (std::isfinite(dx+dy+dz) && dx*dx+dy*dy+dz*dz > 1.0f && input.GameplayQueries(0)>moveQueries);
        // The active native FSM state is at +4, with Wait at +20. Observe a
        // real attack state entered while the original fire consumer runs.
        const auto state = reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(attack + 4));
        const auto first = reinterpret_cast<uintptr_t>(attack + 0x20);
        attacked = attacked || (state > first && state < first+0x200 && input.GameplayQueries(9)>fireQueries);
        if (elapsed < 2500) return SDGO_GAME_TEST_WAIT;
        input.HoldGameplay(0, false); input.HoldGameplay(9, false);
        if (!moved || !attacked) {
            char message[192];sprintf_s(message,"Apsalus form %u native controls: moved=%u attack_state=%u move_queries=%u fire_queries=%u",id,moved,attacked,input.GameplayQueries(0)-moveQueries,input.GameplayQueries(9)-fireQueries);
            context->Assert(0,message);return SDGO_GAME_TEST_FAIL;
        }
        context->Assert(1,"after the genuine transformation, native mapped movement changed the real scene position and primary fire entered its real attack state");
        ++controlsVerified; controlAt=0; settleAt=context->elapsed_ms;
        return SDGO_GAME_TEST_WAIT;
    }
    if (issued > completed) {
        if (mode == 0 && id != (completed % 2 == 0 ? initialId : (initialId == 24787 ? 24788u : 24787u))) {
            ++completed;
            context->Assert(1, "original transform request committed its other form and released movement mode");
        } else if (context->elapsed_ms - issuedAt > 3000) {
            context->Assert(0, "original Apsalus transformation did not release after three seconds; retain unit/bank/mode/handle observations");
            return SDGO_GAME_TEST_FAIL;
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if (completed == 2 && controlsVerified == 2) return SDGO_GAME_TEST_PASS;
    if (context->elapsed_ms > 500 && context->elapsed_ms-settleAt > 500 && mode == 0) {
        const unsigned result = request(move);
        ++issued; issuedAt = context->elapsed_ms;
        // The network path returns zero after dispatch. Only the subsequent
        // form commit and release prove acceptance; never resend each frame.
        char message[192];
        sprintf_s(message,"apsalus probe: original transform request issued, immediate return=%u; await real form commit and release",result);
        context->Observe(message);
    }
    return SDGO_GAME_TEST_WAIT;
}
}
