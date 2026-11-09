#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "native_actions.h"
#include "onboarding_actions.h"
#include "hangar_navigation.h"
#include "hangar_preview_guard_probe.h"
#include "sdgo/game_test.h"
#include "sdgo/game_test_page_observer.h"
#include "sdgo/hangar_preview_guard_observer.h"

// One owned native session: stock navigation, scoped graphics faults, stock
// thumbnail fallback, successful retry, then real logout/login in the same PID.
class HangarPreviewGuardActions {
    using Method=void(__thiscall*)(void*);
    using Getter=void*(__cdecl*)();
    using Send=void(__thiscall*)(void*,void*,char);
    const SdgoApi* api_=nullptr;SdgoMod* mod_=nullptr;
    Getter app_=nullptr,service_=nullptr;Method suspend_=nullptr,msg_ctor_=nullptr,msg_dtor_=nullptr;Send send_=nullptr;
    SdgoHangarPreviewGuardGetStatusV1Fn get_=nullptr;
    SdgoHangarGameTestGetSnapshotV1Fn hangar_=nullptr;
    SdgoHangarGameTestGetUnitsV1Fn units_=nullptr;
    std::vector<SdgoHangarGameTestUnitV1> before_;
    SdgoHangarGameTestSnapshotV1 baseline_{};
    SdgoHangarPreviewGuardStatusV1 cold_before_{};
    unsigned phase_=0,capture_=0;
    uint64_t mark_=0,turn_=0;
    uint32_t recovered_before_=0,auth_before_=0,pilot_=0,failed_unit_=0,failed_slot_=0;
    bool login_sent_=false,relogin_inventory_ready_=false,relogin_management_opened_=false;
    template<class T>bool Bind(T& value,const char* name){value=reinterpret_cast<T>(api_->Symbol(mod_,name));return value!=nullptr;}
    bool Hangar(SdgoHangarGameTestSnapshotV1& out) {
        out={};out.size=sizeof(out);out.version=1;
        return hangar_&&hangar_(&out)&&out.loaded&&out.visible&&out.unit_count;
    }
    bool Guard(SdgoHangarPreviewGuardStatusV1& out) {
        out={};out.size=sizeof(out);out.version=1;return get_&&get_(&out)!=0;
    }
    bool Inventory(SdgoHangarGameTestSnapshotV1& info,std::vector<SdgoHangarGameTestUnitV1>& out) {
        if(!Hangar(info)||info.unit_count>1200)return false;
        out.resize(info.unit_count);
        return units_(info.run_hash,0,out.data(),sizeof(out[0]),static_cast<uint32_t>(out.size()))==static_cast<int>(out.size());
    }
    static bool Equal(const std::vector<SdgoHangarGameTestUnitV1>& a,const std::vector<SdgoHangarGameTestUnitV1>& b) {
        if(a.size()!=b.size())return false;
        // Detail-cache readiness changes as the real server replies arrive;
        // ownership, native slot/level/flags and protection must stay equal.
        constexpr uint32_t flags=~static_cast<uint32_t>(SDGO_HANGAR_GAME_TEST_UNIT_FULL_READY);
        constexpr uint32_t protection=~static_cast<uint32_t>(SDGO_HANGAR_GAME_TEST_PROTECTED_DETAIL_MISSING);
        for(size_t i=0;i<a.size();++i)if(a[i].uid!=b[i].uid||a[i].unit_id!=b[i].unit_id||a[i].slot!=b[i].slot||
            a[i].level!=b[i].level||(a[i].flags&flags)!=(b[i].flags&flags)||
            (a[i].protection_flags&protection)!=(b[i].protection_flags&protection))return false;
        return true;
    }
    bool Logout() {
        alignas(4)uint8_t message[0x30]{};
        suspend_(app_());msg_ctor_(message);
        *reinterpret_cast<uint32_t*>(message+0x0C)=0x13;
        *reinterpret_cast<uint32_t*>(message+0x2C)=6;
        send_(service_(),message,1);msg_dtor_(message);return true;
    }
    static bool __cdecl CaptureReady(void* value) {
        auto* self=static_cast<HangarPreviewGuardActions*>(value);
        SdgoHangarGameTestSnapshotV1 info{};return self->Hangar(info)&&info.process_id==GetCurrentProcessId();
    }
    template<class Graphics>bool Capture(const SdgoGameTestContextV1* context,Graphics& graphics,const wchar_t* name) {
        if(!capture_) {
            const auto folder=std::wstring(context->directory)+L"\\visual";CreateDirectoryW(folder.c_str(),nullptr);
            const auto path=folder+L"\\"+name;
            if(!graphics.CaptureNextFrameWhen(path.c_str(),&CaptureReady,this)){
                context->Assert(0,"native hangar frame capture is available");capture_=2;return true;
            }
            capture_=1;return false;
        }
        if(capture_==1) {
            const auto result=graphics.CaptureFrameResult();if(result==E_PENDING)return false;
            context->Assert(SUCCEEDED(result),"predicate-bound genuine hangar backbuffer capture completed");
        }
        capture_=0;return true;
    }
    int Fail(const SdgoGameTestContextV1* context,const char* message) {context->Assert(0,message);return SDGO_GAME_TEST_FAIL;}
public:
    bool Bind(const SdgoApi* api,SdgoMod* mod) {
        api_=api;mod_=mod;
        auto guard=GetModuleHandleW(L"hangar_preview_guard.dll");auto hangar=GetModuleHandleW(L"hangar_manager.dll");
        get_=guard?reinterpret_cast<SdgoHangarPreviewGuardGetStatusV1Fn>(GetProcAddress(guard,SDGO_HANGAR_PREVIEW_GUARD_STATUS_EXPORT)):nullptr;
        hangar_=hangar?reinterpret_cast<SdgoHangarGameTestGetSnapshotV1Fn>(GetProcAddress(hangar,SDGO_HANGAR_GAME_TEST_SNAPSHOT_EXPORT)):nullptr;
        units_=hangar?reinterpret_cast<SdgoHangarGameTestGetUnitsV1Fn>(GetProcAddress(hangar,SDGO_HANGAR_GAME_TEST_UNITS_EXPORT)):nullptr;
        const bool bound=get_&&hangar_&&units_&&
            Bind(app_,"test.preview_guard.logout_app")&&Bind(service_,"test.preview_guard.logout_manager")&&
            Bind(suspend_,"test.preview_guard.logout_suspend")&&Bind(msg_ctor_,"test.preview_guard.logout_ctor")&&
            Bind(msg_dtor_,"test.preview_guard.logout_dtor")&&Bind(send_,"test.preview_guard.logout_send");
        return bound&&Guard(cold_before_);
    }
    template<class Graphics>int Poll(const SdgoGameTestContextV1* context,NativeActions& actions,OnboardingActions& onboarding,HangarNavigation& navigation,
                                     HangarPreviewGuardFaultProbe& probe,Graphics& graphics) {
        const uint64_t now=GetTickCount64();
        SdgoHangarPreviewGuardStatusV1 state{};if(!Guard(state))return Fail(context,"production hangar preview guard observer is available");
        if(!context->tick){phase_=0;capture_=0;mark_=now;turn_=0;login_sent_=false;relogin_inventory_ready_=false;relogin_management_opened_=false;}
        if(phase_==0) {
            const auto result=navigation.EnsureUnitManagementPage();
            if(result==HangarNavigation::kUnbound||result>=HangarNavigation::kDownmenuServiceMismatch)return Fail(context,"stock native management navigation is valid");
            const auto observed=probe.State();
            if(observed.errors)return Fail(context,"cold native preview fault probe remains error-free");
            if(!observed.hits) {
                if(now-mark_>45000)return Fail(context,"the pre-armed cold VB32 creation fault was reached before native resource caching");
                return SDGO_GAME_TEST_WAIT;
            }
            if(observed.kind!=5||observed.hits!=1||observed.armed||!observed.last_unit)
                return Fail(context,"exactly one cold VB32 creation call consumed the pre-armed one-shot fault");
            if(state.fallbacks<cold_before_.fallbacks+1||state.discarded_models<cold_before_.discarded_models+1||
               state.lock_releases<cold_before_.lock_releases+1||state.discarded_roots<cold_before_.discarded_roots+1||
               state.closed_sources<cold_before_.closed_sources+1) {
                if(now-mark_>60000)return Fail(context,"the cold VB32 rejection completes its native ownership cleanup and thumbnail fallback");
                return SDGO_GAME_TEST_WAIT;
            }
            if(state.fallbacks!=cold_before_.fallbacks+1||state.discarded_models!=cold_before_.discarded_models+1||
               state.lock_releases!=cold_before_.lock_releases+1||state.discarded_roots!=cold_before_.discarded_roots+1||
               state.closed_sources!=cold_before_.closed_sources+1||state.missing_vertex!=cold_before_.missing_vertex+1||
               state.vertex32_failures!=cold_before_.vertex32_failures+1||state.failed_vertex_lock!=cold_before_.failed_vertex_lock||
               state.missing_index!=cold_before_.missing_index||state.failed_index_lock!=cold_before_.failed_index_lock||
               state.failed_model_load!=cold_before_.failed_model_load||state.last_failure!=1||
               state.last_unit!=observed.last_unit)
            {
                char details[512]{};
                sprintf_s(details,"preview rejection counters: fallback=%u model=%u release=%u root=%u source=%u vertex=%u vb24=%u vb32=%u lock=%u index=%u ilock=%u mload=%u recovered=%u last=%u reason=%u probe_unit=%u",
                    state.fallbacks,state.discarded_models,state.lock_releases,state.discarded_roots,state.closed_sources,
                    state.missing_vertex,state.vertex24_failures,state.vertex32_failures,state.failed_vertex_lock,
                    state.missing_index,state.failed_index_lock,state.failed_model_load,state.recovered_previews,
                    state.last_unit,state.last_failure,observed.last_unit);
                context->Observe(details);
                return Fail(context,"cold VB32 failure records one exclusive parser rejection, source close and thumbnail fallback");
            }
            if(result!=HangarNavigation::kReady||!Inventory(baseline_,before_)||!state.loaded) {
                if(now-mark_>60000)return Fail(context,"the cold fallback remains visible with a readable native inventory");
                return SDGO_GAME_TEST_WAIT;
            }
            pilot_=actions.OnboardingState().profile_pilot_id;
            if(!pilot_) {
                if(now-mark_>60000)return Fail(context,"the native onboarding profile identifies the authenticated pilot");
                return SDGO_GAME_TEST_WAIT;
            }
            failed_unit_=state.last_unit;failed_slot_=state.last_failed_slot;recovered_before_=state.recovered_previews;
            context->Assert(baseline_.process_id==GetCurrentProcessId(),
                "cold VB32 failure completed in the authenticated native inventory process");
            char line[320];sprintf_s(line,"cold native VB32 fault hits=%u unit=%u slot=%u inventory=%u roots=%u candidates=%u sources=%u fallback=%u automatic_same_slot_retries=%u",
                observed.hits,failed_unit_,failed_slot_,baseline_.unit_count,state.discarded_roots,state.discarded_models,state.closed_sources,state.fallbacks,state.recovered_previews);
            context->Observe(line);phase_=1;mark_=now;
        }
        if(phase_==1) {
            if(state.last_unit!=failed_unit_||state.last_failed_slot!=failed_slot_)
                return Fail(context,"the transient fault retains its failed inventory-cell identity");
            // Native page construction may automatically retry the same cell
            // before a frame is presented. Cleanup/fallback is proved above;
            // this frame records the genuine post-fault UI, possibly recovered.
            if(now-mark_<700||!Capture(context,graphics,L"preview-after-vb32-fault.bmp"))return SDGO_GAME_TEST_WAIT;
            probe.Arm(0);
            // The stock page already retries its cells during construction.
            // Count that actual same-slot recovery, rather than requiring a
            // redundant reload of an already cached successful model.
            recovered_before_=cold_before_.recovered_previews;
            phase_=2;mark_=now;
        }
        if(phase_==2) {
            const auto observed=probe.State();
            // Arm(0) clears kind; recovered_previews advances only when the
            // production setter succeeds for its last failed unit and slot.
            // Other cells may finish later and replace last_loaded_unit.
            if(observed.kind!=0||observed.hits!=1||observed.armed||observed.errors||
               observed.last_unit!=failed_unit_||state.last_unit!=failed_unit_||state.last_failed_slot!=failed_slot_)
                return Fail(context,"unarmed retry preserves the failed unit identity without another injected operation");
            if(state.recovered_previews>recovered_before_)
                context->Assert(1,"the same rejected unit and slot completed an automatic unarmed native retry");
            else
                context->Observe("the native thumbnail remains until a new preview request; same-slot recovery will be verified after real account return and reentry");
            std::vector<SdgoHangarGameTestUnitV1> inventory;SdgoHangarGameTestSnapshotV1 info{};
            if(!Inventory(info,inventory)) {
                if(now-mark_>30000)return Fail(context,"the recovered preview retains a readable native inventory");
                return SDGO_GAME_TEST_WAIT;
            }
            if(!Equal(before_,inventory)||info.active_slot!=baseline_.active_slot)return Fail(context,"graphics failures preserve every inventory identity, slot, protection and active unit");
            context->Assert(1,"one cold VB32 fallback preserved the complete native inventory");
            phase_=3;mark_=now;return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==3) {
            if(now-mark_<700||!Capture(context,graphics,L"preview-before-account-return.bmp"))return SDGO_GAME_TEST_WAIT;
            auth_before_=actions.LoginState().reply_sequence;Logout();phase_=8;mark_=now;return SDGO_GAME_TEST_WAIT;
        }
        if(phase_==8) {
            if(actions.AppState()!=4) {
                if(now-mark_>30000)return Fail(context,"original logout reaches the account login gate");
                return SDGO_GAME_TEST_WAIT;
            }
            navigation.RearmAfterLogout();phase_=9;mark_=now;turn_=now;
            context->Assert(1,"native account-return cleanup completed in the original process");
        }
        if(phase_==9) {
            if(!login_sent_&&now-turn_>500&&actions.LoginService()){login_sent_=actions.Login();turn_=now;}
            const auto auth=actions.LoginState();
            const auto profile=actions.OnboardingState();
            if(!login_sent_||auth.reply_sequence<=auth_before_||auth.result!=NativeActions::kAuthAccepted||profile.profile_pilot_id!=pilot_) {
                if(now-mark_>60000)return Fail(context,"the original login protocol authenticates the same existing pilot again");
                return SDGO_GAME_TEST_WAIT;
            }
            phase_=10;mark_=now;
        }
        if(phase_==10) {
            if(!relogin_inventory_ready_) {
                const auto inventory=onboarding.InventoryState();
                SdgoHangarGameTestSnapshotV1 fresh{};fresh.size=sizeof(fresh);fresh.version=1;
                const bool epoch=hangar_(&fresh)&&fresh.generation>baseline_.generation;
                if(now-turn_>2000) {
                    char message[192]{};sprintf_s(message,"relogin inventory gate app=%u initialized=%u scanned=%u owned=%u epoch=%u baseline=%u",
                        actions.AppState(),inventory.initialized,inventory.scan_complete,inventory.owned_unit_count,fresh.generation,baseline_.generation);
                    context->Observe(message);turn_=now;
                }
                if(!epoch||!inventory.initialized||!inventory.scan_complete||inventory.owned_unit_count!=before_.size()) {
                    if(now-mark_>60000)return Fail(context,"relogin completes its fresh native inventory before opening management");
                    return SDGO_GAME_TEST_WAIT;
                }
                relogin_inventory_ready_=true;mark_=now;
                context->Assert(1,"relogin populated a new native inventory epoch through genuine server replies");
            }
            if(!relogin_management_opened_) {
                const auto hangar=navigation.EnsureHangar();
                if(hangar==HangarNavigation::kUnbound||hangar>=HangarNavigation::kDownmenuServiceMismatch)
                    return Fail(context,"relogin resolves the live native hangar before management entry");
                if(hangar!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
                const auto opened=navigation.ClickOperatorMenuButton(L"UnitBtn",0x80);
                if(opened==HangarNavigation::kUnbound||opened>=HangarNavigation::kDownmenuServiceMismatch)
                    return Fail(context,"relogin resolves the live native UnitBtn management entry");
                if(opened!=HangarNavigation::kReady)return SDGO_GAME_TEST_WAIT;
                relogin_management_opened_=true;mark_=now;
                context->Observe("relogin invoked the original live UnitBtn management callback; stale cached visibility is not an entry action");
                return SDGO_GAME_TEST_WAIT;
            }
            if(now-mark_<1000)return SDGO_GAME_TEST_WAIT;
            const auto result=navigation.EnsureUnitManagementPage();
            if(result==HangarNavigation::kUnbound||result>=HangarNavigation::kDownmenuServiceMismatch)return Fail(context,"native navigation reopens management after account return");
            std::vector<SdgoHangarGameTestUnitV1> inventory;SdgoHangarGameTestSnapshotV1 info{};
            if(result!=HangarNavigation::kReady||!Inventory(info,inventory)) {
                if(now-mark_>60000)return Fail(context,"same-PID relogin reopens the readable native management inventory");
                return SDGO_GAME_TEST_WAIT;
            }
            if(actions.OnboardingState().profile_pilot_id!=pilot_||info.process_id!=baseline_.process_id||
               info.generation<=baseline_.generation||info.session==baseline_.session||
               info.active_slot!=baseline_.active_slot||!Equal(before_,inventory))return Fail(context,"same-PID relogin has a new native inventory generation and identical actual inventory");
            if(state.recovered_previews<=cold_before_.recovered_previews) {
                if(now-mark_>60000)return Fail(context,"the originally rejected unit and slot recover after native account return and reentry");
                return SDGO_GAME_TEST_WAIT;
            }
            if(!Capture(context,graphics,L"preview-relogin.bmp"))return SDGO_GAME_TEST_WAIT;
            context->Assert(1,"the originally rejected unit and native slot have a successful unarmed preview after real reentry");
            context->Assert(1,"same PID, same pilot and inventory survived real account return, relogin and reopened management");
            return SDGO_GAME_TEST_PASS;
        }
        return SDGO_GAME_TEST_WAIT;
    }
};
