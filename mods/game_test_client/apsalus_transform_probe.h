#pragma once

// Opt-in native GameTest scenario, sealed in the copied observer manifest.
// Uses the normal, owned unit selected by
// --play-unit-uid and the original transform admission/request business.
// No keyboard, profile, inventory, model, FSM or server state is fabricated.
namespace apsalus_probe {
inline int Poll(const SdgoGameTestContextV1* context, const SdgoApi* api, SdgoMod* mod) {
    static const bool enabled = api->SettingInt(mod, "ApsalusRoundtrip", 0) != 0;
    if (!enabled) return SDGO_GAME_TEST_PASS;
    using Find = unsigned char*(__thiscall*)(void*, unsigned);
    using Request = unsigned(__thiscall*)(void*);
    static auto manager = reinterpret_cast<void**>(api->Symbol(mod, "game.unit_manager"));
    static auto find = reinterpret_cast<Find>(api->Symbol(mod, "um.find_object"));
    static auto request = reinterpret_cast<Request>(api->Symbol(mod, "test.apsalus_transform_request"));
    static unsigned issued = 0, completed = 0, initialId = 0, lastMode = ~0u, lastId = 0;
    static uint64_t issuedAt = 0;
    if (!manager || !find || !request || !*manager) {
        context->Assert(0, "Apsalus probe requires verified native request and unit-manager bindings");
        return SDGO_GAME_TEST_FAIL;
    }
    auto* unit = find(*manager, 1);
    if (!unit) return SDGO_GAME_TEST_WAIT;
    auto* move = *reinterpret_cast<unsigned char**>(unit + 0x9cc);
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
    if (completed == 2) return SDGO_GAME_TEST_PASS;
    if (context->elapsed_ms > 500 && mode == 0) {
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
