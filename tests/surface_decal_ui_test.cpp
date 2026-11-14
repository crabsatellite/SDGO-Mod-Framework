#include "../mods/unit_appearance_client/decal/native_ui.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
using namespace sdgo::surface_decal;
using String=sdgo::hangar::ui::NativeString;
namespace {
unsigned checks=0,queries=0;int buttonTable=0;
std::array<unsigned char,0x300> service{},dialog{},preview{},button{};
void* result=nullptr;
template<class T,class A>void Put(A& a,unsigned offset,T value){std::memcpy(a.data()+offset,&value,sizeof value);}
void Check(bool ok,const char* why){++checks;if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
void* __fastcall Find(void* root,void*,const String* name,int kind){
    ++queries;Check(root==dialog.data(),"lookup uses the live page root");
    const auto* text=name->capacity<8?name->data.small:name->data.pointer;
    Check(!std::wcscmp(text,kind==16?L"Multi3DRender":L"SaveBtn"),"native lookup preserves short and long string ABI");
    return result;
}
void* Symbol(SdgoMod*,const char* name){
    if(!std::strcmp(name,"hangar.ui_button_vtable"))return &buttonTable;
    return reinterpret_cast<void*>(&Find); // other UI functions are never invoked
}
}
int main(){
    ui::Page page;SdgoApi api{};api.Symbol=Symbol;Check(page.Bind(&api,nullptr),"bind native lookup");
    // Reproduce the reported construction state: stale UTF-16 bytes occupied
    // the cached preview field before the factory became active.
    Put(service,0xd4,0x72006900u);Put(service,0xd0,reinterpret_cast<void*>(1));
    Check(!page.Ready(service.data())&&!page.Preview(service.data())&&queries==0,"inactive page never dereferences uninitialized controls");
    Put<unsigned char>(service,0xac,1);
    Check(!page.Ready(service.data())&&!page.Preview(service.data()),"unreadable pending root fails closed");
    Put(service,0xd0,dialog.data());
    Check(!page.Preview(service.data())&&queries==0,"hidden dialog never searches its controls");
    Put<unsigned char>(dialog,0x162,1);result=preview.data();
    Put(preview,0x15c,dialog.data());Put(preview,0x134,16u);
    Check(page.Preview(service.data())==preview.data(),"ready page resolves the actual named preview instead of stale cached bytes");
    Put(preview,0x15c,service.data());Check(!page.Preview(service.data()),"foreign dialog control rejected");
    Put(preview,0x15c,dialog.data());Put(preview,0x134,4u);Check(!page.Preview(service.data()),"wrong control type rejected");
    result=button.data();Put(button,0,&buttonTable);Put(button,0x15c,dialog.data());Put(button,0x134,4u);
    Check(page.Footer(service.data(),0)==button.data(),"footer resolves by native control name");
    result=nullptr;Check(!page.Preview(service.data()),"missing preview waits for native initialization");
    Put<unsigned char>(service,0xac,0);const auto before=queries;
    Check(!page.Preview(service.data())&&queries==before,"page exit stops all control access");
    Put<unsigned char>(service,0xac,1);result=preview.data();Put(preview,0x134,16u);
    Check(page.Preview(service.data())==preview.data(),"re-entered page resolves fresh controls");
    std::cout<<checks<<" checks passed\n";
}
