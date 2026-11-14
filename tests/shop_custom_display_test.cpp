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
std::map<uint32_t,uint8_t*> categories,operators;
unsigned checks=0,integrityCalls=0;
bool injectNativeFault=false,allDisplayRowsReserved=false;
void Check(bool b,const char* text){if(!b)throw std::runtime_error(text);++checks;printf("PASS %s\n",text);}
void* __cdecl Data(){return &grid;}
void* __cdecl New(size_t n){void* p=malloc(n);if(p)allocated.insert(p);return p;}
void __cdecl Delete(void* p){if(p){allocated.erase(p);free(p);}}
int __fastcall Find(void*,void*,void** out,int id,int type,char)
{
    const auto identity=static_cast<uint32_t>(id);
    if(type==0x23) {
        auto found=rows.find(identity);if(found!=rows.end()){*out=found->second;return 1;}
    } else if(type==0x22) {
        auto found=categories.find(identity);if(found!=categories.end()){*out=found->second;return 1;}
    } else if(type==0x2C) {
        auto found=operators.find(identity);if(found!=operators.end()){*out=found->second;return 1;}
    }
    *out=nullptr;return 0;
}
void __fastcall MapFind(void*,void*,void** out,const uint32_t* id){if(injectNativeFault)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);*out=(rows.count(*id)||allDisplayRowsReserved)?reinterpret_cast<void*>(1):grid.end;}
void* __fastcall Make(void*,void*,const Pair* pair){auto* node=static_cast<Pair*>(New(sizeof(Pair)));if(node)*node=*pair;return node;}
void __fastcall Insert(void*,void*,Result* out,void* value,char)
{
    auto* node=static_cast<Pair*>(value);out->inserted=rows.count(node->id)?0:1;out->node=value;
    if(out->inserted){nodes[node->id]=node;rows[node->id]=static_cast<uint8_t*>(node->box->data);}else Delete(node);
}
void __fastcall Integrity(void*,void*,const void* record,uint32_t size,uint32_t type)
{
    const auto id=Read32(static_cast<const uint8_t*>(record));
    Check(size==1279&&type==0x23&&id>=210002&&id<260001,"native display record checksum registration has correct type and width");++integrityCalls;
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
        const uint32_t customProduct=mode==3?1039001:1019001;
        std::wstring profile=mode==3
            ? L"\xFEFF[shop]\r\nRevision=test\r\n[product:1039001]\r\nName=训练副官\r\nTemplateId=1030001\r\nCategory=1219047\r\nPrice=100\r\nPayment=1\r\nValidity=1\r\n"
            : L"\xFEFF[shop]\r\nRevision=test\r\n[product:1019001]\r\nName=新售扎古\r\nTemplateId=1010001\r\nCategory=1120022\r\nPrice=500\r\nPayment=1\r\nValidity=1\r\n";
        if(mode==4)for(int i=1;i<=5000;++i)profile+=L"[product:"+std::to_wstring(1011000+i)+L"]\r\nName=容量测试\r\nTemplateId=1010001\r\nCategory=1120022\r\nPrice=500\r\nPayment=1\r\nValidity=1\r\n";
        std::ofstream out(labels/L"display.ini",std::ios::binary);out.write(reinterpret_cast<const char*>(profile.data()),static_cast<std::streamsize>(profile.size()*2));out.close();
        std::array<uint8_t,1279> native{};Put32(native.data(),210001);Put32(native.data()+5,mode==3?1030001:1010001);
        const wchar_t title[]=L"Original native title",sprite[]=L"sprs\\4000.spr";
        const wchar_t confirm[]=L"sprs\\4001.spr",templateCompact[]=L"sprs\\4002.spr",templateLarge[]=L"sprs\\4003.spr";
        memcpy(native.data()+9,title,sizeof(title));memcpy(native.data()+0x36F,sprite,sizeof(sprite));
        memcpy(native.data()+0x3D3,confirm,sizeof(confirm));memcpy(native.data()+0x437,templateCompact,sizeof(templateCompact));
        memcpy(native.data()+0x49B,templateLarge,sizeof(templateLarge));rows[210001]=native.data();const auto before=native;
        std::array<uint8_t,614> category{};std::array<uint8_t,1671> operatorRow{};
        if(mode==3) {
            Put32(category.data(),1219047);Put32(category.data()+105,60047);
            Put32(operatorRow.data(),60047);
            const wchar_t icon[]=L"sdgo\\mods\\operator_catalog\\assets\\60047\\46.spr";
            memcpy(operatorRow.data()+733,icon,sizeof(icon));
        }
        if(mode==1||mode==4)rows[210002]=native.data(); // another table owns this global key
        allDisplayRowsReserved=mode==5;
        injectNativeFault=mode==2;
        CustomProducts catalog;catalog.Read(local.c_str());Check(!catalog.Empty(),"published parent Chinese profile read alongside independent static dictionary");
        SdgoApi api{};api.size=sizeof(api);api.version=1;api.Symbol=Symbol;api.Log=Log;Check(catalog.Bind(&api,nullptr),"native display adapters resolve through verified symbols");
        if(mode==3) {
            Check(!catalog.Install()&&allocated.empty()&&rows.size()==1&&integrityCalls==0,
                  "operator product defers before its category row exists without partial allocation");
            categories[1219047]=category.data();
            Put32(category.data()+105,59999);
            Check(!catalog.Install()&&allocated.empty()&&rows.size()==1&&integrityCalls==0,
                  "type-12 category with a non-operator reference refuses before any native allocation");
            Put32(category.data()+105,60047);
            Check(!catalog.Install()&&allocated.empty()&&rows.size()==1&&integrityCalls==0,
                  "operator product defers before its operator row exists without partial allocation");
            operators[60047]=operatorRow.data();
            auto* icon=reinterpret_cast<wchar_t*>(operatorRow.data()+733);
            std::fill_n(icon,50,L'a');
            Check(!catalog.Install()&&allocated.empty()&&rows.size()==1&&integrityCalls==0,
                  "unterminated operator icon path refuses before any native allocation");
            std::fill_n(icon,50,L'\0');
            const wchar_t malformed[]=L"sdgo\\mods\\operator_catalog\\assets\\60047\\46.txr";
            memcpy(icon,malformed,sizeof(malformed));
            Check(!catalog.Install()&&allocated.empty()&&rows.size()==1&&integrityCalls==0,
                  "non-SPR operator icon path refuses before any native allocation");
            std::fill_n(icon,50,L'\0');
            const wchar_t valid[]=L"sdgo\\mods\\operator_catalog\\assets\\60047\\46.spr";
            memcpy(icon,valid,sizeof(valid));
        }
        const auto categoryBefore=category;
        const auto operatorBefore=operatorRow;
        const bool installed=catalog.Install();
        if(mode==4) {
            Check(installed&&rows.size()==5003&&integrityCalls==5001,"5001 offers grow past the old cap and 64K section list without overwriting a foreign key");
            Check(rows.at(210002)==native.data()&&native==before,"foreign and original native records remain unchanged");
            std::vector<sdgo::shop::Option> options(600);
            for(auto& option:options){option.productId=1011001;option.category=1120022;option.price=500;option.payment=1;option.validity=1;}
            options.back().productId=1016000;
            std::array<uint8_t,0x100> view{};
            *reinterpret_cast<uintptr_t*>(view.data()+0x88)=reinterpret_cast<uintptr_t>(options.data());
            *reinterpret_cast<uintptr_t*>(view.data()+0x8C)=reinterpret_cast<uintptr_t>(options.data()+options.size());
            const auto saved=options;catalog.Observe(view.data());
            Check(catalog.Title(1016000)&&*catalog.Title(1016000)==L"容量测试","authoritative title validation reaches rows beyond the old 512 option buffer");
            Check(!memcmp(saved.data(),options.data(),options.size()*sizeof(options[0])),"large native reply business bytes are preserved");
            for(void* p:allocated)free(p);return 0;
        }
        if(mode==5){Check(!installed&&allocated.empty()&&rows.size()==1&&native==before&&integrityCalls==0,"shared native record exhaustion refuses the whole plan before any allocation or write");return 0;}
        if(mode==2){Check(!installed&&catalog.ExceptionCode()==EXCEPTION_ACCESS_VIOLATION&&native==before&&allocated.empty(),"native registration SEH is contained before it can escape a game UI hook");return 0;}
        if(mode==1){Check(installed&&native==before&&rows.size()==3&&rows.at(210002)==native.data()&&Read32(rows.at(210003))==210003,"foreign native key is skipped while the offer registers in the next free slot");for(void* p:allocated)free(p);return 0;}
        Check(installed,"previously absent ProductId installs a real native display record");
        Check(rows.size()==2,"native product grid gains exactly one appended record");
        Check(integrityCalls==1,"appended product is registered with the native integrity map");
        const auto* added=rows.at(210002);Check(Read32(added+5)==customProduct&&!wcscmp(reinterpret_cast<const wchar_t*>(added+9),L"自定义商品"),"new row identity is exact and unverified host text stays neutral");
        if(mode==3) {
            const wchar_t* expected[]={L"sdgo\\mods\\operator_catalog\\assets\\60047\\46.spr",
                                      L"sdgo\\mods\\operator_catalog\\assets\\60047\\46.spr"};
            for(size_t i=0;i<2;++i)
                Check(!wcscmp(reinterpret_cast<const wchar_t*>(added+kProductArtOffsets[i]),expected[i]),
                      "operator product maps the native icon role into the proven shop art consumers");
            Check(!memcmp(added+0x437,native.data()+0x437,200),
                  "unproven compact and large art fields remain byte-identical to the native template");
            Check(category==categoryBefore&&operatorRow==operatorBefore,
                  "operator storefront art resolution preserves category and operator source records");
        } else {
            Check(!memcmp(added+0x36F,native.data()+0x36F,400),"native template art reused without copying its descriptions or billing data");
        }
        Check(catalog.Title(customProduct)==nullptr,"author title unavailable before native shop reply verification");
        sdgo::shop::Option option{};option.productId=customProduct;option.category=mode==3?1219047:1120022;option.price=mode==3?100:500;option.payment=1;option.validity=1;
        const auto optionBefore=option;std::array<uint8_t,0x100> view{};*reinterpret_cast<uintptr_t*>(view.data()+0x88)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(view.data()+0x8C)=reinterpret_cast<uintptr_t>(&option+1);
        const std::wstring authored=mode==3?L"训练副官":L"新售扎古";
        catalog.Observe(view.data());Check(catalog.Title(customProduct)&&*catalog.Title(customProduct)==authored,"matching authoritative category price currency and term admit Chinese parent title");
        CustomProducts::Clear();Check(!catalog.Title(customProduct),"render completion clears verification before any later popup");
        std::array<uint8_t,0xC0> buyOptions{};*reinterpret_cast<uintptr_t*>(buyOptions.data()+0xB0)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(buyOptions.data()+0xB4)=reinterpret_cast<uintptr_t>(&option+1);
        catalog.Observe(buyOptions.data(),0xB0);Check(catalog.Title(customProduct)&&*catalog.Title(customProduct)==authored,"initial BUY options vector independently admits the guarded title");
        CustomProducts::Clear();std::array<uint8_t,0xC0> giftOptions{};*reinterpret_cast<uintptr_t*>(giftOptions.data()+0xB4)=reinterpret_cast<uintptr_t>(&option);*reinterpret_cast<uintptr_t*>(giftOptions.data()+0xB8)=reinterpret_cast<uintptr_t>(&option+1);
        catalog.Observe(giftOptions.data(),0xB4);Check(catalog.Title(customProduct)&&*catalog.Title(customProduct)==authored,"initial GIFT options vector independently admits the guarded title");
        CustomProducts::Clear();
        std::array<uint8_t,0x100> popup{};memcpy(popup.data()+0x84,&option,sizeof(option));catalog.ObservePopup(popup.data());
        Check(catalog.Title(customProduct)&&*catalog.Title(customProduct)==authored,"popup independently checks its current native option rather than cached list verification");
        CustomProducts::Clear();catalog.Observe(view.data());
        Check(!memcmp(&option,&optionBefore,sizeof(option)),"shop reply business bytes remain unchanged");
        ++option.price;catalog.Observe(view.data());Check(catalog.Title(customProduct)==nullptr,"stale host profile cannot rename a differently priced live product");
        Check(native==before,"every original native record byte preserved");
        Check(catalog.Install()&&rows.size()==2,"display registration is idempotent");
        printf("SHOP CUSTOM CHINESE DISPLAY PASS %u checks\n",checks);
        for(void* p:allocated)free(p);return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
