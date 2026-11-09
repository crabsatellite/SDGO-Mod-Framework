#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "../mods/game_test_client/active_selection_observer.h"
#define private public
#include "../mods/game_test_client/unit_acquisition_actions.h"
#undef private
namespace {
uint8_t profile[0x800]{},inventory[0x40]{},brief[0x19]{};
uint8_t* profilePointer=profile;uint8_t* inventoryPointer=inventory;
int failures=0,calls=0;
template<class T> void Put(uint8_t* p,size_t at,T value){std::memcpy(p+at,&value,sizeof(value));}
void Check(bool value,const char* message){if(!value){++failures;std::printf("FAIL %s\n",message);}}
void __fastcall Brief(void* self,void*,void* out,uint16_t slot){
    Check(self==inventory && slot==4,"getter receives exact native inventory and ushort slot");
    ++calls;std::memcpy(out,brief,sizeof(brief));
}
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"flow.profile_pointer"))return &profilePointer;
    if(!std::strcmp(name,"flow.unit_info_pointer"))return &inventoryPointer;
    if(!std::strcmp(name,"flow.unit_brief_by_slot"))return reinterpret_cast<void*>(&Brief);
    return nullptr;
}
uint8_t customService[0x500]{},customRoot[0x200]{};
uint8_t machineRoot[0x200]{},machineButton[0x200]{};
uint8_t customVtable[0x100]{},serviceManagerStorage{};
void* serviceManager=&serviceManagerStorage;
int customOpenCallbacks=0;
void* __fastcall NativeStringCtor(void* self,void*,const wchar_t*){return self;}
void __fastcall NativeStringDtor(void*,void*){}
void* __fastcall FindService(void*,void*,const void*){return customService;}
void* __fastcall FindControl(void* self,void*,const void*,uint32_t kind){
    return self==machineRoot && kind==4?machineButton:nullptr;
}
void __stdcall MachineCallback(void* machine,uint32_t event,const void*,void* control){
    Check(machine==reinterpret_cast<void*>(0x1234) && event==0x101 &&
          control==machineButton,"custom manager opens through the exact original machine button callback");
    ++customOpenCallbacks;
}
void CheckEarlyCustomServiceOpen(){
    std::memset(customService,0,sizeof(customService));
    std::memset(customRoot,0,sizeof(customRoot));
    std::memset(machineRoot,0,sizeof(machineRoot));
    std::memset(machineButton,0,sizeof(machineButton));
    Put(customService,0,customVtable);
    Put(customService,UnitAcquisitionActions::kCustomRoot,customRoot);
    Put(machineButton,UnitAcquisitionActions::kControlParent,machineRoot);
    static const wchar_t buttonName[]=L"MachineSelectBtn";
    Put(machineButton,UnitAcquisitionActions::kControlName,buttonName);
    Put(machineButton,UnitAcquisitionActions::kControlName+
        UnitAcquisitionActions::kNativeStringSize,
        static_cast<uint32_t>(std::wcslen(buttonName)));
    Put(machineButton,UnitAcquisitionActions::kControlName+
        UnitAcquisitionActions::kNativeStringCapacity,
        static_cast<uint32_t>(std::wcslen(buttonName)));
    machineButton[UnitAcquisitionActions::kControlVisible]=1;
    machineButton[UnitAcquisitionActions::kControlEnabled]=1;
    UnitAcquisitionActions actions;
    actions.serviceManagedPointer_=&serviceManager;
    actions.serviceFindByName_=reinterpret_cast<
        UnitAcquisitionActions::ServiceFindByNameFn>(&FindService);
    actions.nativeWStringCtor_=reinterpret_cast<
        UnitAcquisitionActions::NativeWStringCtorFn>(&NativeStringCtor);
    actions.nativeWStringDtor_=reinterpret_cast<
        UnitAcquisitionActions::NativeWStringDtorFn>(&NativeStringDtor);
    actions.nativeUiFindControl_=reinterpret_cast<
        UnitAcquisitionActions::NativeUiFindControlFn>(&FindControl);
    actions.customVtable_=customVtable;
    actions.machineCallback_=&MachineCallback;
    customOpenCallbacks=0;
    const auto first=actions.PollCustomCandidate(
        reinterpret_cast<void*>(0x1234),machineRoot);
    Check(first==UnitAcquisitionActions::kOpeningCustomManager &&
          actions.State().custom_open_invocations==1 &&
          customOpenCallbacks==1,
          "an already-created custom service with a hidden root invokes MachineSelectBtn once");
    const auto second=actions.PollCustomCandidate(
        reinterpret_cast<void*>(0x1234),machineRoot);
    Check(second==UnitAcquisitionActions::kWaitingForCustomManager &&
          actions.State().custom_open_invocations==1 &&
          customOpenCallbacks==1,
          "a still-hidden custom root waits without toggling MachineSelectBtn again");
}
}
int main(){
    CheckEarlyCustomServiceOpen();
    SdgoApi api{};api.Symbol=&Symbol;ActiveSelectionObserver observer;
    Check(observer.Bind(&api,reinterpret_cast<SdgoMod*>(1)),"binding");
    Put(profile,0x138,uint32_t{101});Put(profile,0x1A9,uint32_t{22097});Put(profile,0x1AD,uint16_t{4});
    Put(profile,0x1AF,uint16_t{0xFFFF}); // flags adjacent to the native ushort slot
    Put(profile,0x1B1,UINT64_C(0x1234567800000042));
    Put(inventory,0x0C,int32_t{1200});inventory[0x20]=1;
    Put(brief,0,uint32_t{22097});Put(brief,4,UINT64_C(0x1234567800000042));Put(brief,12,uint16_t{4});
    const auto beforeProfile=profilePointer;uint8_t saved[sizeof(profile)];std::memcpy(saved,profile,sizeof(saved));
    ActiveSelectionObserver::Snapshot baseline{},after{};
    Check(observer.Read(101,&baseline) && baseline.inventory_matches && baseline.slot==4 &&
          baseline.uid==UINT64_C(0x1234567800000042),"64-bit identity and packed slot are read faithfully");
    constexpr uint32_t previewCandidate=10001;
    Check(baseline.unit!=previewCandidate &&
          baseline.MatchesOwnedSelection(101,UINT64_C(0x1234567800000042)),
          "an unowned preview candidate is independent of the sealed equipped UID and its real inventory identity");
    auto inconsistent=baseline;inconsistent.inventory_uid++;
    Check(!inconsistent.MatchesOwnedSelection(101,UINT64_C(0x1234567800000042)),
          "the sealed equipped UID must also match the real inventory record");
    inconsistent=baseline;inconsistent.inventory_unit++;
    Check(!inconsistent.MatchesOwnedSelection(101,UINT64_C(0x1234567800000042)),
          "the equipped unit number must match the real inventory record");
    inconsistent=baseline;inconsistent.inventory_slot++;
    Check(!inconsistent.MatchesOwnedSelection(101,UINT64_C(0x1234567800000042)),
          "the equipped slot must match the real inventory record");
    Check(!baseline.MatchesOwnedSelection(102,UINT64_C(0x1234567800000042)) &&
          !baseline.MatchesOwnedSelection(101,UINT64_C(0x1234567800000043)),
          "foreign pilots and unsealed equipped UIDs are rejected");
    Put(profile,0x1A9,uint32_t{15019});
    Check(observer.Read(101,&after) && !after.inventory_matches && !baseline.SameSelection(after),
          "a stale temporary profile is observable, not mislabeled unavailable or restored");
    Put(profile,0x1A9,uint32_t{22097});
    Check(observer.Read(101,&after) && after.inventory_matches && baseline.SameSelection(after),"genuine restored identity matches baseline");
    int before=calls;Check(!observer.Read(999,&after) && calls==before,"foreign profile is never consumed");
    inventory[0x20]=0;Check(!observer.Read(101,&after) && calls==before,"incomplete native inventory waits without getter");inventory[0x20]=1;
    profilePointer=nullptr;Check(!observer.Read(101,&after),"missing native account");profilePointer=beforeProfile;
    Put(inventory,0x0C,int32_t{2});Check(!observer.Read(101,&after),"out-of-capacity active slot");Put(inventory,0x0C,int32_t{1200});
    Check(!std::memcmp(saved,profile,sizeof(saved)),"observation never changes native account bytes");
    std::printf("active selection observer: %d failures\n",failures);return failures?1:0;
}
