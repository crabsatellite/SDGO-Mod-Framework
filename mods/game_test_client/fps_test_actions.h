#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "sdgo/fps_limit_observer.h"
#include "sdgo/game_test.h"

// Read-only native FPS observations. This never installs a timer, synthesizes
// frames, changes application state or modifies the production FPS value.
class FpsTestActions {
public:
    bool Bind(const SdgoApi* api, SdgoMod* mod, int requested, bool enabled) {
        app_ = reinterpret_cast<AppFn>(api->Symbol(mod,"game.app"));
        requested_=requested; effective_=enabled ? requested:60; enabled_=enabled;
        if (enabled_) {
            const HMODULE module=GetModuleHandleW(L"fps_limit.dll");
            snapshot_=module ? reinterpret_cast<SdgoFpsLimitGetSnapshotV1Fn>(
                GetProcAddress(module,SDGO_FPS_LIMIT_SNAPSHOT_EXPORT)):nullptr;
        }
        return app_ && requested>=1 && requested<=1000 && QueryPerformanceFrequency(&frequency_) &&
            (!enabled_ || snapshot_);
    }
    uint32_t State() const {
        auto* app=app_ ? app_():nullptr;
        return app ? *reinterpret_cast<const uint32_t*>(app+0x4A0):UINT32_MAX;
    }
    int Cap(const SdgoGameTestContextV1* context,uint64_t presentFrequency,
            uint64_t minimumPresentGap,uint64_t maximumPresentGap) {
        auto* app=app_();
        auto* timer=app ? *reinterpret_cast<const uint8_t* const*>(app+0x74):nullptr;
        if (!timer)return SDGO_GAME_TEST_WAIT;
        const int cap=*reinterpret_cast<const int*>(app+0x80);
        const float interval=*reinterpret_cast<const float*>(timer+0x18);
        context->Assert(cap==effective_,"the real application's FPS field matches the configured or disabled baseline cap");
        context->Assert(std::isfinite(interval) && std::abs(double(interval)-1.0/double(effective_))<0.0000001,
            "the real native simulation timer interval equals one divided by the configured update cap");
        context->Assert((GetModuleHandleW(L"fps_limit.dll")!=nullptr)==enabled_,
            "the production FPS Mod load state matches the explicit test plan");
        context->Assert(app[0x44A]==0,
            "the real native independent-render path remains available for direct render pacing");
        SdgoFpsLimitSnapshotV1 production{};
        if (enabled_) {
            if (!ReadSnapshot(production)) {
                context->Assert(false,"the production FPS Mod exposes its passive pacing snapshot");
                return SDGO_GAME_TEST_PASS;
            }
            if (!production.period_ticks || production.present_calls<2 ||
                (effective_==30 && !production.waited_calls))
                return SDGO_GAME_TEST_WAIT;
            const uint64_t expectedPeriod=
                (production.qpc_frequency+static_cast<uint64_t>(effective_)-1)/
                static_cast<uint64_t>(effective_);
            context->Assert(production.render_cap==static_cast<uint32_t>(effective_),
                "the production pacing snapshot uses the configured render cap");
            context->Assert(production.update_hz==static_cast<uint32_t>(effective_),
                "the production pacing snapshot uses the configured update cap");
            context->Assert(production.qpc_frequency>0 && production.period_ticks==expectedPeriod,
                "the production pacing period is the exact QPC ceiling for the configured render cap");
            if (effective_==30)context->Assert(production.waited_calls>0,
                "the 30 FPS positive control exercised the production wait path");
        }
        char message[640];sprintf_s(message,
            "fps native requested=%d mod_enabled=%u effective=%d interval_seconds=%.9f expected_interval=%.9f "
            "update_cap=%d production_present_calls=%u waited_calls=%u production_qpc=%llu period_ticks=%llu "
            "period_ms=%.6f successful_present_qpc=%llu min_success_gap_ticks=%llu max_success_gap_ticks=%llu",
            requested_,enabled_ ? 1u:0u,cap,interval,1.0/double(effective_),effective_,
            production.present_calls,production.waited_calls,
            static_cast<unsigned long long>(production.qpc_frequency),
            static_cast<unsigned long long>(production.period_ticks),
            production.qpc_frequency ? 1000.0*double(production.period_ticks)/double(production.qpc_frequency):0.0,
            static_cast<unsigned long long>(presentFrequency),
            static_cast<unsigned long long>(minimumPresentGap),
            static_cast<unsigned long long>(maximumPresentGap));
        context->Observe(message);
        return SDGO_GAME_TEST_PASS;
    }
    int Cadence(const SdgoGameTestContextV1* context,uint32_t presents,
                uint64_t presentFrequency,uint64_t minimumPresentGap,uint64_t maximumPresentGap) {
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        if (!sampling_) {
            // Login-page assets are loaded through real native updates. Do not
            // count a synchronous startup load as steady rendering. Require a
            // full second of consecutive responsive updates and real Presents
            // before beginning the independent measurement windows.
            const double gap=lastPoll_ ? double(now.QuadPart-lastPoll_)/double(frequency_.QuadPart):1.0;
            responsive_=gap<=0.2 ? responsive_+1:0;
            lastPoll_=now.QuadPart;
            if (responsive_<static_cast<unsigned>(effective_) || presents<30)return SDGO_GAME_TEST_WAIT;
            sampling_=true;start_=totalStart_=now.QuadPart;startTick_=totalStartTick_=context->tick;
            startPresents_=totalStartPresents_=presents;
            context->Observe("fps warmup completed: responsive native updates and at least 30 successful D3D9 Presents");
            return SDGO_GAME_TEST_WAIT;
        }
        const double seconds=double(now.QuadPart-start_)/double(frequency_.QuadPart);
        if (seconds<3.0)return SDGO_GAME_TEST_WAIT;
        const uint32_t updates=context->tick-startTick_;
        const uint32_t presented=presents-startPresents_;
        const double updateRate=updates/seconds, presentRate=presented/seconds;
        char message[640];sprintf_s(message,
            "fps cadence window=%u seconds=%.6f updates=%u presents=%u update_hz=%.3f present_hz=%.3f "
            "cap=%d min_success_gap_ticks=%llu max_success_gap_ticks=%llu successful_present_qpc=%llu",
            window_+1,seconds,updates,presented,updateRate,presentRate,effective_,
            static_cast<unsigned long long>(minimumPresentGap),
            static_cast<unsigned long long>(maximumPresentGap),
            static_cast<unsigned long long>(presentFrequency));
        context->Observe(message);
        // Independent wall-clock and genuine successful Present counts, not
        // the game's own FPS display or a repeated configuration log.
        if (++window_==3) {
            const double totalSeconds=double(now.QuadPart-totalStart_)/double(frequency_.QuadPart);
            const double totalUpdates=(context->tick-totalStartTick_)/totalSeconds;
            const double totalPresents=(presents-totalStartPresents_)/totalSeconds;
            SdgoFpsLimitSnapshotV1 production{};
            const bool productionRead=!enabled_ || ReadSnapshot(production);
            const double minimumGapSeconds=presentFrequency
                ? double(minimumPresentGap)/double(presentFrequency):0.0;
            const double maximumGapSeconds=presentFrequency
                ? double(maximumPresentGap)/double(presentFrequency):0.0;
            sprintf_s(message,
                "fps cadence total seconds=%.6f update_hz=%.3f present_hz=%.3f cap=%d render_limited=%u "
                "update_cap=%d min_success_gap_ms=%.6f max_success_gap_ms=%.6f "
                "production_present_calls=%u waited_calls=%u period_ticks=%llu",
                totalSeconds,totalUpdates,totalPresents,effective_,enabled_ ? 1u:0u,
                effective_,
                minimumGapSeconds*1000.0,maximumGapSeconds*1000.0,
                production.present_calls,production.waited_calls,
                static_cast<unsigned long long>(production.period_ticks));
            context->Observe(message);
            // The original timer catches up after scheduler stalls. Keep all
            // individual windows visible; judge cadence over the entire sample
            // rather than incorrectly treating a catch-up burst as a new cap.
            context->Assert(totalUpdates>=effective_*0.80 && totalUpdates<=effective_*1.10,
                "real native simulation cadence across all sample windows follows the configured update cap");
            context->Assert(totalPresents>1.0,"the real native renderer completed successful D3D9 Presents");
            context->Assert(presentFrequency>0 && minimumPresentGap>0 && maximumPresentGap>=minimumPresentGap,
                "independent successful D3D9 Present invocation gaps were measured with QPC");
            const double configuredPeriod=1.0/double(effective_);
            const double allowedJitter=configuredPeriod*0.10>0.0005
                ? configuredPeriod*0.10:0.0005;
            if (enabled_) {
                context->Assert(productionRead,
                    "the production FPS pacing snapshot remains readable after all cadence windows");
                context->Assert(totalPresents<=effective_*1.10,
                    "actual D3D9 Present throughput does not exceed the configured render cap");
                context->Assert(minimumGapSeconds+allowedJitter>=configuredPeriod,
                    "the minimum successful D3D9 Present gap prevents unbounded over-frequency rendering");
                if (effective_==30)context->Assert(production.waited_calls>0,
                    "the 30 FPS positive control retained proof that production pacing waited");
            }
            return SDGO_GAME_TEST_PASS;
        }
        start_=now.QuadPart;startTick_=context->tick;startPresents_=presents;
        return SDGO_GAME_TEST_WAIT;
    }
private:
    using AppFn=uint8_t*(__cdecl*)();
    bool ReadSnapshot(SdgoFpsLimitSnapshotV1& value) const {
        value={};value.size=sizeof(value);value.version=1;
        return snapshot_ && snapshot_(&value)!=0 && value.size==sizeof(value) && value.version==1;
    }
    AppFn app_=nullptr;
    SdgoFpsLimitGetSnapshotV1Fn snapshot_=nullptr;
    LARGE_INTEGER frequency_{};
    LONGLONG start_=0;
    LONGLONG totalStart_=0;
    LONGLONG lastPoll_=0;
    uint32_t startTick_=0,startPresents_=0,window_=0;
    uint32_t totalStartTick_=0,totalStartPresents_=0;
    unsigned responsive_=0;
    int requested_=60,effective_=60;
    bool enabled_=false;
    bool sampling_=false;
};
