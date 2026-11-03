#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

// Hidden Windows can receive icon queries while the native service descriptor
// exists but its waiting-room view has not been constructed. The shipped
// wrapper dereferences descriptor+4 unconditionally. Returning "not handled"
// for that one query lets the application's normal default handler answer it.
class QuietWaitRoomMessages {
public:
    using Original=uintptr_t(__thiscall*)(void*,HWND,UINT,WPARAM,LPARAM);
    bool Install(const SdgoApi* api,SdgoMod* mod) {
        static_assert(sizeof(void*)==4,"Original client window ABI is x86");
        if(original_)return true;
        if(!api || !api->HookCode || (active_ && active_!=this))return false;
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
    inline static QuietWaitRoomMessages* active_=nullptr;
};
