#include <windows.h>
#include <cstdint>
#include <cwchar>
#include "sdgo/sdgo_api.h"

namespace {
// The supported game's VC++ wide string has an eight-wchar inline union,
// followed by its length and capacity. This is a borrowed input view only;
// the native player copies it with the game's allocator when it retains it.
struct NativeString {
    union { const wchar_t* pointer; wchar_t inline_text[8]; } storage;
    uint32_t length, capacity;
};
static_assert(sizeof(NativeString)==24);
static_assert(offsetof(NativeString,length)==16);
using PlayFn=void(__thiscall*)(void*,const NativeString*);
using TransitionFn=void(__thiscall*)(void*,const uint32_t*);
using StreamStatusFn=uint32_t(WINAPI*)(void*);
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
PlayFn g_play=nullptr;
TransitionFn g_transition=nullptr;
StreamStatusFn g_streamStatus=nullptr;
volatile LONG g_state=0,g_battleSeen=0;
bool g_introAvailable=false;
constexpr wchar_t kLobby[]=L"bgm\\0042.mp3";
constexpr wchar_t kIntro[]=L"bgm\\0043.mp3";

uint32_t Track(const NativeString* value) {
    if(!value || value->length!=12 || value->capacity<value->length)return 0;
    const wchar_t* text=value->capacity>7?value->storage.pointer:value->storage.inline_text;
    if(!text)return 0;
    if(_wcsnicmp(text,kLobby,12)==0)return 42;
    if(_wcsnicmp(text,kIntro,12)==0)return 43;
    return 0;
}
bool LobbyState(uint32_t state) {
    return (state>=4 && state<=18) || state==20 || (state>=25 && state<=29);
}
void __fastcall Play(void* sound,void*,const NativeString* requested) {
    const uint32_t before=Track(requested);
    const auto state=static_cast<uint32_t>(InterlockedCompareExchange(&g_state,0,0));
    const bool played=InterlockedCompareExchange(&g_battleSeen,0,0)!=0;
    NativeString intro{};
    intro.storage.pointer=kIntro;intro.length=12;intro.capacity=12;
    const NativeString* selected=before==42 && !played && g_introAvailable && LobbyState(state)?&intro:requested;
    g_play(sound,selected);
    // Read back the stream opened by the original Miles player. No music
    // toggle, channel gain, loop setting or output device is changed here.
    void* stream=*reinterpret_cast<void**>(static_cast<uint8_t*>(sound)+0xC8);
    const uint32_t status=stream && g_streamStatus?g_streamStatus(stream):0;
    g_api->Log(g_mod,SDGO_LOG_INFO,
        "native BGM requested=%u selected=%u app=%u battle_seen=%u stream_open=%u native_status=%u",
        before,Track(selected),state,played?1u:0u,stream?1u:0u,status);
}
void __fastcall Transition(void* app,void*,const uint32_t* pending) {
    // Observe the actual transition, rather than the earlier queued request.
    // The mirrored destination also covers BGM requests made inside OnEnter.
    const auto current=*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(app)+0x4A0);
    if(current==22)InterlockedExchange(&g_battleSeen,1);
    if(pending && pending[0]==4)InterlockedExchange(&g_battleSeen,0);
    InterlockedExchange(&g_state,static_cast<LONG>(pending?pending[0]:current));
    g_transition(app,pending);
    const auto entered=*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(app)+0x4A0);
    InterlockedExchange(&g_state,static_cast<LONG>(entered));
    if(entered==22)InterlockedExchange(&g_battleSeen,1);
}
}
SDGO_MOD_ENTRY {
    if(!api || api->version!=SDGO_API_VERSION ||
       api->size<offsetof(SdgoApi,HookCode)+sizeof(api->HookCode) ||
       !api->Symbol || !api->HookCode || !api->Log || g_play || g_transition)return 0;
    if(!api->Symbol(mod,"lobby_music.play") || !api->Symbol(mod,"lobby_music.transition"))return 0;
    const DWORD attributes=GetFileAttributesW(L"BGM\\0043.mp3");
    g_introAvailable=attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
    g_api=api;g_mod=mod;
    const HMODULE miles=GetModuleHandleW(L"Mss32.dll");
    g_streamStatus=miles?reinterpret_cast<StreamStatusFn>(GetProcAddress(miles,"_AIL_stream_status@4")):nullptr;
    if(!api->HookCode(mod,"lobby_music.transition",reinterpret_cast<void*>(&Transition),reinterpret_cast<void**>(&g_transition)) ||
       !api->HookCode(mod,"lobby_music.play",reinterpret_cast<void*>(&Play),reinterpret_cast<void**>(&g_play)))return 0;
    api->Log(mod,g_introAvailable?SDGO_LOG_INFO:SDGO_LOG_WARN,
        g_introAvailable?"first login uses 0043; lobby requests after entering a battle retain 0042; login gate resets the session":
                         "BGM/0043.mp3 is missing; retaining native music selection");
    return 1;
}
