#pragma once
#include <windows.h>
#include <climits>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sdgo {
inline constexpr std::uint64_t kShopProfileMaximumBytes = 32ull * 1024 * 1024;
// Parse each owned profile once. Repeated Windows INI lookups scan large files
// on every field, and fixed section-name buffers silently constrain capacity.
class ShopProfile {
    struct Less {bool operator()(const std::wstring& a,const std::wstring& b) const {return _wcsicmp(a.c_str(),b.c_str())<0;}};
public:
    using Section=std::map<std::wstring,std::wstring,Less>;
    std::map<std::wstring,Section,Less> sections;
    explicit ShopProfile(const wchar_t* path)
    {
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("shop profile cannot be opened");
        std::vector<char> bytes;
        try {
            BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
            if(!GetFileInformationByHandle(file,&info)||!GetFileSizeEx(file,&size)||size.QuadPart<0||
               static_cast<std::uint64_t>(size.QuadPart)>kShopProfileMaximumBytes||
               (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("shop profile is not a regular file within the 32 MiB budget");
            bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD count=0;
            if(!bytes.empty()&&(!ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size()))
                throw std::runtime_error("shop profile read is incomplete");
        }catch(...){CloseHandle(file);throw;}
        CloseHandle(file);
        std::wstring text;
        if(bytes.size()>=2&&((static_cast<uint8_t>(bytes[0])==0xFF&&static_cast<uint8_t>(bytes[1])==0xFE)||
                            (static_cast<uint8_t>(bytes[0])==0xFE&&static_cast<uint8_t>(bytes[1])==0xFF))) {
            if(bytes.size()%2)throw std::runtime_error("invalid UTF-16 shop profile");
            const bool little=static_cast<uint8_t>(bytes[0])==0xFF;
            for(std::size_t i=2;i<bytes.size();i+=2){const auto a=static_cast<uint8_t>(bytes[i]),b=static_cast<uint8_t>(bytes[i+1]);text.push_back(static_cast<wchar_t>(little?a|(b<<8):(a<<8)|b));}
        }else if(!bytes.empty()) {
            const bool bom=bytes.size()>=3&&static_cast<uint8_t>(bytes[0])==0xEF&&static_cast<uint8_t>(bytes[1])==0xBB&&static_cast<uint8_t>(bytes[2])==0xBF;
            const char* data=bytes.data()+(bom?3:0);const int size=static_cast<int>(bytes.size()-(bom?3:0));
            if(size) {
                UINT codePage=CP_UTF8;DWORD flags=MB_ERR_INVALID_CHARS;
                int count=MultiByteToWideChar(codePage,flags,data,size,nullptr,0);
                if(!count&&!bom){codePage=CP_ACP;flags=0;count=MultiByteToWideChar(codePage,flags,data,size,nullptr,0);}
                if(!count)throw std::runtime_error("invalid encoded shop profile");
                text.resize(count);if(MultiByteToWideChar(codePage,flags,data,size,text.data(),count)!=count)throw std::runtime_error("shop profile decoding is incomplete");
            }
        }
        if(text.find(L'\0')!=std::wstring::npos)throw std::runtime_error("shop profile contains a null character");
        Section* current=nullptr;
        for(std::size_t start=0;start<text.size();) {
            const auto end=text.find(L'\n',start);
            const auto line=Trim(std::wstring_view(text).substr(start,end==std::wstring::npos?text.size()-start:end-start));
            start=end==std::wstring::npos?text.size():end+1;
            if(line.empty()||line.front()==L';'||line.front()==L'#')continue;
            if(line.front()==L'['&&line.back()==L']') {
                auto name=std::wstring(Trim(line.substr(1,line.size()-2)));
                if(name.empty())throw std::runtime_error("empty shop profile section");
                auto added=sections.emplace(std::move(name),Section{});
                if(!added.second)throw std::runtime_error("duplicate shop profile section");
                current=&added.first->second;
            }else {
                const auto equals=line.find(L'=');
                if(!current||equals==std::wstring_view::npos)throw std::runtime_error("invalid shop profile field");
                auto key=Trim(line.substr(0,equals));auto value=Trim(line.substr(equals+1));
                if(value.size()>=2&&(value.front()==L'\''||value.front()==L'"')&&value.back()==value.front())value=value.substr(1,value.size()-2);
                if(key.empty()||!current->emplace(std::wstring(key),std::wstring(value)).second)throw std::runtime_error("duplicate or empty shop profile field");
            }
        }
    }
    std::wstring Value(const wchar_t* section,const wchar_t* key,const wchar_t* fallback=L"") const
    {
        const auto found=sections.find(section);if(found==sections.end())return fallback;
        const auto value=found->second.find(key);return value==found->second.end()?fallback:value->second;
    }
    int Number(const wchar_t* section,const wchar_t* key,int fallback=0) const
    {
        const auto text=Value(section,key,std::to_wstring(fallback).c_str());wchar_t* end=nullptr;
        const auto value=_wcstoi64(text.c_str(),&end,10);
        if(text.empty()||end==text.c_str()||!end||*end||value<INT_MIN||value>INT_MAX)throw std::runtime_error("invalid shop profile integer");
        return static_cast<int>(value);
    }
private:
    static std::wstring_view Trim(std::wstring_view text)
    {
        const auto first=text.find_first_not_of(L" \t\r");if(first==std::wstring_view::npos)return {};
        return text.substr(first,text.find_last_not_of(L" \t\r")-first+1);
    }
};
} // namespace sdgo
