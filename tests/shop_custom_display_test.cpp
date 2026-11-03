#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include "../mods/shop_localization/custom_products.h"

namespace {
using namespace sdgo::shop_localization;
struct Grid {uint32_t unused=0;void* end=reinterpret_cast<void*>(0x1234);};
static_assert(offsetof(Grid,end)==4);
struct Box {uint32_t type;void* data;};struct Pair {uint32_t id;Box* box;};
struct Result {void* node;uint8_t inserted;uint8_t pad[3];};
Grid grid;std::map<uint32_t,Pair*> nodes;std::map<uint32_t,uint8_t*> rows;std::set<void*> allocated;
unsigned checks=0,integrityCalls=0;
bool injectNativeFault=false;
void Check(bool b,const char* text){if(!b)throw std::runtime_error(text);++checks;printf("PASS %s\n",text);}
void* __cdecl Data(){return &grid;}
void* __cdecl New(size_t n){void* p=malloc(n);if(p)allocated.insert(p);return p;}
void __cdecl Delete(void* p){if(p){allocated.erase(p);free(p);}}
int __fastcall Find(void*,void*,void** out,int id,int type,char)
{
    auto found=rows.find(static_cast<uint32_t>(id));if(type!=0x23||found==rows.end()){*out=nullptr;return 0;}*out=found->second;return 1;
}
void __fastcall MapFind(void*,void*,void** out,const uint32_t* id){if(injectNativeFault)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);*out=rows.count(*id)?reinterpret_cast<void*>(1):grid.end;}
void* __fastcall Make(void*,void*,const Pair* pair){auto* node=static_cast<Pair*>(New(sizeof(Pair)));if(node)*node=*pair;return node;}
void __fastcall Insert(void*,void*,Result* out,void* value,char)
{
    auto* node=static_cast<Pair*>(value);out->inserted=rows.count(node->id)?0:1;out->node=value;
    if(out->inserted){nodes[node->id]=node;rows[node->id]=static_cast<uint8_t*>(node->box->data);}else Delete(node);
}
void __fastcall Integrity(void*,void*,const void* record,uint32_t size,uint32_t type)
{
    Check(size==1279&&type==0x23&&Read32(static_cast<const uint8_t*>(record))==210002,"native display record checksum registration has correct type and width");++integrityCalls;
}
void* Symbol(SdgoMod*,const char* name)
{
    if(!strcmp(name,"shop_localization.grid_data"))return reinterpret_cast<void*>(&Data);
    if(!strcmp(name,"shop_localization.grid_find"))return reinterpret_cast<void*>(&Find);
    if(!strcmp(name,"shop_localization.native_new"))return reinterpret_cast<void*>(&New);
    if(!strcmp(name,"shop_localization.native_delete"))return reinterpret_cast<void*>(&Delete);
    if(!strcmp(name,"shop_localization.grid_make_node"))return reinterpret_cast<void*>(&Make);
    if(!strcmp(name,"shop_localization.grid_insert"))return reinterpret_cast<void*>(&Insert);
    if(!strcmp(name,"shop_localization.grid_register"))return reinterpret_cast<void*>(&Integrity);
    if(!strcmp(name,"shop_localization.grid_map_find"))return reinterpret_cast<void*>(&MapFind);
    return nullptr;
}
void __cdecl Log(SdgoMod*,int,const char*,...){}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc!=3)return 2;
    try {
        const std::filesystem::path base=argv[1];const int mode=_wtoi(argv[2]);
        const auto local=base/L"mods/shop_localization",labels=base/L"mods/shop_labels";
        std::filesystem::create_directories(local);std::filesystem::create_directories(labels);
        const std::wstring profile=L"\xFEFF[shop]\r\nRevision=test\r\n[product:1019001]\r\nName=新售扎古\r\nTemplateId=1010001\r\nCategory=1120022\r\nPrice=500\r\nPayment=1\r\nValidity=1\r\n";
        std::ofstream out(labels/L"display.ini",std::ios::binary);out.write(reinterpret_cast<const char*>(profile.data()),static_cast<std::streamsize>(profile.size()*2));out.close();
        std::array<uint8_t,1279> native{};Put32(native.data(),210001);Put32(native.data()+5,1010001);
        const wchar_t title[]=L"Original native title",sprite[]=L"sprs\\4000.spr";memcpy(native.data()+9,title,sizeof(title));memcpy(native.data()+0x36F,sprite,sizeof(sprite));rows[210001]=native.data();const auto before=native;
        if(mode==1)rows[210002]=native.data(); // foreign-key reservation prevents any append
        injectNativeFault=mode==2;
        CustomProducts catalog;catalog.Read(local.c_str());Check(!catalog.Empty(),"published parent Chinese profile read alongside independent static dictionary");
        SdgoApi api{};api.size=sizeof(api);api.version=1;api.Symbol=Symbol;api.Log=Log;Check(catalog.Bind(&api,nullptr),"native display adapters resolve through verified symbols");
        const bool installed=catalog.Install();
        if(mode==2){Check(!installed&&catalog.ExceptionCode()==EXCEPTION_ACCESS_VIOLATION&&native==before&&allocated.empty(),"native registration SEH is contained before it can escape a game UI hook");return 0;}
        if(mode==1){Check(!installed&&native==before&&rows.size()==2&&allocated.empty(),"foreign native grid reservation refuses before touching rows or allocations");return 0;}
        Check(installed&&rows.size()==2&&integrityCalls==1,"previously absent ProductId gets a real native display record");
        const auto* added=rows.at(210002);Check(Read32(added+5)==1019001&&!wcscmp(reinterpret_cast<const wchar_t*>(added+9),L"自定义商品"),"new row identity is exact and unverified host text stays neutral");
        Check(!memcmp(added+0x36F,native.data()+0x36F,400),"native template art reused without copying its descriptions or billing data");
        Check(catalog.Title(1019001)==nullptr,"author title unavailable before native shop reply verification");
        sdgo::shop::Option option{};option.productId=1019001;option.category=1120022;option.price=500;option.payment=1;option.validity=1;
        const auto optionBefore=option;std::array<uint8_t,0x100> view{};*reinterpret_cast<uintptr_t*>(view.data()+0x88)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(view.data()+0x8C)=reinterpret_cast<uintptr_t>(&option+1);
        catalog.Observe(view.data());Check(catalog.Title(1019001)&&*catalog.Title(1019001)==L"新售扎古","matching authoritative category price currency and term admit Chinese parent title");
        CustomProducts::Clear();Check(!catalog.Title(1019001),"render completion clears verification before any later popup");
        std::array<uint8_t,0x100> popup{};memcpy(popup.data()+0x84,&option,sizeof(option));catalog.ObservePopup(popup.data());
        Check(catalog.Title(1019001)&&*catalog.Title(1019001)==L"新售扎古","popup independently checks its current native option rather than cached list verification");
        CustomProducts::Clear();catalog.Observe(view.data());
        Check(!memcmp(&option,&optionBefore,sizeof(option)),"shop reply business bytes remain unchanged");
        ++option.price;catalog.Observe(view.data());Check(catalog.Title(1019001)==nullptr,"stale host profile cannot rename a differently priced live product");
        Check(native==before,"every original native record byte preserved");
        Check(catalog.Install()&&rows.size()==2,"display registration is idempotent");
        printf("SHOP CUSTOM CHINESE DISPLAY PASS %u checks\n",checks);
        for(void* p:allocated)free(p);return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
