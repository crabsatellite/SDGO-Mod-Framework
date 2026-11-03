// Offline ABI/control fixture. These spies are not game rendering evidence.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/unit_catalog_format.h"
#include "../mods/unit_catalog/preview_runtime.h"

namespace fs=std::filesystem;
namespace {
constexpr uint32_t kUnit=25061, kCategory=1119000;
std::wstring g_directory;
std::map<std::string,void*> g_hooks;
std::array<uint8_t,0x300> g_shop{},g_capsule{},g_panel{},g_drop{},g_pane{};
std::array<std::array<uint8_t,0x200>,32> g_controls{};
std::array<uint8_t,0x3fb> g_row{};
std::array<uint8_t,0x450> g_custom{};
std::array<uint8_t,18> g_customRow{};
uint32_t g_eventVtable[8]{};
bool g_modelSucceeds=true,g_dispatch=true;
unsigned g_modelCalls=0,g_nativeDispatches=0,g_checks=0;
unsigned g_sent=0;

void Check(bool value,const char* message){++g_checks;if(!value){std::fprintf(stderr,"FAIL %s\n",message);std::exit(2);}}
template<class T>void Store(uint8_t* where,T value){std::memcpy(where,&value,sizeof(value));}
template<class T>T Load(const uint8_t* where){T value{};std::memcpy(&value,where,sizeof(value));return value;}
std::string Read(const wchar_t* name){std::ifstream f(fs::path(g_directory)/name);return std::string(std::istreambuf_iterator<char>(f),{});}
bool Success(const wchar_t* name){return Read(name).find("\"native_logic\":true")!=std::string::npos;}

struct HostText{union{wchar_t buffer[8];const wchar_t* pointer;};uint32_t size,capacity;};
static_assert(sizeof(HostText)==24);
using Slot=void(__fastcall*)(void*,void*,const HostText*,uintptr_t);
using Dispatch=void(__fastcall*)(void*,void*,void*,uint8_t);

void __fastcall OriginalModal(uint8_t* self,void*,uint32_t){
    for(size_t offset=0x68,index=0;offset<=0xd0;offset+=4,++index){
        uint8_t* control=offset==0x8c?g_pane.data():g_controls[index].data();
        Store(self+offset,control);control[0x162]=1;
    }
}
void __fastcall OriginalButtons(uint8_t* self,void*){
    Load<uint8_t*>(self+0xcc)[0x162]=1;Load<uint8_t*>(self+0xd0)[0x162]=1;
}
void __fastcall OriginalUnitView(uint8_t* self,void*,uint32_t unit){
    Check(unit==kUnit,"original native unit-render method receives candidate ID");
    ++g_modelCalls;if(g_modelSucceeds)Load<uint8_t*>(self+0x8c)[0x162]=1;
}
void __fastcall OriginalDispatch(void*,void*,void*,uint8_t){++g_nativeDispatches;}
void __fastcall OriginalSlot(void*,void*,const HostText*,uintptr_t){
    if(!g_dispatch)return;
    std::array<uint8_t,64> event{};
    Store(event.data(),static_cast<void*>(g_eventVtable));Store(event.data()+4,static_cast<uint16_t>(0x1f8));
    Store(event.data()+0x2c,Load<uint32_t>(g_row.data()+0x1b));
    reinterpret_cast<Dispatch>(g_hooks.at("preview.capsule.event_dispatch"))(g_panel.data(),nullptr,event.data(),1);
}
void __fastcall OriginalCapsuleInit(void*,void*){g_drop[0x162]=1;}
void __fastcall OriginalCapsuleVisibility(void*,void*,void*){g_drop[0x162]=1;}
void __fastcall OriginalCapsuleMode(void*,void*,int){g_drop[0x162]=1;}
void __fastcall OriginalCustom(void*,void*,void*){
    if(!g_dispatch)return;
    std::array<uint8_t,64> event{};Store(event.data(),static_cast<void*>(g_eventVtable));
    Store(event.data()+4,static_cast<uint16_t>(0x1f8));Store(event.data()+0x2c,Load<uint32_t>(g_customRow.data()+9));
    reinterpret_cast<Dispatch>(g_hooks.at("preview.capsule.event_dispatch"))(g_panel.data(),nullptr,event.data(),1);
}
int __fastcall OriginalSend(void*,void*,void*,const void*,int,char){++g_sent;return 1;}
void* __fastcall FindControl(void* panel,void*,const HostText* name,uint32_t type){
    Check(panel==g_panel.data()&&type==4,"stock capsule control lookup receives original panel/type");
    const auto* text=name->capacity<8?name->buffer:name->pointer;
    Check(std::wstring(text,name->size)==L"CapsuleDropBtn","stock lookup uses native drop-button name");
    return g_drop.data();
}
const wchar_t* Directory(SdgoMod*){return g_directory.c_str();}
void Log(SdgoMod*,int,const char*,...){ }
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"unit_preview.shop_unit_view"))return reinterpret_cast<void*>(&OriginalUnitView);
    if(!std::strcmp(name,"preview.capsule.find_control"))return reinterpret_cast<void*>(&FindControl);
    if(!std::strcmp(name,"preview.capsule.unitview_event_vtable"))return g_eventVtable;
    return nullptr;
}
int Hook(SdgoMod*,const char* name,void* detour,void** original){
    g_hooks[name]=detour;
    if(!std::strcmp(name,"unit_preview.shop_modal_init"))*original=reinterpret_cast<void*>(&OriginalModal);
    else if(!std::strcmp(name,"unit_preview.shop_set_buttons"))*original=reinterpret_cast<void*>(&OriginalButtons);
    else if(!std::strcmp(name,"preview.capsule.slot_event"))*original=reinterpret_cast<void*>(&OriginalSlot);
    else if(!std::strcmp(name,"preview.capsule.event_dispatch"))*original=reinterpret_cast<void*>(&OriginalDispatch);
    else if(!std::strcmp(name,"preview.capsule.ui_init"))*original=reinterpret_cast<void*>(&OriginalCapsuleInit);
    else if(!std::strcmp(name,"preview.capsule.visibility_update"))*original=reinterpret_cast<void*>(&OriginalCapsuleVisibility);
    else if(!std::strcmp(name,"preview.capsule.mode_visibility_update"))*original=reinterpret_cast<void*>(&OriginalCapsuleMode);
    else if(!std::strcmp(name,"preview.capsule.custom_unitview"))*original=reinterpret_cast<void*>(&OriginalCustom);
    else if(!std::strcmp(name,"unit_preview.network_send"))*original=reinterpret_cast<void*>(&OriginalSend);
    else return 0;
    return 1;
}
}

int main(){
    using namespace sdgo::unit_catalog;
    const fs::path directory=fs::temp_directory_path()/(L"sdgo-native-preview-fixture-"+std::to_wstring(GetCurrentProcessId()));
    Check(fs::create_directory(directory),"fixture exclusively creates its own temp directory");
    g_directory=directory.wstring();
    {std::ofstream out(directory/L"native-preview.ini");out<<"[preview]\nunit_id="<<kUnit<<"\ncategory_id="<<kCategory
        <<"\ncontent_revision="<<std::string(64,'a')<<"\nnonce="<<std::string(64,'b')<<"\n";}
    Catalog catalog;Entry unit;unit.kind=kUnitType;unit.id=kUnit;unit.bytes.resize(kUnitBytes);
    Entry item;item.kind=kItemCategoryType;item.id=kCategory;item.bytes.resize(kItemCategoryBytes);
    Put32(item.bytes.data()+kItemCategoryRefOffset,kUnit);catalog.records={unit,item};
    SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Log=&Log;api.Symbol=&Symbol;api.HookCode=&Hook;api.ModDirectory=&Directory;
    Check(sdgo::unit_preview::Attach(&api,reinterpret_cast<SdgoMod*>(1),catalog),"private mode installs only the verified native adapters");
    using Send=int(__fastcall*)(void*,void*,void*,const void*,int,char);
    const auto send=reinterpret_cast<Send>(g_hooks.at("unit_preview.network_send"));
    constexpr std::array<uint16_t,8> blocked={0x0931,0x0758,0x06cc,0x0514,0x05a8,0x05d3,0x09b0,0x072e};
    for(const uint16_t opcode:blocked){
        Check(send(nullptr,nullptr,nullptr,&opcode,2,0)==0,"private preview suppresses a verified spending request before the original transport");
    }
    Check(g_sent==0,"no blocked request reaches original transport");
    const uint16_t refresh=0x082f;
    Check(send(nullptr,nullptr,nullptr,&refresh,2,0)==1&&g_sent==1,"non-committing helper packets preserve the original transport path");
    using Modal=void(__fastcall*)(uint8_t*,void*,uint32_t);
    reinterpret_cast<Modal>(g_hooks.at("unit_preview.shop_modal_init"))(g_shop.data(),nullptr,42);
    Check(g_modelCalls==1&&Success(L"native-preview-shop.json"),"receipt follows actual original-call spy and visible candidate pane");
    Check(g_controls[0][0x162]==0&&Load<uint8_t*>(g_shop.data()+0xcc)[0x162]==0&&Load<uint8_t*>(g_shop.data()+0xd0)[0x162]==0,"foreign SKU metadata and modal spending controls stay hidden");
    reinterpret_cast<void(__fastcall*)(uint8_t*,void*)>(g_hooks.at("unit_preview.shop_set_buttons"))(g_shop.data(),nullptr);
    Check(Load<uint8_t*>(g_shop.data()+0xcc)[0x162]==0,"native balance/button refresh cannot expose modal buy");
    g_modelSucceeds=false;
    reinterpret_cast<Modal>(g_hooks.at("unit_preview.shop_modal_init"))(g_shop.data(),nullptr,42);
    Check(!Success(L"native-preview-shop.json"),"failed original renderer cannot retain an earlier success receipt");
    Store(g_capsule.data()+0x74,static_cast<void*>(g_panel.data()));Store(g_capsule.data()+0x64,static_cast<void*>(g_row.data()));
    Store(g_capsule.data()+0x80,uint32_t{0});Store(g_capsule.data()+0x1a0,uint32_t{1});Put32(g_row.data()+0x0b,6);
    reinterpret_cast<void(__fastcall*)(void*,void*)>(g_hooks.at("preview.capsule.ui_init"))(g_capsule.data(),nullptr);
    Check(g_drop[0x162]==0,"actual drop control hidden after native UI initialization");
    HostText name{};name.pointer=L"UnitRadio0";name.size=10;name.capacity=10;
    Put32(g_row.data()+0x1b,kUnit+1);
    const auto slot=reinterpret_cast<Slot>(g_hooks.at("preview.capsule.slot_event"));
    slot(g_capsule.data(),nullptr,&name,0);
    Check(!fs::exists(directory/L"native-preview-capsule.json"),"another unit's original event is not candidate evidence");
    Put32(g_row.data()+0x1b,kUnit);slot(g_capsule.data(),nullptr,&name,0);
    Check(g_nativeDispatches==2&&Success(L"native-preview-capsule.json"),"target's original unitview dispatch produces role-specific receipt");
    g_dispatch=false;slot(g_capsule.data(),nullptr,&name,0);
    Check(!Success(L"native-preview-capsule.json"),"missing native unitview dispatch invalidates the last receipt");
    g_dispatch=true;
    Store(g_custom.data()+0x78,static_cast<void*>(g_pane.data()));
    Store(g_custom.data()+0x3b0,static_cast<void*>(g_customRow.data()));
    Store(g_custom.data()+0x3b4,static_cast<void*>(g_customRow.data()+g_customRow.size()));
    Put32(g_custom.data()+0x428,0);Put32(g_customRow.data()+9,kUnit);
    const auto custom=reinterpret_cast<void(__fastcall*)(void*,void*,void*)>(g_hooks.at("preview.capsule.custom_unitview"));
    custom(g_custom.data(),nullptr,g_pane.data());
    Check(Success(L"native-preview-capsule.json")&&Read(L"native-preview-capsule.json").find("custom-machine")!=std::string::npos,
          "custom SR candidate follows existing native unitview instead of a fabricated fixed card");
    Put32(g_custom.data()+0x428,1);custom(g_custom.data(),nullptr,g_pane.data());
    Check(g_nativeDispatches==4,"out-of-page custom control retains original call without treating it as target proof");
    for(const wchar_t* nameToRemove:{L"native-preview.ini",L"native-preview-shop.json",L"native-preview-capsule.json"})fs::remove(directory/nameToRemove);
    fs::remove(directory);
    std::printf("PASS %u offline native adapter checks (synthetic controls/call spies; not game UI rendering)\n",g_checks);
    return 0;
}
