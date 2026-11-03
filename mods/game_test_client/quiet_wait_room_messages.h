#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/waitroom_window_observer.h"

// Hidden Windows can receive icon queries while the native service descriptor
// exists but its waiting-room view has not been constructed. The shipped
// wrapper dereferences descriptor+4 unconditionally. Returning "not handled"
// for that one query lets the application's normal default handler answer it.
class QuietWaitRoomMessages {
public:
    using Original=uintptr_t(__thiscall*)(void*,HWND,UINT,WPARAM,LPARAM);
    bool Install(const SdgoApi* api,SdgoMod* mod,SdgoWaitroomWindowSnapshotFn observe=nullptr) {
        static_assert(sizeof(void*)==4,"Original client window ABI is x86");
        if(original_ || shared_)return true;
        if(!api || !api->HookCode || (active_ && active_!=this))return false;
        if(!observe) {
            const auto module=GetModuleHandleW(L"waitroom_window_fix.dll");
            if(module)observe=reinterpret_cast<SdgoWaitroomWindowSnapshotFn>(
                GetProcAddress(module,SDGO_WAITROOM_WINDOW_SNAPSHOT_EXPORT));
        }
        if(observe) {
            const DWORD error=GetLastError();
            SdgoWaitroomWindowSnapshotV1 state{};state.size=sizeof(state);state.version=1;
            const bool ready=observe(&state)!=0 && state.size==sizeof(state) &&
                state.version==1 && state.process_id==GetCurrentProcessId();
            SetLastError(error);
            if(ready){shared_=true;return true;}
        }
        active_=this;
        if(!api->HookCode(mod,"flow.wait_room_window_message",reinterpret_cast<void*>(&Dispatch),
                          reinterpret_cast<void**>(&original_)) || !original_) {
            active_=nullptr;return false;
        }
        return true;
    }
private:
    static uintptr_t __fastcall Dispatch(void* service,void*,HWND window,UINT message,WPARAM w,LPARAM l) {
        auto* self=active_;
        if(!self || !self->original_)return 0;
        if(service && message==WM_GETICON) {
            void* view=nullptr;
            std::memcpy(&view,static_cast<const uint8_t*>(service)+4,sizeof(view));
            if(!view)return 0;
        }
        return self->original_(service,window,message,w,l);
    }
    Original original_=nullptr;
    bool shared_=false;
    inline static QuietWaitRoomMessages* active_=nullptr;
};
