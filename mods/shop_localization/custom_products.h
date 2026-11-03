#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/shop_labels.h"

namespace sdgo::shop_localization {
constexpr size_t kProductBytes = 1279;
struct CustomProduct {
    uint32_t id = 0, templateId = 0, category = 0;
    int price = 0, payment = 0, validity = 0;
    std::wstring name;
};
inline uint32_t Read32(const uint8_t* ptr) { uint32_t value; memcpy(&value,ptr,4);return value; }
inline void Put32(uint8_t* ptr,uint32_t value) {memcpy(ptr,&value,4);}
inline bool Chinese(const std::wstring& text)
{
    return std::any_of(text.begin(),text.end(),[](wchar_t c){return (c>=0x3400&&c<=0x9FFF)||(c>=0xF900&&c<=0xFAFF);});
}
inline bool ProductIdBeforeName(const wchar_t* text,uint32_t* out)
{
    __try {memcpy(out,reinterpret_cast<const uint8_t*>(text)-4,4);return *out>1000000&&*out<2000000;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline bool CategoryIdBeforeName(const wchar_t* text,uint32_t* out)
{
    __try {memcpy(out,reinterpret_cast<const uint8_t*>(text)-5,4);return *out>=1000000&&*out<2400000;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
inline std::array<uint8_t,kProductBytes> ProductRecord(uint32_t nativeId,const CustomProduct& product,const uint8_t* source)
{
    std::array<uint8_t,kProductBytes> data{};
    Put32(data.data(),nativeId);Put32(data.data()+5,product.id);
    // Every remaining field is text or art. Neutral text is used until a real
    // shop reply matches the host profile's category/price/payment/term guard.
    const wchar_t neutral[]=L"自定义商品";
    memcpy(data.data()+9,neutral,sizeof(neutral));
    if(source)memcpy(data.data()+0x36F,source+0x36F,400); // native template's four sprite paths
    return data;
}

class CustomProducts {
    struct Box {uint32_t type;void* data;};
    struct Pair {uint32_t id;Box* box;};
    struct InsertResult {void* node;uint8_t inserted;uint8_t padding[3];};
    using Data=void*(__cdecl*)();
    using Find=int(__thiscall*)(void*,void**,int,int,char);
    using New=void*(__cdecl*)(size_t);
    using Delete=void(__cdecl*)(void*);
    using Make=void*(__thiscall*)(void*,const Pair*);
    using Insert=void(__thiscall*)(void*,InsertResult*,void*,char);
    using Register=void(__thiscall*)(void*,const void*,uint32_t,uint32_t);
    using MapFind=void(__thiscall*)(void*,void**,const uint32_t*);
    Data data=nullptr;Find find=nullptr;New allocate=nullptr;Delete release=nullptr;
    Make make=nullptr;Insert insert=nullptr;Register integrity=nullptr;MapFind mapFind=nullptr;
    const SdgoApi* api=nullptr;SdgoMod* mod=nullptr;
    std::map<uint32_t,CustomProduct> products;
    bool installed=false;
    bool nativeFault=false;
    DWORD nativeException=0;
    static int Number(const std::wstring& file,const wchar_t* section,const wchar_t* key)
    {
        wchar_t text[64]{};GetPrivateProfileStringW(section,key,L"",text,64,file.c_str());
        wchar_t* end=nullptr;long long n=_wcstoi64(text,&end,10);
        if(!text[0]||!end||*end||n<1||n>2147483647)throw std::runtime_error("invalid custom product display guard");
        return static_cast<int>(n);
    }
    bool Row(uint32_t id,const uint8_t** out) const
    {
        __try {void* raw=nullptr;if(!find(data(),&raw,static_cast<int>(id),0x23,0)||!raw||Read32(static_cast<uint8_t*>(raw))!=id)return false;*out=static_cast<uint8_t*>(raw);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool Occupied(uint32_t id) const
    {
        void* node=nullptr;void* grid=data();mapFind(grid,&node,&id);
        return node&&node!=*reinterpret_cast<void**>(static_cast<uint8_t*>(grid)+4);
    }
    static bool CopyOptions(void* self,size_t offset,sdgo::shop::Option* rows,size_t capacity,size_t* count)
    {
        __try {
            const auto* base=static_cast<uint8_t*>(self);const auto begin=*reinterpret_cast<const uintptr_t*>(base+offset),end=*reinterpret_cast<const uintptr_t*>(base+offset+4);
            if(!begin||end<begin||(end-begin)%sizeof(sdgo::shop::Option)||(end-begin)/sizeof(sdgo::shop::Option)>capacity)return false;
            *count=(end-begin)/sizeof(sdgo::shop::Option);memcpy(rows,reinterpret_cast<const void*>(begin),end-begin);return true;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
public:
    inline static thread_local std::set<uint32_t> verified;
    bool Empty() const {return products.empty();}
    void Read(const wchar_t* directory)
    {
        std::wstring root=directory;const auto slash=root.find_last_of(L"\\/");if(slash==std::wstring::npos)throw std::runtime_error("invalid localization Mod directory");
        const std::wstring file=root.substr(0,slash)+L"\\shop_labels\\display.ini";
        if(GetFileAttributesW(file.c_str())==INVALID_FILE_ATTRIBUTES)return;
        std::vector<wchar_t> sections(65536);
        const auto count=GetPrivateProfileSectionNamesW(sections.data(),static_cast<DWORD>(sections.size()),file.c_str());
        if(count>=sections.size()-2)throw std::runtime_error("custom shop display profile too large");
        for(const wchar_t* section=sections.data();*section;section+=wcslen(section)+1) {
            if(wcsncmp(section,L"product:",8))continue;
            wchar_t* end=nullptr;const auto id=wcstoul(section+8,&end,10);
            if(!end||*end||id<=1000000||id>=2000000||products.count(id)||products.size()>=256)throw std::runtime_error("invalid custom product identity");
            CustomProduct p;p.id=id;p.templateId=Number(file,section,L"TemplateId");p.category=Number(file,section,L"Category");
            p.price=Number(file,section,L"Price");p.payment=Number(file,section,L"Payment");p.validity=Number(file,section,L"Validity");
            wchar_t name[128]{};const auto len=GetPrivateProfileStringW(section,L"Name",L"",name,128,file.c_str());p.name=name;
            if(!len||len>50||!Chinese(p.name)||(p.payment!=1&&p.payment!=2))throw std::runtime_error("custom product needs a Chinese title and valid display guards");
            products.emplace(p.id,std::move(p));
        }
    }
    bool Bind(const SdgoApi* value,SdgoMod* owner)
    {
        api=value;mod=owner;
        data=reinterpret_cast<Data>(api->Symbol(mod,"shop_localization.grid_data"));
        find=reinterpret_cast<Find>(api->Symbol(mod,"shop_localization.grid_find"));
        allocate=reinterpret_cast<New>(api->Symbol(mod,"shop_localization.native_new"));
        release=reinterpret_cast<Delete>(api->Symbol(mod,"shop_localization.native_delete"));
        make=reinterpret_cast<Make>(api->Symbol(mod,"shop_localization.grid_make_node"));
        insert=reinterpret_cast<Insert>(api->Symbol(mod,"shop_localization.grid_insert"));
        integrity=reinterpret_cast<Register>(api->Symbol(mod,"shop_localization.grid_register"));
        mapFind=reinterpret_cast<MapFind>(api->Symbol(mod,"shop_localization.grid_map_find"));
        return data&&find&&allocate&&release&&make&&insert&&integrity&&mapFind;
    }
    bool InstallUnsafe()
    {
        if(installed||Empty())return true;
        std::map<uint32_t,const uint8_t*> native;
        uint32_t next=210001;
        for(;next<260001;++next){const uint8_t* row=nullptr;if(!Row(next,&row))break;native.emplace(Read32(row+5),row);}
        if(next==210001||next>=260001)return false;
        std::vector<std::pair<uint32_t,std::array<uint8_t,kProductBytes>>> additions;
        for(const auto& entry:products) {
            if(native.count(entry.first))continue;
            if(Occupied(next)||!native.count(entry.second.templateId))return false;
            additions.emplace_back(next,ProductRecord(next,entry.second,native.at(entry.second.templateId)));++next;
        }
        struct Pending {
            uint8_t* row=nullptr;Box* box=nullptr;void* node=nullptr;Delete free=nullptr;
            ~Pending(){if(node)free(node);if(box)free(box);if(row)free(row);}
        };
        std::vector<std::unique_ptr<Pending>> pending;
        for(const auto& addition:additions) {
            auto item=std::make_unique<Pending>();item->free=release;item->row=static_cast<uint8_t*>(allocate(kProductBytes));item->box=static_cast<Box*>(allocate(sizeof(Box)));
            if(!item->row||!item->box)return false;
            memcpy(item->row,addition.second.data(),kProductBytes);*item->box=Box{0x23,item->row};
            Pair pair{addition.first,item->box};item->node=make(data(),&pair);if(!item->node)return false;pending.push_back(std::move(item));
        }
        for(size_t i=0;i<pending.size();++i){auto& item=*pending[i];auto* row=item.row;auto* node=item.node;item.node=nullptr;InsertResult result{};insert(data(),&result,node,0);
            if(!result.inserted)return false;
            item.row=nullptr;item.box=nullptr;integrity(data(),row,static_cast<uint32_t>(kProductBytes),0x23);
            const uint8_t* found=nullptr;if(!Row(additions[i].first,&found)||found!=row)return false;
        }
        installed=true;api->Log(mod,SDGO_LOG_INFO,"custom Chinese product display records registered: %u; existing native rows preserved",static_cast<unsigned>(additions.size()));return true;
    }
    bool Install()
    {
        if(nativeFault)return false;
        __try {return InstallUnsafe();}
        __except(EXCEPTION_EXECUTE_HANDLER){nativeException=GetExceptionCode();nativeFault=true;return false;}
    }
    DWORD ExceptionCode() const {return nativeException;}
    static void Clear() {verified.clear();}
    void ObserveRows(const sdgo::shop::Option* rows,size_t count) const
    {
        verified.clear();
        for(size_t i=0;i<count;++i){const auto& row=rows[i];const auto entry=products.find(row.productId);if(entry==products.end())continue;const auto& p=entry->second;
            if(row.category==p.category&&row.price==p.price&&row.payment==p.payment&&row.validity==p.validity)verified.insert(p.id);
        }
    }
    void Observe(void* self,size_t offset=0x88) const
    {
        verified.clear();sdgo::shop::Option rows[512]{};size_t count=0;
        if(CopyOptions(self,offset,rows,512,&count))ObserveRows(rows,count);
    }
    void ObservePopup(void* self) const
    {
        sdgo::shop::Option row{};verified.clear();
        __try {memcpy(&row,static_cast<uint8_t*>(self)+0x84,sizeof(row));}
        __except(EXCEPTION_EXECUTE_HANDLER){return;}
        ObserveRows(&row,1);
    }
    const std::wstring* Title(uint32_t id) const
    {
        const auto found=products.find(id);return found!=products.end()&&verified.count(id)?&found->second.name:nullptr;
    }
};
} // namespace sdgo::shop_localization
