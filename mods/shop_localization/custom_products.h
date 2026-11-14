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
#include "sdgo/shop_profile.h"

namespace sdgo::shop_localization {
constexpr size_t kProductBytes = 1279;
constexpr size_t kProductArtOffsets[] = {0x36F, 0x3D3, 0x437, 0x49B};
constexpr size_t kOperatorSpriteOffsets[] = {205, 313, 421, 533, 633, 733, 833};
constexpr size_t kOperatorSpriteCapacities[] = {54, 54, 54, 50, 50, 50, 50};
// The native 97x97 operator icon role matches both proven shop consumers:
// the list/search thumbnail and the BUY/confirmation preview.
constexpr size_t kShopOperatorSpriteRoles[] = {5, 5};
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
inline bool WriteProductArt(
    std::array<uint8_t,kProductBytes>& data,
    const std::array<std::wstring,2>& art)
{
    for(size_t i=0;i<art.size();++i) {
        if(art[i].empty()||art[i].size()>=50)return false;
        std::memset(data.data()+kProductArtOffsets[i],0,100);
        std::memcpy(data.data()+kProductArtOffsets[i],art[i].data(),art[i].size()*sizeof(wchar_t));
    }
    return true;
}
inline std::array<uint8_t,kProductBytes> ProductRecord(
    uint32_t nativeId,const CustomProduct& product,const uint8_t* source,
    const std::array<std::wstring,2>* operatorArt=nullptr)
{
    std::array<uint8_t,kProductBytes> data{};
    Put32(data.data(),nativeId);Put32(data.data()+5,product.id);
    // Every remaining field is text or art. Neutral text is used until a real
    // shop reply matches the host profile's category/price/payment/term guard.
    const wchar_t neutral[]=L"自定义商品";
    memcpy(data.data()+9,neutral,sizeof(neutral));
    if(source)memcpy(data.data()+0x36F,source+0x36F,400); // native template's four sprite paths
    if(operatorArt&&!WriteProductArt(data,*operatorArt))
        throw std::runtime_error("operator storefront art path exceeds native product capacity");
    return data;
}

class CustomProducts {
    struct Box {uint32_t type;void* data;};
    struct Pair {uint32_t id;Box* box;};
    struct InsertResult {void* node;uint8_t inserted;uint8_t padding[3];};
    using Data=void*(__cdecl*)();
    using Find=uint8_t(__thiscall*)(void*,void**,int,int,char);
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
    std::map<uint32_t,const uint8_t*> nativeProducts;
    bool productsScanned=false,capacityExhausted=false;
    bool installed=false;
    bool nativeFault=false;
    DWORD nativeException=0;
    static int Number(const sdgo::ShopProfile& file,const wchar_t* section,const wchar_t* key)
    {
        const int n=file.Number(section,key);if(n<1)throw std::runtime_error("invalid custom product display guard");return n;
    }
    bool Row(uint32_t id,uint32_t type,const uint8_t** out) const
    {
        __try {void* raw=nullptr;if(!find(data(),&raw,static_cast<int>(id),static_cast<int>(type),0)||!raw||Read32(static_cast<uint8_t*>(raw))!=id)return false;*out=static_cast<uint8_t*>(raw);return true;}
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool NativeText(const uint8_t* row,size_t offset,size_t capacity,std::wstring* out) const
    {
        __try {
            const auto* text=reinterpret_cast<const wchar_t*>(row+offset);size_t length=0;
            while(length<capacity&&text[length])++length;
            if(length==capacity)return false;
            out->assign(text,length);return true;
        } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool OperatorArt(const CustomProduct& product,std::array<std::wstring,2>* art,bool* replace) const
    {
        *replace=false;
        // Type 12 is the native operator/item family. Wait for its category
        // table instead of permanently registering a template-only product
        // while operator_catalog is still applying later in the load order.
        if(product.category/100000!=12)return true;
        const uint8_t* category=nullptr;
        if(!Row(product.category,0x22,&category))return false;
        const uint32_t operatorId=Read32(category+105);
        if(operatorId<60001||operatorId>60999)return false;
        const uint8_t* operatorRow=nullptr;
        if(!Row(operatorId,0x2C,&operatorRow))return false;
        for(size_t i=0;i<art->size();++i) {
            const size_t role=kShopOperatorSpriteRoles[i];
            if(!NativeText(operatorRow,kOperatorSpriteOffsets[role],kOperatorSpriteCapacities[role],&(*art)[i])||
               (*art)[i].size()<4||_wcsicmp((*art)[i].c_str()+(*art)[i].size()-4,L".spr")||(*art)[i].size()>=50)
                return false;
        }
        *replace=true;return true;
    }
    bool Occupied(uint32_t id) const
    {
        void* node=nullptr;void* grid=data();mapFind(grid,&node,&id);
        return node&&node!=*reinterpret_cast<void**>(static_cast<uint8_t*>(grid)+4);
    }
    static size_t OptionCount(void* self,size_t offset)
    {
        __try {
            const auto* base=static_cast<uint8_t*>(self);
            const auto begin=*reinterpret_cast<const uintptr_t*>(base+offset),end=*reinterpret_cast<const uintptr_t*>(base+offset+4);
            return begin&&end>=begin&&(end-begin)%sizeof(sdgo::shop::Option)==0
                ?(end-begin)/sizeof(sdgo::shop::Option):0;
        }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
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
        const std::wstring path=root.substr(0,slash)+L"\\shop_labels\\display.ini";
        if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return;
        const sdgo::ShopProfile file(path.c_str());
        for(const auto& entry:file.sections) {
            const wchar_t* section=entry.first.c_str();
            if(wcsncmp(section,L"product:",8))continue;
            wchar_t* end=nullptr;const auto id=wcstoul(section+8,&end,10);
            if(!end||*end||id<=1000000||id>=2000000||products.count(id))throw std::runtime_error("invalid custom product identity");
            CustomProduct p;p.id=id;p.templateId=Number(file,section,L"TemplateId");p.category=Number(file,section,L"Category");
            p.price=Number(file,section,L"Price");p.payment=Number(file,section,L"Payment");p.validity=Number(file,section,L"Validity");
            p.name=file.Value(section,L"Name");
            if(p.name.empty()||p.name.size()>50||!Chinese(p.name)||(p.payment!=1&&p.payment!=2))throw std::runtime_error("custom product needs a Chinese title and valid display guards");
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
        if(capacityExhausted)return false;
        // Native record IDs are shared with other table kinds. Discover existing
        // product rows across the supported interval; never assume a gap is ours.
        if(!productsScanned) {
            const uint8_t* first=nullptr;if(!Row(210001,0x23,&first))return false;
            for(uint32_t id=210001;id<260001;++id){const uint8_t* row=nullptr;if(Row(id,0x23,&row))nativeProducts.emplace(Read32(row+5),row);}
            productsScanned=true;
        }
        const auto& native=nativeProducts;
        if(native.empty())return false;
        uint32_t next=210001;
        std::vector<std::pair<uint32_t,std::array<uint8_t,kProductBytes>>> additions;
        for(const auto& entry:products) {
            if(native.count(entry.first))continue;
            while(next<260001&&Occupied(next))++next;
            if(next>=260001){capacityExhausted=true;api->Log(mod,SDGO_LOG_ERROR,"no free supported shop display record IDs; all existing table records preserved");return false;}
            if(!native.count(entry.second.templateId))return false;
            std::array<std::wstring,2> art{};bool replace=false;
            if(!OperatorArt(entry.second,&art,&replace))return false;
            additions.emplace_back(next,ProductRecord(next,entry.second,native.at(entry.second.templateId),replace?&art:nullptr));++next;
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
            const uint8_t* found=nullptr;if(!Row(additions[i].first,0x23,&found)||found!=row)return false;
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
        verified.clear();
        const size_t count=OptionCount(self,offset);
        if(!count||count>sdgo::kShopProfileMaximumBytes/sizeof(sdgo::shop::Option))return;
        std::vector<sdgo::shop::Option> rows(count);size_t copied=0;
        if(CopyOptions(self,offset,rows.data(),rows.size(),&copied))ObserveRows(rows.data(),copied);
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
