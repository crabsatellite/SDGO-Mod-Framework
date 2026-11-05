// Production x86 DLL with native calling-convention stand-ins. This does not
// decode audio or replace the separate native login/battle/return acceptance.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include "sdgo/sdgo_api.h"

namespace {
struct NativeString {
    union { const wchar_t* pointer; wchar_t inline_text[8]; } storage;
    uint32_t length,capacity;
};
static_assert(sizeof(NativeString)==24);
using Play=void(__fastcall*)(void*,void*,const NativeString*);
using Transition=void(__fastcall*)(void*,void*,const uint32_t*);
Play play=nullptr;Transition transition=nullptr;
std::array<uint8_t,0x4A4> app{};
std::array<uint8_t,0x200> sound{};
NativeString input{};
std::wstring heard;
const NativeString* heardInput=nullptr;
bool playDuringEntry=false,missingSymbol=false;
unsigned checks=0,failures=0,calls=0,hooks=0;
void Check(bool value,const char* text) {
    ++checks;if(!value){++failures;std::printf("FAIL: %s\n",text);}
}
void __cdecl Log(SdgoMod*,int,const char*,...) {}
void __fastcall OriginalPlay(void* self,void*,const NativeString* path) {
    Check(self==sound.data(),"native sound this pointer preserved");
    ++calls;heardInput=path;
    const auto text=path->capacity>7?path->storage.pointer:path->storage.inline_text;
    heard.assign(text,path->length);
    Check(text[path->length]==0,"borrowed input retains terminator");
    *reinterpret_cast<void**>(sound.data()+0xC8)=reinterpret_cast<void*>(0x1234);
}
void __fastcall OriginalTransition(void* self,void*,const uint32_t* next) {
    Check(self==app.data(),"native app this pointer preserved");
    *reinterpret_cast<uint32_t*>(app.data()+0x4A0)=next[0];
    if(playDuringEntry)play(sound.data(),nullptr,&input);
}
void* __cdecl Symbol(SdgoMod*,const char* name) {
    if(!std::strcmp(name,"lobby_music.play"))return reinterpret_cast<void*>(&OriginalPlay);
    if(!std::strcmp(name,"lobby_music.transition"))return missingSymbol?nullptr:reinterpret_cast<void*>(&OriginalTransition);
    return nullptr;
}
int __cdecl Hook(SdgoMod*,const char* name,void* detour,void** original) {
    ++hooks;
    if(!std::strcmp(name,"lobby_music.play")){
        play=reinterpret_cast<Play>(detour);*original=reinterpret_cast<void*>(&OriginalPlay);return 1;
    }
    if(!std::strcmp(name,"lobby_music.transition")){
        transition=reinterpret_cast<Transition>(detour);*original=reinterpret_cast<void*>(&OriginalTransition);return 1;
    }
    return 0;
}
void SetPath(const wchar_t* value) {
    input={};input.length=static_cast<uint32_t>(std::wcslen(value));
    if(input.length<8){std::memcpy(input.storage.inline_text,value,(input.length+1)*sizeof(wchar_t));input.capacity=7;}
    else {input.storage.pointer=value;input.capacity=input.length;}
}
void Enter(uint32_t state) {transition(app.data(),nullptr,&state);}
void Expect(const wchar_t* path,const char* text) {
    NativeString unchanged=input;const unsigned before=calls;
    play(sound.data(),nullptr,&input);
    Check(calls==before+1,"native play called exactly once");
    Check(heard==path,text);
    Check(!std::memcmp(&input,&unchanged,sizeof(input)),"caller's string is not modified or freed");
    Check(sound[0x100]==0x6A,"native gain/enable controls untouched");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    const bool available=std::wcscmp(argv[2],L"missing")!=0;
    wchar_t dll[MAX_PATH]{},oldDirectory[MAX_PATH]{},temporary[MAX_PATH]{},root[MAX_PATH]{};
    if(!GetFullPathNameW(argv[1],MAX_PATH,dll,nullptr) || !GetCurrentDirectoryW(MAX_PATH,oldDirectory) ||
       !GetTempPathW(MAX_PATH,temporary) || !GetTempFileNameW(temporary,L"bgm",0,root))return 3;
    DeleteFileW(root);if(!CreateDirectoryW(root,nullptr) || !SetCurrentDirectoryW(root))return 3;
    CreateDirectoryW(L"BGM",nullptr);
    if(available){HANDLE f=CreateFileW(L"BGM\\0043.mp3",GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);if(f==INVALID_HANDLE_VALUE)return 3;CloseHandle(f);}
    HMODULE library=LoadLibraryW(dll);if(!library)return 4;
    auto load=reinterpret_cast<SdgoModLoadFn>(GetProcAddress(library,"SdgoModLoad"));
    SdgoApi api{};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Log=&Log;api.Symbol=&Symbol;api.HookCode=&Hook;
    Check(load!=nullptr,"production entry export");
    Check(!load(nullptr,nullptr),"invalid API rejected");
    missingSymbol=true;Check(!load(&api,nullptr) && hooks==0,"missing symbol refuses all hooks");missingSymbol=false;
    Check(load(&api,nullptr)!=0 && hooks==2 && play && transition,"both verified native sites hooked");
    sound[0x100]=0x6A;
    SetPath(L"bgm\\0042.mp3");Expect(L"bgm\\0042.mp3","unknown state retains native track");
    Enter(4);Expect(available?L"bgm\\0043.mp3":L"bgm\\0042.mp3","first login selects intro when present");
    for(uint32_t state:{5u,6u,7u,12u,18u,20u,25u,29u}){
        Enter(state);Expect(available?L"bgm\\0043.mp3":L"bgm\\0042.mp3","navigation preserves intro before first battle");
    }
    playDuringEntry=true;Enter(5);playDuringEntry=false;
    Check(heard==(available?L"bgm\\0043.mp3":L"bgm\\0042.mp3"),"nested OnEnter music uses destination state");
    Enter(21);Expect(L"bgm\\0042.mp3","loading-screen music untouched");
    Enter(18);Expect(available?L"bgm\\0043.mp3":L"bgm\\0042.mp3","cancelled loading does not count as a battle");
    SetPath(L"BGM\\0042.MP3");Expect(available?L"bgm\\0043.mp3":L"BGM\\0042.MP3","native path matching is case insensitive");
    for(const wchar_t* other:{L"bgm\\0001.mp3",L"bgm\\0017.mp3",L"MyMusic\\0042.mp3",L"x",L""}){
        SetPath(other);Expect(other,"other songs and short strings untouched");Check(heardInput==&input,"unmodified input forwarded directly");
    }
    SetPath(L"bgm\\0042.mp3");playDuringEntry=true;Enter(22);playDuringEntry=false;
    Check(heard==L"bgm\\0042.mp3","battle OnEnter track is untouched");
    for(uint32_t state:{24u,18u,5u,6u,12u}){Enter(state);Expect(L"bgm\\0042.mp3","battle return and subsequent navigation retain 0042");}
    const auto before=calls;Enter(4);Check(calls==before,"state observation never forces playback when native BGM is disabled");
    Expect(available?L"bgm\\0043.mp3":L"bgm\\0042.mp3","new login resets session");
    FreeLibrary(library);
    if(available)DeleteFileW(L"BGM\\0043.mp3");RemoveDirectoryW(L"BGM");SetCurrentDirectoryW(oldDirectory);RemoveDirectoryW(root);
    std::printf("%s %u production DLL / native ABI checks (%s intro file); no real audio playback\n",failures?"FAIL":"PASS",checks,available?"present":"missing");
    return failures?1:0;
}
