#include "../mods/game_test_client/quiet_audio.h"
#include <cstdio>
#include <cstring>
namespace {
QuietAudio::OpenFn detour=nullptr;
int failures=0,preferenceCalls=0,volumeCalls=0,openCalls=0;bool reject=false,refuseMute=false;
QuietAudio* observedGuard=nullptr;
float gain=1.0f;int driver;
int WINAPI Preference(uint32_t key,int value) {if (key!=15 || value!=1)++failures;++preferenceCalls;return 0;}
void WINAPI Volume(void* actual,float volume) {
    if (actual!=&driver || volume!=0.0f)++failures;
    // The volume API can yield to the independent receipt writer. It must see
    // only completed readbacks, even while a returned driver is being muted.
    const auto pending=observedGuard->State();
    if (pending.opened!=pending.muted || pending.failed)++failures;
    gain=refuseMute ? 1.0f:volume;++volumeCalls;
}
float WINAPI Gain(void* actual) {if (actual!=&driver)++failures;return gain;}
void* WINAPI Open(uint32_t rate,int bits,int channels,uint32_t flags) {
    ++openCalls;if (preferenceCalls!=openCalls || rate!=44100 || bits!=16 || channels!=2 || flags!=0x21)++failures;
    return reject ? nullptr:&driver;
}
int __cdecl Hook(SdgoMod*,const char* library,const char* name,void* replacement,void** original) {
    if (std::strcmp(library,"Mss32.dll") || std::strcmp(name,"_AIL_open_digital_driver@16"))return 0;
    detour=reinterpret_cast<QuietAudio::OpenFn>(replacement);*original=reinterpret_cast<void*>(&Open);return 1;
}
}
int main() {
    SdgoApi api{};api.HookImport=&Hook;QuietAudio guard;const QuietAudio::Bindings bindings{&Preference,&Volume,&Gain};
    observedGuard=&guard;
    if (!guard.Install(&api,reinterpret_cast<SdgoMod*>(1),&bindings) || !detour)return 1;
    if (detour(44100,16,2,0x20)!=&driver || volumeCalls!=1 || gain!=0.0f)++failures;
    reject=true;if (detour(44100,16,2,0x20)!=nullptr || volumeCalls!=1)++failures;
    const auto state=guard.State();if (!state.active || state.opened!=1 || state.muted!=1 || state.failed || state.open_failed!=1)++failures;
    // A real successful fallback remains admissible only with zero gain.
    reject=false;
    if (detour(44100,16,2,0x20)!=&driver)++failures;
    const auto fallback=guard.State();
    if (fallback.opened!=2 || fallback.muted!=2 || fallback.failed || fallback.open_failed!=1)++failures;
    // A nonzero native volume readback must still fail the quiet policy.
    refuseMute=true;
    if (detour(44100,16,2,0x20)!=&driver)++failures;
    const auto audible=guard.State();
    if (audible.opened!=3 || audible.muted!=2 || audible.failed!=1 || audible.open_failed!=1)++failures;
    std::printf("Quiet Miles native-driver policy: %d failures\n",failures);return failures ? 1:0;
}
