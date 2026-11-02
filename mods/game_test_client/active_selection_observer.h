#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

// Read-only native account/inventory observation. No UI, hooks or writes to
// game records. Cache availability is distinct from a mismatched active unit.
class ActiveSelectionObserver {
public:
    struct Snapshot {
        uint32_t pilot=0,unit=0,inventory_unit=0;
        uint64_t uid=0,inventory_uid=0;
        uint16_t slot=0,inventory_slot=0;
        bool inventory_matches=false;
        bool SameSelection(const Snapshot& other) const {
            return pilot==other.pilot && unit==other.unit && uid==other.uid && slot==other.slot;
        }
    };
    using BriefFn=void(__thiscall*)(void*,void*,uint16_t);
    bool Bind(const SdgoApi* api,SdgoMod* mod) {
        if(!api || !api->Symbol)return false;
        profile_=reinterpret_cast<uint8_t**>(api->Symbol(mod,"flow.profile_pointer"));
        inventory_=reinterpret_cast<uint8_t**>(api->Symbol(mod,"flow.unit_info_pointer"));
        brief_=reinterpret_cast<BriefFn>(api->Symbol(mod,"flow.unit_brief_by_slot"));
        return profile_ && inventory_ && brief_;
    }
    bool Read(uint32_t expectedPilot,Snapshot* output) const {
        if(!output || !profile_ || !inventory_ || !brief_ || !expectedPilot)return false;
        Snapshot s;
        __try {
            auto* profile=*profile_;auto* inventory=*inventory_;
            if(!profile || !inventory || inventory[0x20]!=1)return false;
            const int32_t capacity=ReadValue<int32_t>(inventory+0x0C);
            s.pilot=ReadValue<uint32_t>(profile+0x138);
            s.unit=ReadValue<uint32_t>(profile+0x1A9);
            s.slot=ReadValue<uint16_t>(profile+0x1AD);
            s.uid=ReadValue<uint64_t>(profile+0x1B1);
            if(s.pilot!=expectedPilot || capacity<=0 || capacity>UINT16_MAX ||
                !s.slot || s.slot>capacity || !s.unit || !s.uid)return false;
            alignas(4) uint8_t brief[0x19]{};
            brief_(inventory,brief,s.slot);
            s.inventory_unit=ReadValue<uint32_t>(brief);
            s.inventory_uid=ReadValue<uint64_t>(brief+4);
            s.inventory_slot=ReadValue<uint16_t>(brief+12);
            s.inventory_matches=s.unit==s.inventory_unit && s.uid==s.inventory_uid && s.slot==s.inventory_slot;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
        *output=s;return true;
    }
private:
    template<class T> static T ReadValue(const void* source) {T value;std::memcpy(&value,source,sizeof(value));return value;}
    uint8_t** profile_=nullptr;
    uint8_t** inventory_=nullptr;
    BriefFn brief_=nullptr;
};
