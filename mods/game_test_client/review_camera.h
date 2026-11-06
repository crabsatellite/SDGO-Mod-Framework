#pragma once
#include <windows.h>
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "sdgo/pvp_ai_client_observer.h"
#include "sdgo/mission_ai_client_observer.h"
#include "../battle_ai/native_capabilities.h"

// Opt-in review camera for recorded footage (BattleReviewCamera=1). While a battle runs,
// the native tracking camera follows one AI unit at a time and moves on after
// BattleReviewCameraSeconds, so hidden-client footage shows the AI fighting rather than
// the idle test pilot. It changes only the camera's target pointer (unit manager +0x82C8
// -> tracking camera, +0x344 target), the field the native dead view points at another
// unit; no unit, input, packet or result is touched, and nothing is asserted.
class ReviewCamera {
public:
    void Install(const SdgoApi* api,SdgoMod* mod) {
        api_=api; mod_=mod;
        enabled_=api->SettingInt(mod,"BattleReviewCamera",0)!=0;
        const int seconds=api->SettingInt(mod,"BattleReviewCameraSeconds",6);
        holdMs_=static_cast<uint32_t>(seconds>=1 && seconds<=120 ? seconds:6)*1000u;
        if (enabled_) api->Log(mod,SDGO_LOG_INFO,"review camera: follows one AI unit for %u s at a time",holdMs_/1000u);
    }
    bool Enabled() const { return enabled_; }

    // Main thread, after the native app update. Either snapshot may be absent.
    void Tick(bool inBattle,const SdgoPvpAiClientSnapshotV1* pvp,const SdgoMissionAiClientSnapshotV1* mission) {
        if (!enabled_) return;
        if (!inBattle) { current_=0; return; }
        uint8_t* um=nullptr; uint8_t* camera=nullptr;
        if (!Manager(um) || !battle_ai::native::detail::ReadPointer(um,kUmCamera,camera) ||
            !battle_ai::native::detail::Readable(camera+kCameraTarget,sizeof(void*))) return;
        uint32_t ids[24]{}; bool engaged[24]{}; unsigned count=0;
        if (pvp)
            for (unsigned i=0;i<pvp->bot_count && i<12 && count<24;++i) {
                const auto& bot=pvp->bots[i];
                if (!bot.object_id || !bot.valid_registered_object || !bot.ai_enabled) continue;
                ids[count]=bot.object_id; engaged[count]=bot.target_object_id && bot.target_hp>0; ++count;
            }
        if (mission)
            for (unsigned i=0;i<12 && count<24;++i) {
                const auto& participant=mission->participants[i];
                if (!participant.object_id || !participant.spawned || !participant.valid_registered_object || participant.current_hp<=0) continue;
                ids[count]=participant.object_id; engaged[count]=participant.target_valid && participant.target_object_id; ++count;
            }
        uint8_t* unit=nullptr;
        if (count) {
            const uint32_t now=GetTickCount();
            unsigned currentIndex=0;
            for (unsigned i=0;i<count;++i) if (ids[i]==current_) { currentIndex=i; unit=Alive(um,current_); }
            if (!unit || now-since_>=holdMs_) {
                int pick=-1; uint8_t* pickUnit=nullptr;
                for (int pass=0;pass<2 && pick<0;++pass)
                    for (unsigned k=1;k<=count;++k) {
                        const unsigned i=(currentIndex+k)%count;
                        if (pass==0 && !engaged[i]) continue;
                        if (ids[i]==current_ && unit && count>1) continue;   // move on while another AI is up
                        if (uint8_t* candidate=Alive(um,ids[i])) { pick=static_cast<int>(i); pickUnit=candidate; break; }
                    }
                if (pick>=0) {
                    if (ids[pick]!=current_ && api_)
                        api_->Log(mod_,SDGO_LOG_INFO,"review camera: following AI object %lu",static_cast<unsigned long>(ids[pick]));
                    current_=ids[pick]; unit=pickUnit; since_=now;
                } else if (unit) since_=now;
            }
        } else current_=0;
        uint8_t* target=unit;
        if (!target && !battle_ai::native::detail::ReadPointer(um,kUmLocalPlayer,target)) return;   // the test pilot
        uint8_t* present=nullptr;
        battle_ai::native::detail::ReadPointer(camera,kCameraTarget,present);
        if (present!=target) *reinterpret_cast<uint8_t**>(camera+kCameraTarget)=target;
    }

private:
    static constexpr size_t kUmCamera=0x82C8;
    static constexpr size_t kUmLocalPlayer=0x88;
    static constexpr size_t kCameraTarget=0x344;
    static constexpr size_t kUnitHp=0x38;
    using FindObjectFn=uint8_t*(__thiscall*)(void*,uint32_t);

    bool Manager(uint8_t*& um) {
        if (!manager_) manager_=reinterpret_cast<void**>(api_->Symbol(mod_,"game.unit_manager"));
        return manager_ && battle_ai::native::detail::ReadPointer(reinterpret_cast<uint8_t*>(manager_),0,um);
    }
    uint8_t* Alive(uint8_t* um,uint32_t id) {
        if (!find_) find_=reinterpret_cast<FindObjectFn>(api_->Symbol(mod_,"um.find_object"));
        if (!find_) return nullptr;
        uint8_t* unit=nullptr;
        __try { unit=find_(um,id); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        int32_t hp=0;
        if (!unit || !battle_ai::native::detail::Read(unit,kUnitHp,hp) || hp<=0) return nullptr;
        return unit;
    }

    const SdgoApi* api_=nullptr; SdgoMod* mod_=nullptr;
    bool enabled_=false;
    uint32_t holdMs_=6000,current_=0,since_=0;
    void** manager_=nullptr;
    FindObjectFn find_=nullptr;
};
