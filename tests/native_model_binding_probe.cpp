// Unshipped diagnostic: observe original model loading and mesh lookup inputs.
// It never substitutes a resource, a model, a registry result or an actor.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include "sdgo/sdgo_api.h"

namespace {
struct Registers {std::uint32_t edi,esi,ebp,savedEsp,ebx,edx,ecx,eax,flags;};
void* g_unit=nullptr;void* g_load=nullptr;void* g_mesh=nullptr;
wchar_t g_output[MAX_PATH]={};
alignas(16) const std::uint32_t kMxcsr=0x1f80;
bool Word(std::uint32_t address,std::uint32_t* output){
    __try{*output=*reinterpret_cast<const std::uint32_t*>(address);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void String(FILE* file,std::uint32_t object){
    std::uint32_t length=0,capacity=0,pointer=object;
    if(!Word(object+16,&length)||!Word(object+20,&capacity)||length>240||capacity<length||
       (capacity>=8&&!Word(object,&pointer))){std::fputs("null",file);return;}
    std::fputc('"',file);
    for(std::uint32_t i=0;i<length;++i){
        wchar_t value=0;
        __try{value=*reinterpret_cast<const wchar_t*>(pointer+i*2);}
        __except(EXCEPTION_EXECUTE_HANDLER){std::fputs("[unreadable]",file);break;}
        std::fprintf(file,"\\u%04x",static_cast<unsigned>(value));
    }
    std::fputc('"',file);
}
void __cdecl Observe(Registers* r,unsigned kind){
    const DWORD incoming=GetLastError();FILE* file=nullptr;
    if(_wfopen_s(&file,g_output,L"ab")||!file){SetLastError(incoming);return;}
    std::uint32_t arg=0,key=0,record=0,identity=0;
    Word(r->savedEsp+8,&arg);
    std::fprintf(file,"{\"pid\":%lu,\"thread\":%lu,\"kind\":%u,\"this\":\"0x%08lx\",\"argument\":\"0x%08lx\"",
        GetCurrentProcessId(),GetCurrentThreadId(),kind,static_cast<DWORD>(r->ecx),static_cast<DWORD>(arg));
    if(kind==0){
        Word(arg+4,&record);Word(record,&identity);
        std::fprintf(file,",\"unit\":%lu",static_cast<DWORD>(identity));
    }else{
        std::fputs(",\"text\":",file);String(file,arg);
        if(kind==2){Word(r->ecx+0x11c,&key);std::fprintf(file,",\"model_key\":%lu",static_cast<DWORD>(key));}
    }
    std::fputs("}\n",file);std::fclose(file);SetLastError(incoming);
}
#define TRACE(name,kind,original) \
__declspec(naked) void name(){ \
    __asm{pushfd} __asm{pushad} __asm{mov eax,esp} __asm{sub esp,210h} __asm{and esp,0fffffff0h} \
    __asm{mov [esp+200h],eax} __asm{fxsave [esp]} __asm{fninit} __asm{ldmxcsr dword ptr [kMxcsr]} __asm{cld} \
    __asm{push kind} __asm{push eax} __asm{call Observe} __asm{add esp,8} __asm{fxrstor [esp]} \
    __asm{mov esp,[esp+200h]} __asm{popad} __asm{popfd} __asm{jmp dword ptr [original]} \
}
TRACE(Unit,0,g_unit)
TRACE(Load,1,g_load)
TRACE(Mesh,2,g_mesh)
#undef TRACE
}
SDGO_MOD_ENTRY{
    if(!api||api->version!=1||!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RUN",g_output,MAX_PATH))return 0;
    if(swprintf_s(g_output,L"%ls\\model-bindings.jsonl",api->ModDirectory(mod))<0)return 0;
    return api->HookCode(mod,"probe.unit_model",reinterpret_cast<void*>(&Unit),&g_unit)&&
        api->HookCode(mod,"probe.model_load",reinterpret_cast<void*>(&Load),&g_load)&&
        api->HookCode(mod,"probe.model_mesh",reinterpret_cast<void*>(&Mesh),&g_mesh);
}
