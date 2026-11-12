#pragma once
#include <cmath>
#include "scoreboard_actions.h"
#include "unit_transform_path.h"
#include "native_transform_packets.h"

// Opt-in native GameTest scenario, sealed in the copied observer manifest.
// Uses the normal, owned unit selected by
// --play-unit-uid and the original transform admission/request business.
// No keyboard, profile, inventory, model, FSM or server state is fabricated.
namespace apsalus_probe {
using LocalFind = unsigned char*(__thiscall*)(void*, unsigned);
inline unsigned char* LocalPlayer(void* manager,LocalFind find) {
    __try {
        auto* unit=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(manager)+0x88);
        return unit&&unit[0x31e9]&&find(manager,*reinterpret_cast<unsigned*>(unit+4))==unit?unit:nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
inline int Poll(const SdgoGameTestContextV1* context, const SdgoApi* api, SdgoMod* mod, ScoreboardActions& input) {
    static const bool legacy = api->SettingInt(mod, "ApsalusRoundtrip", 0) != 0;
    static const bool directed = api->SettingInt(mod, "UnitDirectedTransforms", 0) != 0;
    static const bool enabled = legacy || directed || api->SettingInt(mod, "UnitTransformRoundtrip", 0) != 0;
    if (!enabled) return SDGO_GAME_TEST_PASS;
    packetObserver.Observe(context);
    using Find = unsigned char*(__thiscall*)(void*, unsigned);
    using Request = unsigned char(__thiscall*)(void*);
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
    static unsigned issuedBank = 0;
    static sdgo_test::transforms::Path path{};
    static uint64_t issuedAt = 0;
    static uint64_t admissionAt = 0, lastRequestAt = 0, deniedObservationAt = 0;
    static unsigned controlsVerified = 0;
    static uint64_t controlAt = 0, settleAt = 0;
    static float baseline[3] = {};
    static bool moved = false, attacked = false;
    static uint32_t moveQueries = 0, fireQueries = 0;
    if (!manager || !find || !request || !moveInput || !fireInput || !*manager) {
        context->Assert(0, "Apsalus probe requires verified native request and unit-manager bindings");
        return SDGO_GAME_TEST_FAIL;
    }
    auto* unit = LocalPlayer(*manager,find);
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
    if (legacy && id != 24787 && id != 24788) {
        context->Assert(0, "Apsalus probe must start with a genuinely owned Apsalus III");
        return SDGO_GAME_TEST_FAIL;
    }
    if (!initialId) {
        using Grid=void*(__cdecl*)();using GridFind=bool(__thiscall*)(void*,const unsigned char**,unsigned,int,char);
        const auto grid=reinterpret_cast<Grid>(api->Symbol(mod,"grid.data"));
        const auto lookup=reinterpret_cast<GridFind>(api->Symbol(mod,"grid.find"));
        if(!grid||!lookup) {
            context->Assert(0,"selected actor has an actual native unit definition");return SDGO_GAME_TEST_FAIL;
        }
        const int configured=directed?api->SettingInt(mod,"UnitTransformSteps",3):2;
        unsigned initialTarget=0;std::memcpy(&initialTarget,spec+316,4);
        const bool self=initialTarget==id&&!directed&&!legacy;
        const char* error=nullptr;
        if(configured<1||configured>static_cast<int>(sdgo_test::transforms::MaxSteps)||
            !sdgo_test::transforms::Build(id,static_cast<unsigned>(configured),!directed||legacy,
                [&](unsigned identity){const unsigned char* raw=nullptr;
                    return lookup(grid(),&raw,identity,2,0)&&raw?sdgo_test::transforms::Record{raw,4976}:sdgo_test::transforms::Record{};
                },path,error,self)){
            context->Assert(0,error?error:"native transform path step count is outside 1..16");return SDGO_GAME_TEST_FAIL;
        }
        initialId=id;
        for(unsigned i=0;i<path.steps;++i){char message[160];
            sprintf_s(message,"native transform path step=%u source=%u target=%u field=316",i+1,path.ids[i],path.ids[i+1]);
            context->Observe(message);
        }
    }
    if(!path.contains(id)){
        context->Assert(0,"actor left the selected native record-derived transform path");return SDGO_GAME_TEST_FAIL;
    }
    if (lastMode != mode || lastId != id) {
        using Grid=void*(__cdecl*)();using GridFind=bool(__thiscall*)(void*,const unsigned char**,unsigned,int,char);
        const auto grid=reinterpret_cast<Grid>(api->Symbol(mod,"grid.data"));
        const auto lookup=reinterpret_cast<GridFind>(api->Symbol(mod,"grid.find"));
        const unsigned char* declared=nullptr;unsigned actorTarget=0,declaredTarget=0;
        std::memcpy(&actorTarget,spec+316,4);
        if(grid&&lookup&&lookup(grid(),&declared,id,2,0)&&declared)std::memcpy(&declaredTarget,declared+316,4);
        char message[288];
        sprintf_s(message, "apsalus native probe unit=%u bank=%u mode=%u handle=%u issued=%u completed=%u actor_target=%u grid_target=%u actor_spec=%p grid_spec=%p", id, unit[0xe34], mode, handle, issued, completed,actorTarget,declaredTarget,spec,declared);
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
        if (mode == 0 && completed<path.steps && id == path.ids[completed+1] &&
            (path.ids[completed]!=path.ids[completed+1] || unit[0xe34]!=issuedBank)) {
            ++completed;
            char message[192];sprintf_s(message,"native transform committed step=%u expected=%u actual=%u mode=%u bank=%u",completed,path.ids[completed],id,mode,unit[0xe34]);
            context->Observe(message);
            context->Assert(1, "original transform request committed its other form and released movement mode");
        } else if (context->elapsed_ms - issuedAt > 3000) {
            char message[224];sprintf_s(message,"admitted native transform failed its record-derived commit after three seconds: expected=%u actual=%u mode=%u bank=%u",path.ids[completed+1],id,mode,unit[0xe34]);
            context->Assert(0,message);
            return SDGO_GAME_TEST_FAIL;
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if (completed == path.steps && controlsVerified == path.steps) return SDGO_GAME_TEST_PASS;
    if (context->elapsed_ms > 500 && context->elapsed_ms-settleAt > 500 && mode == 0) {
        if(lastRequestAt&&context->elapsed_ms-lastRequestAt<500)return SDGO_GAME_TEST_WAIT;
        lastRequestAt=context->elapsed_ms;
        if(!admissionAt)admissionAt=context->elapsed_ms;
        const unsigned previousBank=unit[0xe34];
        const unsigned result = request(move);
        // AL is the native admission result. A denied local request has not
        // started a transformation; wait for normal reload/cooldown/eligibility
        // instead of treating that denial as an unfinished committed transform.
        if(!result){
            if(!deniedObservationAt||context->elapsed_ms-deniedObservationAt>=2000){
                const auto* action=*reinterpret_cast<const unsigned char**>(move+0x224);
                const auto* definition=*reinterpret_cast<const unsigned char**>(unit+0xab0);
                const unsigned condition=definition?definition[0x140]:0;
                short ammo=-1,maximum=-1;
                if(action&&(condition==2||condition==5)){
                    const unsigned offset=condition==2?0x3dc:0x6fc;
                    const auto* weapon=*reinterpret_cast<const unsigned char*const*>(action+offset+12);
                    std::memcpy(&ammo,action+offset,2);if(weapon)std::memcpy(&maximum,weapon+0x7c,2);
                }
                char message[224];sprintf_s(message,"native transform admission denied unit=%u condition=%u action_block=%u ammo=%d max=%d waited_ms=%llu",id,condition,action?action[0x9b2]:0,ammo,maximum,static_cast<unsigned long long>(context->elapsed_ms-admissionAt));
                context->Observe(message);deniedObservationAt=context->elapsed_ms;
            }
            if(context->elapsed_ms-admissionAt>20000){
                context->Assert(0,"native transform admission stayed denied after normal reload/cooldown waiting; eligibility was not overridden");return SDGO_GAME_TEST_FAIL;
            }
            return SDGO_GAME_TEST_WAIT;
        }
        admissionAt=deniedObservationAt=0;
        ++issued; issuedAt = context->elapsed_ms;issuedBank=previousBank;
        // Admission alone is insufficient. Require the actual form commit and
        // released movement state; never resend an admitted request each frame.
        char message[192];
        sprintf_s(message,"apsalus probe: original transform request issued, immediate return=%u; await real form commit and release",result);
        context->Observe(message);
    }
    return SDGO_GAME_TEST_WAIT;
}
}
