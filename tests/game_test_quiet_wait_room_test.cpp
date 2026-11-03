#include "../mods/game_test_client/quiet_wait_room_messages.h"
#include <cstdio>

namespace {
using Detour=uintptr_t(__fastcall*)(void*,void*,HWND,UINT,WPARAM,LPARAM);
Detour detour=nullptr;
int calls=0,failures=0;
void* expected=nullptr;
UINT expectedMessage=0;
uintptr_t __fastcall Original(void* service,void*,HWND window,UINT message,WPARAM w,LPARAM l) {
    ++calls;
    if(service!=expected || window!=reinterpret_cast<HWND>(0x1234) || message!=expectedMessage ||
       w!=0x5678 || l!=0x2345)++failures;
    return 0x12340000;
}
int __cdecl Hook(SdgoMod*,const char* symbol,void* replacement,void** original) {
    if(std::strcmp(symbol,"flow.wait_room_window_message"))return 0;
    detour=reinterpret_cast<Detour>(replacement);*original=reinterpret_cast<void*>(&Original);return 1;
}
}
int main() {
    SdgoApi api{};api.HookCode=&Hook;
    QuietWaitRoomMessages guard;
    if(!guard.Install(&api,reinterpret_cast<SdgoMod*>(1)) || !detour)return 1;
    struct Descriptor {void* vtable;void* view;} descriptor{};
    expected=&descriptor;
    const auto invoke=[&](UINT message){expectedMessage=message;return detour(expected,nullptr,reinterpret_cast<HWND>(0x1234),message,0x5678,0x2345);};
    if(invoke(WM_GETICON)!=0 || calls!=0)++failures;
    descriptor.view=reinterpret_cast<void*>(1);
    if(invoke(WM_GETICON)!=0x12340000 || calls!=1)++failures;
    descriptor.view=nullptr;
    if(invoke(WM_CLOSE)!=0x12340000 || calls!=2)++failures;
    if(invoke(WM_KEYDOWN)!=0x12340000 || calls!=3)++failures;
    expected=nullptr;
    if(invoke(WM_GETICON)!=0x12340000 || calls!=4)++failures;
    std::printf("Quiet waiting-room icon query ABI: %d failures\n",failures);
    return failures?1:0;
}
