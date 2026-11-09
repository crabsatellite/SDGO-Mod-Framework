#include <windows.h>
#include <intrin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/hangar_preview_guard_observer.h"

namespace {
using Byte=uint8_t;
template<class T>T Read(const void* p,size_t offset=0) {
    T value{};std::memcpy(&value,static_cast<const Byte*>(p)+offset,sizeof(value));return value;
}
template<class T>void Write(void* p,size_t offset,T value) {
    std::memcpy(static_cast<Byte*>(p)+offset,&value,sizeof(value));
}
enum Failure:uint32_t {None=0,MissingVertex=1,VertexLock=2,MissingIndex=3,IndexLock=4,ModelLoad=5};
struct Scope {
    Scope* prior=nullptr;void* row=nullptr;void* renderer=nullptr;
    uint32_t unit=0,slot=0;Failure failure=None;unsigned setter_depth=0,parser_depth=0;
    bool candidate_discarded=false,renderer_lock_released=false;
};
thread_local Scope* current=nullptr;
struct LoadFrame {LoadFrame* prior=nullptr;Scope* owner=nullptr;void* source=nullptr;bool parser_rejected=false;};
thread_local LoadFrame* current_load=nullptr;
template<class T>struct RestorePointer {
    T*& slot;T* prior;
    ~RestorePointer(){slot=prior;}
};
struct Depth {
    unsigned& value;
    explicit Depth(unsigned& depth):value(depth){++value;}
    ~Depth(){--value;}
};
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
using RowFn=void(__thiscall*)(void*,void*);
using SetFn=uint8_t(__thiscall*)(void*,const void*,const void*);
using ParseFn=void*(__thiscall*)(void*,void*,uint8_t);
using LoadFn=uint8_t(__thiscall*)(void*,const void*,const void*);
using CloseFn=int(__cdecl*)(void*);
using LeaveFn=void(__thiscall*)(void*);
using DeleteFn=void*(__thiscall*)(void*,unsigned);
using UnlockFn=long(__stdcall*)(void*);
RowFn original_row=nullptr;SetFn original_set=nullptr;ParseFn original_parse=nullptr;
LoadFn original_load=nullptr;
CloseFn close_source=nullptr;
LeaveFn leave_renderer=nullptr;void* row_set_return=nullptr;
void* vertex24_site=nullptr;void* vertex24_after_site=nullptr;
void* vertex32_site=nullptr;void* vertex32_after_site=nullptr;
void* index_site=nullptr;void* index_after_site=nullptr;
void* model_result_site=nullptr;void* row_result_site=nullptr;
void* vertex24_continue=nullptr;void* vertex32_continue=nullptr;
void* model_cleanup=nullptr;void* row_thumbnail=nullptr;
SdgoHangarPreviewGuardStatusV1 status{};
void Count(uint32_t& value){InterlockedIncrement(reinterpret_cast<volatile LONG*>(&value));}
bool Parsing(){return current&&current->setter_depth&&current->parser_depth;}
void Fail(Failure reason) {
    if(!current||current->failure!=None)return;
    current->failure=reason;
    switch(reason) {
    case MissingVertex:Count(status.missing_vertex);break;
    case VertexLock:Count(status.failed_vertex_lock);break;
    case MissingIndex:Count(status.missing_index);break;
    case IndexLock:Count(status.failed_index_lock);break;
    case ModelLoad:Count(status.failed_model_load);break;
    default:break;
    }
}
// The object was constructed and returned by the original allocator/reader.
// Its own scalar deleting destructor releases its native members and allocation.
void NativeDelete(void* object) {
    if(object)reinterpret_cast<DeleteFn>(Read<void**>(object)[0])(object,1);
}
void Unlock(void* object) {
    reinterpret_cast<UnlockFn>(Read<void**>(object)[12])(object);
}

void __fastcall Row(void* self,void*,void* row) {
    Scope scope{};scope.prior=current;scope.row=row;
    RestorePointer<Scope> restore{current,current};
    current=&scope;Count(status.rows);
    original_row(self,row);
    current=scope.prior;
    if(scope.failure!=None) {
        const auto error=GetLastError();
        status.last_unit=scope.unit;status.last_failed_slot=scope.slot;status.last_failure=scope.failure;
        if(status.fallbacks<=5||status.fallbacks%1000==0)
            g_api->Log(g_mod,SDGO_LOG_WARN,"unit %lu preview load rejected (%u); native thumbnail fallback used",
                static_cast<unsigned long>(scope.unit),static_cast<unsigned>(scope.failure));
        SetLastError(error);
    }
}
uint8_t __fastcall SetModel(void* self,void*,const void* name,const void* animation) {
    if(!current||_ReturnAddress()!=row_set_return||current->renderer) {
        // A reentrant, unrelated preview must not inherit an outer row's GPU
        // rejection scope. Its own original behavior and AL result are kept.
        RestorePointer<Scope> restore{current,current};current=nullptr;
        return original_set(self,name,animation);
    }
    Scope& scope=*current;scope.renderer=self;Count(status.previews);
    // Read the definition only after the original row has validated and used
    // it to reach this exact model-set call. Empty/2D row jobs stay untouched.
    if(scope.row) {
        if(const auto* definition=Read<const void*>(scope.row,4))scope.unit=Read<uint32_t>(definition);
        // The original row already used this zero-based inventory slot for
        // its one-based native lookup. Duplicate definitions in other cells
        // must not be counted as this failed cell's recovery.
        scope.slot=Read<uint16_t>(scope.row,8);
    }
    uint8_t result=0;
    {Depth depth(scope.setter_depth);result=original_set(self,name,animation);}
    if(scope.failure==None){
        Count(status.loaded);status.last_loaded_unit=scope.unit;
        if(scope.unit&&scope.unit==status.last_unit&&scope.slot==status.last_failed_slot)Count(status.recovered_previews);
        return result;
    }
    const auto error=GetLastError();
    // No activation was performed. The row still owns the native renderer CS.
    // On a guarded GPU error the parser returned null before registration or
    // animation instancing. Other false model-load results retain their native
    // constructed ownership and use the same destructor as the original setter.
    if(void* candidate=Read<void*>(self,0x34)) {
        NativeDelete(candidate);Write<void*>(self,0x34,nullptr);Count(status.discarded_models);
    }
    scope.candidate_discarded=true;SetLastError(error);return result;
}
void* __fastcall Parse(void* self,void*,void* source,uint8_t textures) {
    if(!current||!current->setter_depth||!current_load||current_load->owner!=current)
        return original_parse(self,source,textures);
    Scope& scope=*current;void* root=nullptr;
    {Depth depth(scope.parser_depth);root=original_parse(self,source,textures);}
    if(scope.failure==None)return root;
    const auto error=GetLastError();
    // BF0630 rejects a null root before it registers/instances it. Finish the
    // reader first, preserving stream position and native temporary cleanup.
    if(root){NativeDelete(root);Count(status.discarded_roots);}
    current_load->source=source;current_load->parser_rejected=true;
    SetLastError(error);return nullptr;
}
uint8_t __fastcall LoadModel(void* self,void*,const void* name,const void* animation) {
    if(current&&current->setter_depth&&!self){Fail(ModelLoad);return 0;}
    if(!current||!current->setter_depth)return original_load(self,name,animation);
    LoadFrame frame{};frame.prior=current_load;frame.owner=current;current_load=&frame;
    RestorePointer<LoadFrame> restore{current_load,frame.prior};
    const auto result=original_load(self,name,animation);
    // Native C5AF70 closes its source only on successful BF0630. Pair the
    // source only for the null parser result introduced by this Mod, after
    // that exact load invocation has finished using it.
    if(!result&&frame.parser_rejected&&frame.source) {
        const auto error=GetLastError();close_source(frame.source);Count(status.closed_sources);SetLastError(error);
    }
    current_load=frame.prior;return result;
}
int __cdecl BeforeVertex(void* part,unsigned stride) {
    if(!Parsing())return 0;
    if(current->failure!=None)return 1;
    const size_t offset=stride==24?4:0x28;
    if(!Read<void*>(part,offset+4)){
        Count(stride==24?status.vertex24_failures:status.vertex32_failures);Fail(MissingVertex);return 1;
    }
    return 0;
}
int __cdecl AfterVertex(void* part,unsigned stride,int result) {
    if(!Parsing())return 0;
    const size_t offset=stride==24?4:0x28;
    if(result<0){Count(stride==24?status.vertex24_failures:status.vertex32_failures);Fail(VertexLock);return 1;}
    if(!Read<void*>(part,offset+0x18)) {
        const auto error=GetLastError();
        // A successful Lock must be balanced even if its mapped result is bad.
        Unlock(Read<void*>(part,offset+4));Count(stride==24?status.vertex24_failures:status.vertex32_failures);
        Fail(VertexLock);SetLastError(error);return 1;
    }
    return 0;
}
int __cdecl BeforeIndex(void* buffer) {
    if(!Parsing())return 0;
    if(current->failure!=None)return 1;
    if(!Read<void*>(buffer,4)){Fail(MissingIndex);return 1;}
    return 0;
}
int __cdecl AfterIndex(void* buffer,long result) {
    if(!Parsing())return 0;
    if(result!=0){Fail(IndexLock);return 1;}
    if(!Read<void*>(buffer,0x18)) {
        const auto error=GetLastError();Unlock(Read<void*>(buffer,4));
        Fail(IndexLock);SetLastError(error);return 1;
    }
    return 0;
}
int __cdecl ModelResult(void* renderer,unsigned result) {
    if(!current||!current->setter_depth||renderer!=current->renderer)return 0;
    if(!(result&255u))Fail(ModelLoad);
    return current->failure!=None;
}
int __cdecl RowResult(void* row) {
    if(!current||current->row!=row||current->failure==None||!current->renderer||
       !current->candidate_discarded||current->renderer_lock_released)return 0;
    const auto error=GetLastError();
    leave_renderer(current->renderer);current->renderer_lock_released=true;
    Count(status.lock_releases);Count(status.fallbacks);SetLastError(error);return 1;
}

// Site callbacks preserve the live register/flag state and the entire x87/SSE
// context. The exceptional branches rejoin original native cleanup code.
#define SAVE_CONTEXT __asm pushfd __asm pushad __asm mov ebx,esp __asm sub esp,528 __asm and esp,0fffffff0h __asm fxsave [esp]
#define RESTORE_CONTEXT __asm fxrstor [esp] __asm mov esp,ebx __asm popad __asm popfd

__declspec(naked) void Vertex24Before() {
    SAVE_CONTEXT
    __asm push 24
    __asm push dword ptr [ebx+4]
    __asm call BeforeVertex
    __asm add esp,8
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24_continue]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24_site]
}
__declspec(naked) void Vertex32Before() {
    SAVE_CONTEXT
    __asm push 32
    __asm push dword ptr [ebx+4]
    __asm call BeforeVertex
    __asm add esp,8
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32_continue]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32_site]
}
__declspec(naked) void Vertex24After() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx+28]
    __asm push 24
    __asm push dword ptr [ebx+4]
    __asm call AfterVertex
    __asm add esp,12
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24_continue]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex24_after_site]
}
__declspec(naked) void Vertex32After() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx+28]
    __asm push 32
    __asm push dword ptr [ebx+4]
    __asm call AfterVertex
    __asm add esp,12
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32_continue]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [vertex32_after_site]
}
__declspec(naked) void IndexFailure() {
    __asm pop edi
    __asm pop esi
    __asm pop ebx
    __asm xor eax,eax
    __asm pop ebp
    __asm ret 8
}
__declspec(naked) void IndexBefore() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx+4]
    __asm call BeforeIndex
    __asm add esp,4
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp IndexFailure
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [index_site]
}
__declspec(naked) void IndexAfter() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx+28]
    __asm push dword ptr [ebx+4]
    __asm call AfterIndex
    __asm add esp,8
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp IndexFailure
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [index_after_site]
}
__declspec(naked) void ModelResultGate() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx+28]
    __asm push dword ptr [ebx]
    __asm call ModelResult
    __asm add esp,8
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [model_cleanup]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [model_result_site]
}
__declspec(naked) void RowResultGate() {
    SAVE_CONTEXT
    __asm push dword ptr [ebx]
    __asm call RowResult
    __asm add esp,4
    __asm test eax,eax
    __asm jz native_path
    RESTORE_CONTEXT
    __asm jmp dword ptr [row_thumbnail]
native_path:
    RESTORE_CONTEXT
    __asm jmp dword ptr [row_result_site]
}
template<class T>bool Bind(T& target,const char* name) {
    target=reinterpret_cast<T>(g_api->Symbol(g_mod,name));return target!=nullptr;
}
bool Site(const char* name,void* hook,void** original) {return g_api->HookSite(g_mod,name,hook,original)!=0;}
}

extern "C" __declspec(dllexport) int __cdecl SdgoHangarPreviewGuardGetStatusV1(SdgoHangarPreviewGuardStatusV1* out) {
    if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    auto copy=status;copy.size=sizeof(copy);copy.version=1;
    copy.active_preview=current&&current->setter_depth?1u:0u;
    copy.active_parser=Parsing()?1u:0u;
    copy.active_failure=current?static_cast<uint32_t>(current->failure):0u;
    copy.active_unit=current?current->unit:0u;*out=copy;return 1;
}
SDGO_MOD_ENTRY {
    if(!api||!mod||api->version!=SDGO_API_VERSION||api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite)||
       !api->Symbol||!api->HookCode||!api->HookSite||!api->Log||original_row)return 0;
    g_api=api;g_mod=mod;
    if(!Bind(row_set_return,"preview_guard.row_set_return")||
       !Bind(close_source,"preview_guard.close_source")||
       !Bind(leave_renderer,"preview_guard.leave_renderer")||
       !Bind(vertex24_continue,"preview_guard.vertex24_continue")||
       !Bind(vertex32_continue,"preview_guard.vertex32_continue")||
       !Bind(model_cleanup,"preview_guard.model_cleanup")||!Bind(row_thumbnail,"preview_guard.row_thumbnail"))return 0;
    if(!api->HookCode(mod,"preview_guard.row_apply",reinterpret_cast<void*>(&Row),reinterpret_cast<void**>(&original_row))||
       !api->HookCode(mod,"preview_guard.set_model",reinterpret_cast<void*>(&SetModel),reinterpret_cast<void**>(&original_set))||
       !api->HookCode(mod,"preview_guard.load_model",reinterpret_cast<void*>(&LoadModel),reinterpret_cast<void**>(&original_load))||
       !api->HookCode(mod,"preview_guard.parse",reinterpret_cast<void*>(&Parse),reinterpret_cast<void**>(&original_parse))||
       !Site("preview_guard.vertex24_before",reinterpret_cast<void*>(&Vertex24Before),&vertex24_site)||
       !Site("preview_guard.vertex24_after",reinterpret_cast<void*>(&Vertex24After),&vertex24_after_site)||
       !Site("preview_guard.vertex32_before",reinterpret_cast<void*>(&Vertex32Before),&vertex32_site)||
       !Site("preview_guard.vertex32_after",reinterpret_cast<void*>(&Vertex32After),&vertex32_after_site)||
       !Site("preview_guard.index_before",reinterpret_cast<void*>(&IndexBefore),&index_site)||
       !Site("preview_guard.index_after",reinterpret_cast<void*>(&IndexAfter),&index_after_site)||
       !Site("preview_guard.model_result",reinterpret_cast<void*>(&ModelResultGate),&model_result_site)||
       !Site("preview_guard.row_result",reinterpret_cast<void*>(&RowResultGate),&row_result_site))return 0;
    api->Log(mod,SDGO_LOG_INFO,"unit-row preview failures release their native candidate and renderer lock, then use the original thumbnail branch");
    return 1;
}
