// Unshipped readback of every individually expected native unit definition.
// The controller signals only after all fresh native table-ready markers.
// No hook, player interaction, database write or native table mutation.
#include <windows.h>
#include <share.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/script_registration.h"
namespace {
constexpr std::size_t bytes=0x72;
using Definition=std::array<std::uint8_t,bytes>;
using Find=void*(__thiscall*)(void*,std::uint32_t);
Find find=nullptr;void* registry=nullptr;const void* hash=nullptr;const void* compare=nullptr;
std::vector<Definition> expected;
char run[80]{},inputSha[80]{};
std::filesystem::path signal,output;
std::uint32_t Word(const void* p) { std::uint32_t v;std::memcpy(&v,p,4);return v; }
bool Read(const std::filesystem::path& path,std::vector<std::uint8_t>& raw,std::size_t maximum) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};BY_HANDLE_FILE_INFORMATION info{};
    bool ok=GetFileSizeEx(file,&size)&&GetFileInformationByHandle(file,&info)&&size.QuadPart>0&&
        static_cast<unsigned long long>(size.QuadPart)<=maximum&&
        !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
    if(ok){raw.resize(static_cast<std::size_t>(size.QuadPart));DWORD n=0;ok=ReadFile(file,raw.data(),static_cast<DWORD>(raw.size()),&n,nullptr)&&n==raw.size();}
    CloseHandle(file);return ok;
}
bool Map(std::uint32_t* count) {
    __try {
        const auto* map=static_cast<const std::uint8_t*>(registry)+0x18;
        *count=Word(map+0x1c);
        return Word(map+8)==4&&Word(map+0x14)==reinterpret_cast<std::uintptr_t>(hash)&&
            Word(map+0x18)==reinterpret_cast<std::uintptr_t>(compare)&&*count>0&&*count<=1048576&&Word(map+0x20)!=0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Actual(std::uint32_t id,Definition* out,std::uint32_t* address) {
    __try {
        const auto* definition=static_cast<const std::uint8_t*>(find(registry,id));
        if(!definition||Word(definition)!=id)return false;
        std::memcpy(out->data(),definition,bytes);*address=reinterpret_cast<std::uintptr_t>(definition);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::string Hex(const Definition& value) {
    constexpr char digits[]="0123456789abcdef";std::string text;text.reserve(bytes*2);
    for(auto byte:value){text.push_back(digits[byte>>4]);text.push_back(digits[byte&15]);}return text;
}
DWORD WINAPI Pump(void*) {
    FILE* file=_wfsopen(output.c_str(),L"wb",_SH_DENYNO);if(!file)return 1;
    std::fprintf(file,"{\"event\":\"probe_ready\",\"run_id\":\"%s\",\"pid\":%lu,\"expected_sha256\":\"%s\",\"actor_writes\":false,\"table_writes\":false}\n",run,GetCurrentProcessId(),inputSha);std::fflush(file);
    bool requested=false;const auto deadline=GetTickCount64()+1200000;
    while(GetTickCount64()<deadline) {
        std::vector<std::uint8_t> request;
        if(Read(signal,request,80)&&std::string(request.begin(),request.end())==std::string(run)+"\n"){requested=true;break;}
        Sleep(100);
    }
    unsigned readable=0,equal=0;std::uint32_t before=0,after=0;const bool ready=requested&&Map(&before);
    if(ready)for(const auto& wanted:expected) {
        Definition actual{};std::uint32_t address=0;const auto id=Word(wanted.data());const bool found=Actual(id,&actual,&address);
        const bool same=found&&actual==wanted;if(found)++readable;if(same)++equal;
        const auto text=Hex(actual);
        std::fprintf(file,"{\"event\":\"native_definition\",\"run_id\":\"%s\",\"pid\":%lu,\"id\":%u,\"address\":%u,\"readable\":%s,\"equal\":%s,\"actual_hex\":\"%s\"}\n",
            run,GetCurrentProcessId(),id,address,found?"true":"false",same?"true":"false",text.c_str());
    }
    const bool stable=ready&&Map(&after)&&after==before;
    std::fprintf(file,"{\"event\":\"definition_complete\",\"run_id\":\"%s\",\"pid\":%lu,\"requested\":%s,\"map_ready\":%s,\"map_stable\":%s,\"expected\":%u,\"readable\":%u,\"equal\":%u,\"map_count_before\":%u,\"map_count_after\":%u,\"actor_writes\":false,\"table_writes\":false}\n",
        run,GetCurrentProcessId(),requested?"true":"false",ready?"true":"false",stable?"true":"false",static_cast<unsigned>(expected.size()),readable,equal,before,after);
    std::fflush(file);std::fclose(file);return 0;
}
}
SDGO_MOD_ENTRY {
    if(!api||api->version!=1||!api->host_key||std::strcmp(api->host_key,"BattleServer-548A891D-0046C000"))return 0;
    api->SettingString(mod,"RunId","",run,sizeof(run));if(std::strlen(run)!=36)return 0;
    for(const auto* p=run;*p;++p)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||*p=='-'))return 0;
    api->SettingString(mod,"ExpectedSha256","",inputSha,sizeof(inputSha));if(std::strlen(inputSha)!=64)return 0;
    const auto root=std::filesystem::path(api->ModDirectory(mod));std::vector<std::uint8_t> raw;
    if(!Read(root/L"expected.bin",raw,24+4096*bytes)||sdgo::scripts::Sha256(raw)!=inputSha||raw.size()<24||
       std::memcmp(raw.data(),"SDGOBD1\0",8)||Word(raw.data()+8)!=1||Word(raw.data()+12)!=bytes||Word(raw.data()+20))return 0;
    const auto count=Word(raw.data()+16);if(!count||count>4096||raw.size()!=24+count*bytes)return 0;
    unsigned previous=0;
    for(unsigned i=0;i<count;++i){Definition record;std::memcpy(record.data(),raw.data()+24+i*bytes,bytes);
        const auto id=Word(record.data());if(id<=previous||id<10001||id>30000)return 0;previous=id;expected.push_back(record);}
    registry=api->Symbol(mod,"probe.unit_registry");find=reinterpret_cast<Find>(api->Symbol(mod,"probe.find_unit"));
    hash=api->Symbol(mod,"probe.map_hash");compare=api->Symbol(mod,"probe.map_compare");
    if(!registry||!find||!hash||!compare)return 0;
    signal=root/L"verify-definitions.request";output=root/(L"definitions-"+std::wstring(run,run+36)+L".jsonl");
    if(GetFileAttributesW(signal.c_str())!=INVALID_FILE_ATTRIBUTES||GetFileAttributesW(output.c_str())!=INVALID_FILE_ATTRIBUTES)return 0;
    HANDLE thread=CreateThread(nullptr,0,&Pump,nullptr,0,nullptr);if(!thread)return 0;CloseHandle(thread);return 1;
}
