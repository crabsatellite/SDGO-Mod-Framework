// Exercise the production x86 DLL; the admitted 79-byte callback fingerprint
// identifies the exact wrapper recovered from the supported shipped artifact.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "sdgo/sdgo_api.h"
#include "sdgo/waitroom_window_observer.h"

namespace {
unsigned checks = 0, originalCalls = 0, hookCalls = 0;
using Detour = std::uint8_t(__fastcall*)(void*, void*, HWND, UINT, WPARAM, LPARAM);
Detour detour = nullptr;
std::array<std::uint8_t, 79> body = {
    0x55,0x8b,0xec,0x56,0x57,0x8b,0x7d,0x0c,0x8b,0xf1,0x83,0xff,0x10,0x75,0x23,0x6a,
    0x0f,0xe8,0xaa,0x5d,0xcd,0xff,0x8b,0xc8,0xe8,0xa3,0x84,0xfb,0xff,0x68,0x20,0x72,
    0xe8,0x00,0x6a,0x02,0xe8,0x87,0xee,0xcc,0xff,0x50,0xe8,0x71,0x05,0x4b,0x00,0x83,
    0xc4,0x0c,0x8b,0x45,0x14,0x8b,0x4d,0x10,0x8b,0x55,0x08,0x50,0x51,0x8b,0x4e,0x04,
    0x57,0x52,0xe8,0x99,0x1a,0x26,0x00,0x5f,0x32,0xc0,0x5e,0x5d,0xc2,0x10,0x00};
struct Call {void* self;HWND window;UINT message;WPARAM wparam;LPARAM lparam;DWORD error;};
Call observed{};
struct Service {void* vtable;void* view;std::uint32_t untouched;};
void Check(bool value, const char* message) {
    ++checks;if (!value) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}
void Log(SdgoMod*, int, const char*, ...) {SetLastError(0x1111);}
void* Symbol(SdgoMod*, const char* name) {
    return !std::strcmp(name, "window.waitroom_message") ? body.data() : nullptr;
}
std::uint8_t __fastcall Original(void* self, void*, HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    ++originalCalls;observed={self,window,message,wparam,lparam,GetLastError()};
    SetLastError(0x6226);return 0x6a;
}
int Hook(SdgoMod*, const char* name, void* replacement, void** original) {
    if (std::strcmp(name,"window.waitroom_message"))return 0;
    ++hookCalls;detour=reinterpret_cast<Detour>(replacement);
    *original=reinterpret_cast<void*>(&Original);return 1;
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2)throw std::runtime_error("pass the built wait-room DLL");
        auto module=LoadLibraryW(argv[1]);Check(module!=nullptr,"actual production DLL loads");
        auto entry=reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module,"SdgoModLoad"));
        auto snapshot=reinterpret_cast<SdgoWaitroomWindowSnapshotFn>(GetProcAddress(module,SDGO_WAITROOM_WINDOW_SNAPSHOT_EXPORT));
        Check(entry && snapshot,"entry and passive observation export are present");
        SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Log=Log;api.Symbol=Symbol;api.HookCode=Hook;
        const auto originalByte=body.back();body.back()^=1;
        Check(!entry(&api,nullptr) && !hookCalls,"modified callback tail is refused before any hook");
        body.back()=originalByte;Check(entry(&api,nullptr) && hookCalls==1 && detour,"verified callback binds exactly once");
        Check(!entry(&api,nullptr) && hookCalls==1,"another load cannot duplicate the hook");
        Service service{reinterpret_cast<void*>(0x12340000),nullptr,0x5a5a5a5a};const auto before=service;
        HWND window=reinterpret_cast<HWND>(0x1234);constexpr WPARAM wp=0x76543210;constexpr LPARAM lp=static_cast<LPARAM>(0x98765432);
        SetLastError(0x5533);
        Check(detour(&service,nullptr,window,WM_GETICON,wp,lp)==0 && originalCalls==0,
              "unattached view icon query returns the original not-handled result");
        Check(GetLastError()==0x5533 && !std::memcmp(&service,&before,sizeof(service)),
              "guard preserves last-error and every native service byte");
        service.view=reinterpret_cast<void*>(0x5678);SetLastError(0x7788);
        Check(detour(&service,nullptr,window,WM_GETICON,wp,lp)==0x6a && originalCalls==1,
              "attached view retains its original icon callback result");
        Check(observed.self==&service && observed.window==window && observed.message==WM_GETICON &&
              observed.wparam==wp && observed.lparam==lp && observed.error==0x7788 && GetLastError()==0x6226,
              "the x86 callback retains every argument and original last-error behavior");
        service.view=nullptr;
        for(UINT message : {static_cast<UINT>(WM_CLOSE),static_cast<UINT>(WM_KEYDOWN),static_cast<UINT>(WM_GETTEXT)}) {
            const auto count=originalCalls;Check(detour(&service,nullptr,window,message,wp,lp)==0x6a && originalCalls==count+1,
                "non-icon messages remain original even before attachment");
        }
        const auto count=originalCalls;
        Check(detour(nullptr,nullptr,window,WM_GETICON,wp,lp)==0x6a && originalCalls==count+1,
              "an unrelated null service is not reclassified as a missing view");
        SdgoWaitroomWindowSnapshotV1 state{sizeof(state),1,0,0,0,0};
        Check(snapshot(&state) && state.process_id==GetCurrentProcessId() && state.guarded_icons==1 &&
              state.forwarded_messages==originalCalls && state.last_guard_thread==GetCurrentThreadId(),
              "passive readback identifies exactly one scoped guard and the calling thread");
        state.version=2;Check(!snapshot(&state),"unknown observation ABI is refused");
        std::printf("%u checks passed\n",checks);return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
