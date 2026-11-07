#pragma once
#include "sdgo/login_showcase_api.h"

// Additional assertions inside the existing native bootstrap case. The sealed
// developer setting enables this only in the owned login-scene acceptance run.
int ProbeLoginShowcase(const SdgoGameTestContextV1* context) {
    if(!sdgo_test::api->SettingInt(sdgo_test::mod,"LoginShowcaseTest",0))return SDGO_GAME_TEST_PASS;
    auto module=GetModuleHandleW(L"startup_crash_fix.dll");
    auto observe=module?reinterpret_cast<SdgoLoginShowcaseObserveV1Fn>(GetProcAddress(module,SDGO_LOGIN_SHOWCASE_OBSERVE_EXPORT)):nullptr;
    SdgoLoginShowcaseObservationV1 snapshot{};snapshot.size=sizeof(snapshot);
    if(!observe || !observe(&snapshot) || !snapshot.model)return SDGO_GAME_TEST_WAIT;
    const auto expected=static_cast<uint32_t>(sdgo_test::api->SettingInt(sdgo_test::mod,"LoginShowcaseExpectedUnit",0));
    if(expected && snapshot.unit_id!=expected) {
        context->Assert(false,"the original login model constructor received the selected test unit");
        return SDGO_GAME_TEST_FAIL;
    }
    const auto mask=static_cast<uint32_t>(sdgo_test::api->SettingInt(sdgo_test::mod,"LoginShowcaseGradeMask",0));
    if(mask) {
        using Grid=void*(__cdecl*)();
        using Find=int(__thiscall*)(void*,const uint8_t**,uint32_t,int,char);
        auto grid=reinterpret_cast<Grid>(sdgo_test::api->Symbol(sdgo_test::mod,"grid.data"));
        auto find=reinterpret_cast<Find>(sdgo_test::api->Symbol(sdgo_test::mod,"grid.find"));
        const uint8_t* row=nullptr;
        const bool valid=grid&&find&&find(grid(),&row,snapshot.unit_id,2,0)&&row&&row[308]>=1&&row[308]<=4&&row[309]>=2&&row[309]<=5;
        if(!valid || !(mask&(1u<<((row[308]-1)*4+row[309]-2)))) {
            context->Assert(false,"the native unit definition has one of the requested showcase grades");
            return SDGO_GAME_TEST_FAIL;
        }
    }
    // The model pointer and animation handle become visible as soon as the
    // original constructor returns, before its streamed resources have
    // necessarily reached a presented frame. Let the native renderer settle
    // before requesting the acceptance capture.
    static unsigned settled_frames=0;
    if(settled_frames++<900)return SDGO_GAME_TEST_WAIT;
    static unsigned capture=0;
    const auto folder=sdgo_test::directory+L"\\visual";
    if(!capture) {
        CreateDirectoryW(folder.c_str(),nullptr);
        if(!quietGraphics.CaptureNextFrame((folder+L"\\login-showcase.bmp").c_str()))return SDGO_GAME_TEST_FAIL;
        capture=1;return SDGO_GAME_TEST_WAIT;
    }
    if(quietGraphics.CaptureFrameResult()==E_PENDING)return SDGO_GAME_TEST_WAIT;
    context->Assert(SUCCEEDED(quietGraphics.CaptureFrameResult()),"the actual native login backbuffer was captured with its chosen model");
    if(FAILED(quietGraphics.CaptureFrameResult()))return SDGO_GAME_TEST_FAIL;
    const bool animated=snapshot.completed_calls>0 && snapshot.model && snapshot.animation_handle;
    context->Assert(animated,"the original loading-scene constructor produced a native animated model");
    if(!animated)return SDGO_GAME_TEST_FAIL;
    char message[256];sprintf_s(message,"login showcase native unit=%u original=%u set=%u model=%p animation=%u",snapshot.unit_id,snapshot.original_unit_id,snapshot.loading_set_id,reinterpret_cast<void*>(snapshot.model),snapshot.animation_handle);
    context->Observe(message);
    return SDGO_GAME_TEST_PASS;
}
