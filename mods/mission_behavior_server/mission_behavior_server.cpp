#include <windows.h>
#include <array>
#include <cstddef>
#include <mutex>
#include <set>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "sdgo/mission_behavior_profile.h"
#include "sdgo/mission_behavior_server_observer.h"
#include "native_rows.h"

namespace {
using namespace sdgo::mission_behavior;
using FindRow=int(__thiscall*)(void*,std::uint32_t,std::uint8_t**);
using CreateNpc=int(__thiscall*)(
    void*,void*,const void*,void*,const void*);
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
void* g_manager=nullptr;
FindRow g_find=nullptr;
FindRow g_findNpc=nullptr;
CreateNpc g_createNpc=nullptr;
void* g_originalReady=nullptr;
void* g_rejectedReady=nullptr;
Bundle g_bundle;
Profile g_profile=Profile::Oc;
std::mutex g_mutex;
SdgoMissionBehaviorServerSnapshotV1 g_status{};
const DWORD g_defaultMxcsr=0x1f80;
SRWLOCK g_creationLock=SRWLOCK_INIT;
SdgoMissionBehaviorServerCreationV1 g_creation{
    sizeof(SdgoMissionBehaviorServerCreationV1),1};

bool Accessible(const void* pointer,std::size_t size,bool writing) {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at || at+size<at)return false;
    const auto end=at+size;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) ||
            info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto protection=info.Protect&0xff;
        if(writing && protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY &&
            protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto next=reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize;
        if(next<=at)return false;
        at=next;
    }
    return true;
}
bool ReadNative(const void* pointer,NativeNpcRow* row) {
    if(!Accessible(pointer,sizeof(*row),false))return false;
    __try {std::memcpy(row,pointer,sizeof(*row));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReadU32(const void* pointer,std::size_t offset,std::uint32_t& value) {
    if(!pointer){value=0;return false;}
    const auto* bytes=static_cast<const std::uint8_t*>(pointer);
    if(!Accessible(bytes+offset,sizeof(value),false))return false;
    __try {std::memcpy(&value,bytes+offset,sizeof(value));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){value=0;return false;}
}
bool WriteNative(void* pointer,const NpcRow* row,std::uint32_t mask) {
    if(!Accessible(pointer,sizeof(NativeNpcRow),true))return false;
    __try {ApplyNative(*row,mask,*static_cast<NativeNpcRow*>(pointer));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Lookup(std::uint32_t id,std::uint8_t** pointer,NpcRow& row) {
    if(!g_manager || !g_find || !g_find(g_manager,id,pointer) || !*pointer)return false;
    NativeNpcRow native{};
    if(!ReadNative(*pointer,&native))return false;
    row=DecodeNative(native);
    return row.id==id && ValidRow(row);
}
std::uint32_t RowsCrc(const std::vector<NpcRow>& rows) {
    return Crc32(reinterpret_cast<const std::uint8_t*>(rows.data()),rows.size()*sizeof(NpcRow));
}

// Runs in the native World table-download handler before it sets ready and
// registers the time procedure. No rooms can consume a partially changed set.
int __cdecl TablesReady() {
    std::lock_guard<std::mutex> guard(g_mutex);
    ++g_status.ready_epochs;
    std::vector<std::uint8_t*> nodes;
    std::vector<NpcRow> before;
    nodes.reserve(g_bundle.entries.size());before.reserve(g_bundle.entries.size());
    std::uint32_t matches=0,ocMatches=0;
    for(const auto& entry:g_bundle.entries){
        std::uint8_t* node=nullptr;NpcRow row{};
        if(!Lookup(entry.baseline.id,&node,row)){
            g_status.conflict_id=entry.baseline.id;
            if(g_profile==Profile::Oc)goto reject;
            continue;
        }
        if(SameRow(row,entry.baseline))++matches;
        if(SameRow(row,entry.oc))++ocMatches;
        if(!SameRow(row,entry.baseline) && !g_status.conflict_id)
            g_status.conflict_id=entry.baseline.id;
        nodes.push_back(node);before.push_back(row);
    }
    if(g_profile==Profile::Oc && g_status.state==1 && ocMatches==g_bundle.entries.size()){
        g_status.conflict_id=0;
        return 1;
    }
    g_status.baseline_matches=matches;
    if(g_profile==Profile::Gco){
        g_status.state=2;g_status.applied=0;g_status.conflict_id=0;
        g_status.native_rows_crc32=RowsCrc(before);
        g_api->Log(g_mod,SDGO_LOG_INFO,"GCO ready: native task data retained; %u reference rows verified",matches);
        return 1;
    }
    if(matches!=g_bundle.entries.size())goto reject;
    {
        std::set<std::uint32_t> checked;
        for(std::size_t i=0;i<nodes.size();++i){
            const auto& entry=g_bundle.entries[i];
            std::uint8_t* npc=nullptr;
            if(!Accessible(nodes[i],sizeof(NativeNpcRow),true) ||
                (checked.insert(entry.oc.npc).second &&
                 (!g_findNpc(g_manager,entry.oc.npc,&npc) || !npc))){
                g_status.conflict_id=entry.baseline.id;goto reject;
            }
        }
        std::size_t written=0;
        for(;written<nodes.size();++written){
            const auto& entry=g_bundle.entries[written];
            if(!WriteNative(nodes[written],&entry.oc,entry.mask))break;
        }
        std::vector<NpcRow> readback;readback.reserve(nodes.size());
        if(written==nodes.size())for(std::size_t i=0;i<nodes.size();++i){
            NativeNpcRow native{};
            if(!ReadNative(nodes[i],&native) || !SameRow(DecodeNative(native),g_bundle.entries[i].oc))break;
            readback.push_back(DecodeNative(native));
        }
        if(written!=nodes.size() || readback.size()!=nodes.size()){
            const auto rollback=written<nodes.size()?written+1:written;
            bool reverted=true;
            for(std::size_t i=0;i<rollback;++i){
                NativeNpcRow native{};
                if(!WriteNative(nodes[i],&before[i],g_bundle.entries[i].mask) ||
                    !ReadNative(nodes[i],&native) || !SameRow(DecodeNative(native),before[i]))
                    reverted=false;
            }
            if(!reverted){
                g_status.state=4;g_status.applied=0;g_status.native_rows_crc32=0;
                g_api->Log(g_mod,SDGO_LOG_ERROR,"task profile rollback incomplete; native table readiness refused");
                return 0;
            }
            goto reject;
        }
        g_status.state=1;g_status.applied=static_cast<std::uint32_t>(written);
        g_status.native_rows_crc32=RowsCrc(readback);g_status.conflict_id=0;
        g_api->Log(g_mod,SDGO_LOG_INFO,"OC ready: %u task rows applied atomically; native row CRC32=%08lX",g_status.applied,static_cast<unsigned long>(g_status.native_rows_crc32));
        return 1;
    }
reject:
    g_status.state=3;g_status.applied=0;
    g_status.baseline_matches=matches;g_status.native_rows_crc32=0;
    g_api->Log(g_mod,SDGO_LOG_ERROR,"OC profile baseline conflict at task row %lu; table readiness refused; import a matching owner task configuration",static_cast<unsigned long>(g_status.conflict_id));
    return 0;
}

__declspec(naked) void ReadySite() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        sub esp,220h
        and esp,0fffffff0h
        mov [esp+200h],eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_defaultMxcsr]
        cld
        call TablesReady
        mov [esp+204h],eax
        fxrstor [esp]
        mov eax,[esp+204h]
        mov esp,[esp+200h]
        test eax,eax
        jz rejected
        popad
        popfd
        jmp dword ptr [g_originalReady]
    rejected:
        popad
        popfd
        jmp dword ptr [g_rejectedReady]
    }
}

int __fastcall CreateMissionNpc(
    void* room,
    void*,
    void* battlePilot,
    const void* npcData,
    void* battleUnit,
    const void* missionNpc)
{
    NativeNpcRow native{};
    const bool copied=ReadNative(missionNpc,&native);
    const int result=g_createNpc(
        room,battlePilot,npcData,battleUnit,missionNpc);
    if(!result || !copied)return result;

    const auto row=DecodeNative(native);
    const auto* entry=FindOverride(g_bundle,row.id);
    const auto* expected=entry
        ? (g_profile==Profile::Oc?&entry->oc:&entry->baseline)
        : nullptr;
    SdgoMissionBehaviorServerCreationV1 receipt{};
    receipt.size=sizeof(receipt);
    receipt.version=1;
    receipt.profile=g_profile==Profile::Oc?1u:2u;
    receipt.original_result=static_cast<std::uint32_t>(result);
    receipt.tracked=entry?1u:0u;
    receipt.profile_match=!expected||SameRow(row,*expected)?1u:0u;
    receipt.mission_npc_id=row.id;
    receipt.mission_id=row.mission;
    receipt.difficulty=row.difficulty;
    receipt.part=row.part;
    receipt.npc_id=row.npc;
    receipt.x=row.x;receipt.y=row.y;receipt.z=row.z;
    receipt.after_time=row.after_time;
    receipt.abs_time=row.abs_time;
    receipt.manual_id=row.manual;
    receipt.party=row.party;
    receipt.create_limit=row.create_limit;
    receipt.zone=row.zone;
    ReadU32(battlePilot,0,receipt.native_pilot_id);
    ReadU32(battleUnit,0x34,receipt.native_unit_hp);

    AcquireSRWLockExclusive(&g_creationLock);
    receipt.sequence=g_creation.sequence+1;
    g_creation=receipt;
    ReleaseSRWLockExclusive(&g_creationLock);
    return result;
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoMissionBehaviorServerObserveV1(
    SdgoMissionBehaviorServerSnapshotV1* output,std::uint32_t bytes) {
    if(!output || bytes<sizeof(*output))return 0;
    std::lock_guard<std::mutex> guard(g_mutex);
    *output=g_status;return 1;
}
extern "C" __declspec(dllexport) int __cdecl SdgoMissionBehaviorServerReadRowV1(
    std::uint32_t id,sdgo::mission_behavior::NpcRow* output,std::uint32_t bytes) {
    if(!output || bytes<sizeof(*output))return 0;
    std::lock_guard<std::mutex> guard(g_mutex);
    if(g_status.state!=1 && g_status.state!=2)return 0;
    std::uint8_t* pointer=nullptr;
    return Lookup(id,&pointer,*output)?1:0;
}
extern "C" __declspec(dllexport) int __cdecl
SdgoMissionBehaviorServerGetCreationV1(
    SdgoMissionBehaviorServerCreationV1* output,
    std::uint32_t bytes)
{
    if(!output || bytes<sizeof(*output))return 0;
    __try {
        AcquireSRWLockShared(&g_creationLock);
        const auto snapshot=g_creation;
        ReleaseSRWLockShared(&g_creationLock);
        *output=snapshot;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    g_status.size=sizeof(g_status);g_status.version=1;
    const sdgo::ModSettings settings(api,mod);
    char selected[16]{};settings.String("Profile","oc",selected,sizeof(selected));
    if(!ParseProfile(selected,g_profile)){
        api->Log(mod,SDGO_LOG_ERROR,"invalid task profile; select OC or GCO in Mod Manager");return 0;
    }
    g_status.profile=g_profile==Profile::Oc?1:2;
    AcquireSRWLockExclusive(&g_creationLock);
    g_creation={};
    g_creation.size=sizeof(g_creation);
    g_creation.version=1;
    g_creation.profile=g_status.profile;
    ReleaseSRWLockExclusive(&g_creationLock);
    std::string error;
    const auto path=std::filesystem::path(api->ModDirectory(mod))/L"profile.bin";
    const bool profileLoaded=LoadProfileFile(path,g_bundle,error);
    if(g_profile==Profile::Oc &&
       (!profileLoaded ||
        !(g_bundle.header.flags&kNpcOverrides) ||
        g_bundle.entries.empty())){
        if(profileLoaded)
            error="owner mission profile has no server task-row differences";
        api->Log(mod,SDGO_LOG_ERROR,"%s; import the server owner's task configuration",error.c_str());return 0;
    }
    g_status.entries=static_cast<std::uint32_t>(g_bundle.entries.size());
    g_status.payload_crc32=g_bundle.header.payload_crc32;
    g_manager=api->Symbol(mod,"catalog.npc_manager");
    g_find=reinterpret_cast<FindRow>(api->Symbol(mod,"mission_behavior.server.find_npc_row"));
    g_findNpc=reinterpret_cast<FindRow>(api->Symbol(mod,"catalog.find_ai_npc"));
    auto* site=static_cast<std::uint8_t*>(api->Symbol(mod,"mission_behavior.server.tables_ready"));
    g_rejectedReady=api->Symbol(mod,"mission_behavior.server.tables_rejected");
    g_createNpc=reinterpret_cast<CreateNpc>(
        api->Symbol(mod,"mission_behavior.server.create_npc"));
    if(!g_manager || !g_find || !g_findNpc || !site ||
       !g_rejectedReady || !g_createNpc)return 0;
    const auto expected=reinterpret_cast<std::uintptr_t>(api->host_base)+0x435c4c;
    std::uint32_t address=0,value=0;
    std::memcpy(&address,site+2,4);std::memcpy(&value,site+6,4);
    if(site[0]!=0xc7 || site[1]!=0x05 || address!=expected || value!=1){
        api->Log(mod,SDGO_LOG_ERROR,"native table-ready site differs; profile not installed");return 0;
    }
    if(api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite) ||
       !api->HookCode || !api->HookSite ||
       !api->HookCode(
           mod,
           "mission_behavior.server.create_npc",
           reinterpret_cast<void*>(&CreateMissionNpc),
           reinterpret_cast<void**>(&g_createNpc)) ||
       !api->HookSite(
           mod,
           "mission_behavior.server.tables_ready",
           reinterpret_cast<void*>(&ReadySite),
           &g_originalReady))return 0;
    api->Log(mod,SDGO_LOG_INFO,"task profile %s armed; %u owner-provided row deltas; real mission NPC creation receipts enabled; no SQL writes",selected,g_status.entries);
    return 1;
}
