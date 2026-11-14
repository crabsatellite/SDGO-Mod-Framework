#include "sdgo/sdgo_api.h"
#include <windows.h>
#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/rgb_paint_protocol.h"
#include "sdgo/surface_decal_protocol.h"
using sdgo::friend_unit_gift::ExtensionHandlerFn;
ExtensionHandlerFn PrepareRgb(const SdgoApi*,SdgoMod*);
ExtensionHandlerFn PrepareDecals(const SdgoApi*,SdgoMod*);
namespace {
ExtensionHandlerFn rgb=nullptr,decals=nullptr;
std::int32_t __cdecl Dispatch(void* user,std::uint32_t actor,std::uint16_t command,
    const std::uint8_t* data,std::uint32_t size,void* members,void* database,
    std::uint8_t* output,std::uint32_t capacity,std::uint32_t* produced){
    if(produced)*produced=0;
    const auto handler=(command==sdgo::rgb_paint::wire::get||command==sdgo::rgb_paint::wire::save)?rgb:
        (command==sdgo::surface_decal::wire::get||command==sdgo::surface_decal::wire::save)?decals:nullptr;
    return handler?handler(user,actor,command,data,size,members,database,output,capacity,produced):
        static_cast<int>(sdgo::friend_unit_gift::WireStatus::invalid_argument);
}
}
SDGO_MOD_ENTRY {
    if(!api||!mod||!api->SettingInt||!api->SettingInt(mod,"Enabled",0))return 0;
    const bool useRgb=api->SettingInt(mod,"RgbEnabled",1)!=0,useDecals=api->SettingInt(mod,"DecalsEnabled",1)!=0;
    // Prepare both schemas before publishing any callback. One registration
    // makes a failed load atomic; no callback can outlive an unloaded DLL.
    rgb=useRgb?PrepareRgb(api,mod):nullptr;
    if(useRgb&&!rgb)return 0;
    decals=useDecals?PrepareDecals(api,mod):nullptr;
    if(useDecals&&!decals)return 0;
    if(useRgb||useDecals){
        const auto carrier=GetModuleHandleW(L"friend_unit_gift_server.dll");
        const auto registration=reinterpret_cast<sdgo::friend_unit_gift::RegisterExtensionFn>(GetProcAddress(carrier,SDGO_FRIEND_UNIT_GIFT_REGISTER_EXTENSION_EXPORT));
        const auto first=useDecals?sdgo::surface_decal::wire::get:sdgo::rgb_paint::wire::get;
        const auto last=useRgb?sdgo::rgb_paint::wire::save:sdgo::surface_decal::wire::save;
        // Unassigned commands between the two existing pairs are rejected.
        if(!registration||!registration(first,last,&Dispatch,nullptr))return 0;
    }
    api->Log(mod,SDGO_LOG_INFO,"Unit appearance ready: RGB=%d decals=%d",useRgb?1:0,useDecals?1:0);
    return 1;
}
