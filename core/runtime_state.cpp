#include "runtime_state.h"
#include "ini_string.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

extern "C" __declspec(dllexport) const char* __cdecl SdgoCoreRuntimeContractV1() noexcept {
    return "SDGO_CORE_RUNTIME_RECEIPTS_V1";
}

namespace sdgo::runtime {
namespace {
std::string Utf8(const std::wstring& text) {
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return {};
    std::string out(static_cast<size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring Wide(const std::string& text) {return std::wstring(text.begin(),text.end());}
std::string Json(const std::string& text) {
    std::string out="\"";char escaped[8]{};
    for(unsigned char c:text) {
        if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c<32){sprintf_s(escaped,"\\u%04x",static_cast<unsigned>(c));out+=escaped;}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
class Hash {
    BCRYPT_ALG_HANDLE algorithm_=nullptr;BCRYPT_HASH_HANDLE hash_=nullptr;std::vector<unsigned char> object_;
public:
    Hash() {
        DWORD size=0,n=0;
        if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return;
        if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&n,0)<0)return;
        object_.resize(size);
        if(BCryptCreateHash(algorithm_,&hash_,object_.data(),size,nullptr,0,0)<0)hash_=nullptr;
    }
    ~Hash(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    bool Add(const void* data,ULONG size){return hash_&&BCryptHashData(hash_,static_cast<PUCHAR>(const_cast<void*>(data)),size,0)>=0;}
    std::string Finish(){unsigned char bytes[32]{};if(!hash_||BCryptFinishHash(hash_,bytes,sizeof(bytes),0)<0)return {};std::string out;char hex[3]{};for(unsigned char c:bytes){sprintf_s(hex,"%02x",c);out+=hex;}return out;}
};
bool Relative(const std::string& name) {
    if(name=="root:config.xml")return true;
    if(name.empty()||name.front()=='/'||name.find("//")!=std::string::npos)return false;
    size_t begin=0;
    while(begin<name.size()) {
        const size_t end=name.find('/',begin);const auto part=name.substr(begin,end==std::string::npos?end:end-begin);
        if(part.empty()||part=="."||part=="..")return false;
        for(unsigned char c:part)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
        if(end==std::string::npos)return true;begin=end+1;
    }
    return false;
}
bool NoLinks(const std::wstring& path) {
    for(size_t i=3;i<=path.size();++i)if(i==path.size()||path[i]==L'\\') {
        const DWORD attributes=GetFileAttributesW(path.substr(0,i).c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return true;
}
std::string FileHash(const std::wstring& path) {
    if(!NoLinks(path))return {};
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES) {
        const DWORD error=GetLastError();return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?"-":"";
    }
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    LARGE_INTEGER size{};Hash hash;char buffer[16384];DWORD count=0;bool ok=GetFileSizeEx(file,&size)&&size.QuadPart>=0&&size.QuadPart<=16*1024*1024;
    while(ok){if(!ReadFile(file,buffer,sizeof(buffer),&count,nullptr)){ok=false;break;}if(!count)break;ok=hash.Add(buffer,count);}
    CloseHandle(file);return ok?hash.Finish():std::string{};
}
}

std::vector<std::string> ConfigurationFiles(const std::wstring& manifest) {
    auto text=Utf8(sdgo::ReadIniString(manifest,L"framework.configuration",L"files",L"mod.ini"));
    std::vector<std::string> result;size_t begin=0;
    while(begin<=text.size()) {
        const auto end=text.find(',',begin);auto item=text.substr(begin,end==std::string::npos?end:end-begin);
        if(!Relative(item))return {};result.push_back(item);
        if(end==std::string::npos)break;begin=end+1;
    }
    std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());return result;
}

std::string ConfigurationRevision(const std::wstring& directory,const std::vector<std::string>& files) noexcept {
    try {
    if(files.empty())return {};
    Hash hash;
    for(const auto& name:files) {
        if(!Relative(name))return {};
        std::wstring path;
        if(name=="root:config.xml") {
            auto root=directory;for(int i=0;i<3;++i){const auto at=root.find_last_of(L"\\/");if(at==std::wstring::npos)return {};root.resize(at);}path=root+L"\\config.xml";
        }else {auto relative=Wide(name);std::replace(relative.begin(),relative.end(),L'/',L'\\');path=directory+L"\\"+relative;}
        const auto digest=FileHash(path);if(digest.empty())return {};
        const std::string part=name+std::string(1,'\0')+digest+'\n';
        if(!hash.Add(part.data(),static_cast<ULONG>(part.size())))return {};
    }
    return hash.Finish();
    }catch(...){return {};}
}

void Write(const std::wstring& root,const std::wstring& executable,const std::string& host,const std::vector<ModState>& mods)noexcept {
    try {
        FILETIME creation{},exit{},kernel{},user{};
        if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return;
        const auto ticks=(static_cast<unsigned long long>(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
        const auto directory=root+L"\\local-state\\runtime";
        if(!NoLinks(directory))return;
        CreateDirectoryW((root+L"\\local-state").c_str(),nullptr);CreateDirectoryW(directory.c_str(),nullptr);
        std::string json="{\"schema_version\":1,\"pid\":"+std::to_string(GetCurrentProcessId())+",\"creation_ticks\":"+Json(std::to_string(ticks))+",\"executable\":"+Json(Utf8(executable))+",\"host_key\":"+Json(host)+",\"mods\":{";
        bool first=true;
        for(const auto& mod:mods) {
            if(!first)json+=',';first=false;
            json+=Json(mod.id)+":{\"version\":"+Json(mod.version)+",\"requested\":"+(mod.requested?"true":"false")+",\"active\":"+(mod.active?"true":"false")+",\"configuration_sha256\":"+Json(mod.configuration)+",\"configuration_files\":[";
            for(size_t i=0;i<mod.files.size();++i){if(i)json+=',';json+=Json(mod.files[i]);}json+="]}";
        }
        json+="}}\n";
        const auto destination=directory+L"\\"+std::to_wstring(GetCurrentProcessId())+L".json";
        static volatile LONG serial=0;
        const auto temporary=destination+L"."+std::to_wstring(GetCurrentThreadId())+L"."+std::to_wstring(InterlockedIncrement(&serial))+L".tmp";
        HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)return;
        DWORD written=0;const bool ok=WriteFile(file,json.data(),static_cast<DWORD>(json.size()),&written,nullptr)&&written==json.size()&&FlushFileBuffers(file);
        CloseHandle(file);
        if(!ok||!MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))DeleteFileW(temporary.c_str());
    }catch(...){/* Receipts never prevent original-game startup. */}
}
}
