#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/mission_behavior_profile.h"
#include "sdgo/mission_behavior_server_observer.h"
#include "mods/mission_behavior_server/native_rows.h"

using namespace sdgo::mission_behavior;
namespace {
std::map<std::uint32_t,std::array<std::uint8_t,52>> nodes;
std::vector<std::uint8_t> image;
std::wstring directory;
std::string selected;
void* detour=nullptr;
void* createDetour=nullptr;
int continued=0,rejected=0;
void __cdecl Continue(){++continued;}
void __cdecl Rejected(){++rejected;}
void Log(SdgoMod*,int,const char*,...){}
std::uint32_t Setting(SdgoMod*,const char*,const char* fallback,char* out,std::uint32_t capacity){
    const auto value=selected=="default"?std::string(fallback):selected;
    if(value.size()+1>capacity)return 0;
    std::memcpy(out,value.c_str(),value.size()+1);return static_cast<std::uint32_t>(value.size());
}
const wchar_t* Directory(SdgoMod*){return directory.c_str();}
int __fastcall Find(void*,void*,std::uint32_t id,std::uint8_t** out){
    auto it=nodes.find(id);if(it==nodes.end())return 0;*out=it->second.data();return 1;
}
int __fastcall FindNpc(void*,void*,std::uint32_t,std::uint8_t** out){static std::uint8_t data[64]{};*out=data;return 1;}
int __fastcall Create(
    void*,void*,void* pilot,const void*,void* unit,const void*) {
    *static_cast<std::uint32_t*>(pilot)=0x80000021;
    *reinterpret_cast<std::uint32_t*>(
        static_cast<std::uint8_t*>(unit)+0x34)=4321;
    return 1;
}
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"catalog.npc_manager"))return &nodes;
    if(!std::strcmp(name,"catalog.find_ai_npc"))return reinterpret_cast<void*>(&FindNpc);
    if(!std::strcmp(name,"mission_behavior.server.find_npc_row"))return reinterpret_cast<void*>(&Find);
    if(!std::strcmp(name,"mission_behavior.server.tables_ready"))return image.data()+0x4b5a7;
    if(!std::strcmp(name,"mission_behavior.server.tables_rejected"))return reinterpret_cast<void*>(&Rejected);
    if(!std::strcmp(name,"mission_behavior.server.create_npc"))return reinterpret_cast<void*>(&Create);
    return nullptr;
}
int HookSite(SdgoMod*,const char*,void* target,void** original){
    detour=target;*original=reinterpret_cast<void*>(&Continue);return 1;
}
int HookCode(SdgoMod*,const char* name,void* target,void** original){
    if(std::strcmp(name,"mission_behavior.server.create_npc"))return 0;
    createDetour=target;*original=reinterpret_cast<void*>(&Create);return 1;
}
bool Check(bool value,const char* text){if(!value)std::fprintf(stderr,"FAIL: %s\n",text);return value;}
}

int main(int argc,char** argv){
    if(argc!=3)return 2;
    const std::string test=argv[2];
    if(test!="default" && test!="oc" && test!="gco" && test!="conflict" && test!="corrupt")return 2;
    selected=(test=="gco"||test=="default")?test:"oc";
    const auto root=std::filesystem::current_path()/"mission-behavior-fixture"/test;
    std::filesystem::create_directories(root);directory=root.wstring();
    std::array<NpcOverride,2> entries{};
    for(std::size_t i=0;i<entries.size();++i){
        auto& e=entries[i];e.baseline={520001+static_cast<std::uint32_t>(i),88009,2,1,85063,
            100,0,200,5,0,0,2,1,0};e.oc=e.baseline;e.oc.npc=85064;e.oc.x=-100;e.oc.after_time=7;
        e.mask=kChangeNpc|kChangeX|kChangeDelay;
        NativeNpcRow n{e.baseline.id,e.baseline.mission,e.baseline.difficulty,e.baseline.part,
            e.baseline.npc,e.baseline.x,e.baseline.y,e.baseline.z,5,0,0,2,1,0};
        std::memcpy(nodes[e.baseline.id].data(),&n,sizeof(n));
    }
    Header h{};std::memcpy(h.magic,"SDGOMBP1",8);h.version=1;h.header_bytes=sizeof(h);
    h.flags=kBossBindings|kNpcOverrides;h.entry_bytes=sizeof(NpcOverride);h.entry_count=2;
    h.payload_bytes=sizeof(entries);h.payload_crc32=Crc32(reinterpret_cast<const std::uint8_t*>(entries.data()),sizeof(entries));h.bindings_generation=1;
    if(test=="corrupt")++h.payload_crc32;
    {std::ofstream f(root/"profile.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&h),sizeof(h));f.write(reinterpret_cast<const char*>(entries.data()),sizeof(entries));}
    if(test=="conflict")reinterpret_cast<NativeNpcRow*>(nodes.begin()->second.data())->x=999;
    const auto before=nodes;
    image.resize(0x436000);auto* site=image.data()+0x4b5a7;site[0]=0xc7;site[1]=0x05;
    const auto destination=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(image.data())+0x435c4c);
    const std::uint32_t one=1;std::memcpy(site+2,&destination,4);std::memcpy(site+6,&one,4);
    HMODULE module=LoadLibraryA(argv[1]);if(!module)return 3;
    auto load=reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module,"SdgoModLoad"));
    using Observe=int(__cdecl*)(SdgoMissionBehaviorServerSnapshotV1*,std::uint32_t);
    auto observe=reinterpret_cast<Observe>(GetProcAddress(module,"SdgoMissionBehaviorServerObserveV1"));
    using ObserveCreate=int(__cdecl*)(SdgoMissionBehaviorServerCreationV1*,std::uint32_t);
    auto observeCreate=reinterpret_cast<ObserveCreate>(
        GetProcAddress(module,SDGO_MISSION_BEHAVIOR_SERVER_CREATION_EXPORT));
    SdgoApi api{};api.size=sizeof(api);api.version=1;api.host_key="BattleServer-548A891D-0046C000";
    api.host_base=image.data();api.Log=&Log;api.Symbol=&Symbol;api.SettingString=&Setting;
    api.ModDirectory=&Directory;api.HookCode=&HookCode;api.HookSite=&HookSite;
    bool ok=Check(load && observe && observeCreate,"real module exports");
    if(!ok)return 4;
    const auto active=load(&api,reinterpret_cast<SdgoMod*>(1));
    if(test=="corrupt")ok=Check(!active&&!detour&&!createDetour&&nodes==before,"corrupt owner data cannot install hooks or alter rows");
    else{
        ok=Check(active&&detour&&createDetour,"real module installs verified ready and creation hooks")&&ok;
        if(ok)reinterpret_cast<void(__cdecl*)()>(detour)();
        SdgoMissionBehaviorServerSnapshotV1 state{};ok=Check(observe(&state,sizeof(state))!=0,"read-only observer")&&ok;
        if(test=="conflict")ok=Check(rejected==1&&!continued&&state.state==3&&nodes==before,"all-or-nothing baseline rejection")&&ok;
        else if(test=="gco")ok=Check(continued==1&&!rejected&&state.profile==2&&state.state==2&&!state.applied&&nodes==before,"GCO never changes native rows")&&ok;
        else{
            ok=Check(continued==1&&!rejected&&state.profile==1&&state.state==1&&state.applied==2,"OC/default applies before native readiness")&&ok;
            for(const auto& entry:entries){NativeNpcRow n{};std::memcpy(&n,nodes[entry.baseline.id].data(),sizeof(n));ok=Check(SameRow(DecodeNative(n),entry.oc),"native packed row fields match profile")&&ok;}
            reinterpret_cast<void(__cdecl*)()>(detour)();
            ok=Check(continued==2&&!rejected,"duplicate ready acknowledgement is idempotent")&&ok;
        }
        if(ok && test!="conflict") {
            NativeNpcRow row{};
            std::memcpy(&row,nodes.begin()->second.data(),sizeof(row));
            std::array<std::uint8_t,64> pilot{},unit{};
            using CreateDetour=int(__fastcall*)(
                void*,void*,void*,const void*,void*,const void*);
            const auto created=reinterpret_cast<CreateDetour>(createDetour)(
                reinterpret_cast<void*>(1),nullptr,pilot.data(),
                reinterpret_cast<void*>(2),unit.data(),&row);
            SdgoMissionBehaviorServerCreationV1 receipt{};
            ok=Check(
                created==1 &&
                observeCreate(&receipt,sizeof(receipt))!=0 &&
                receipt.sequence==1 &&
                receipt.profile==(test=="gco"?2u:1u) &&
                receipt.original_result==1 &&
                receipt.tracked==1 &&
                receipt.profile_match==1 &&
                receipt.mission_npc_id==row.id &&
                receipt.mission_id==row.mission &&
                receipt.npc_id==row.npc &&
                receipt.x==row.x && receipt.y==row.y && receipt.z==row.z &&
                receipt.native_pilot_id==0x80000021 &&
                receipt.native_unit_hp==4321,
                "successful original creation publishes the exact consumed native row"
            )&&ok;
        }
    }
    FreeLibrary(module);
    if(ok){std::filesystem::remove(root/"profile.bin");std::filesystem::remove(root);std::puts("PASS: real server module, native row ABI and ready-site contract");}
    return ok?0:1;
}
