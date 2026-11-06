#include "sdgo/sdgo_api.h"
#include <windows.h>
#include <string>
static const SdgoApi* g_api=nullptr;
static SdgoMod* g_mod=nullptr;
SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    return api->SettingInt(mod,"FailLoad",0)==0;
}
extern "C" __declspec(dllexport) int __cdecl SdgoSafetyReload() {
    if(!g_api||g_api->size<offsetof(SdgoApi,ConfigurationApplied)+sizeof(g_api->ConfigurationApplied)||!g_api->ConfigurationRevision||!g_api->ConfigurationApplied)return 0;
    char revision[65]{};
    if(!g_api->ConfigurationRevision(g_mod,revision,sizeof(revision)))return 0;
    if(g_api->SettingInt(g_mod,"RejectReload",0))return 0;
    if(g_api->SettingInt(g_mod,"ChangeDuringReload",0)) {
        const auto path=std::wstring(g_api->ModDirectory(g_mod))+L"\\mod.ini";
        WritePrivateProfileStringW(L"settings",L"Amount",L"changed-during-read",path.c_str());
    }
    return g_api->ConfigurationApplied(g_mod,revision);
}
