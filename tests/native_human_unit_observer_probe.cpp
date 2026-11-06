// Unshipped passive probe for an owned native GameTest. Copies bounded scalar
// observations in the callback; a worker performs file I/O. No actor writes.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/battle_human_unit_observer.h"

namespace {
constexpr unsigned kCapacity = 256;
struct Sample {
    volatile LONG ready = 0;
    std::uint32_t type = 0, thread = 0, valid = 0, owner = 0, state = 0;
    std::uint64_t sequence = 0;
    std::int32_t result = 0;
    std::uint8_t rule = 0, mode = 0, selected = 0;
    std::uint32_t source[4]{}, native[4]{}, before_zero[4]{};
};
std::array<Sample,kCapacity> samples;
volatile LONG used = 0, dropped = 0, registered = 0;
SdgoBattleReadHumanUnitObserverStatusV1Fn readStatus = nullptr;
HANDLE stopEvent = nullptr, worker = nullptr;
char runId[80]{};
std::wstring output;
std::uint32_t Word(const std::uint8_t* bytes) { std::uint32_t value = 0;std::memcpy(&value,bytes,4);return value; }

bool Copy(const SdgoBattleHumanUnitEventV1* event, Sample* sample) {
    __try {
        if (!event || event->size != sizeof(*event) || event->version != 1 || event->reserved ||
            event->type < 1 || event->type > 5 || !event->sequence || event->thread_id != GetCurrentThreadId()) return false;
        sample->type = event->type;sample->thread = event->thread_id;sample->sequence = event->sequence;
        sample->rule = event->rule;sample->mode = event->mode;sample->result = event->native_result;
        SdgoBattleHumanUnitObserverStatusV1 status{};status.size = sizeof(status);status.version = 1;
        if (!readStatus(&status) || status.registration_state != 2) return false;
        sample->state = status.registration_state;
        if (event->type == SDGO_BATTLE_HUMAN_UNIT_BEFORE || event->type == SDGO_BATTLE_HUMAN_UNIT_AFTER) {
            if (!event->room || !event->source || !event->player) return false;
            const auto* source = static_cast<const std::uint8_t*>(event->source);
            const auto* player = static_cast<const std::uint8_t*>(event->player);
            sample->owner = Word(source+0x429);sample->selected = player[0x33c];
            if (sample->selected >= 4) return false;
            const bool deck = event->mode == 6 || event->mode == 7;
            for (unsigned i = 0; i < (deck ? 4u : 1u); ++i) {
                sample->source[i] = Word(source+(deck ? 0x19e+i*0x99 : 0x49));
                const auto* slot = player+0x340+(deck ? i : sample->selected)*0x658;
                if (event->type == SDGO_BATTLE_HUMAN_UNIT_BEFORE) {
                    bool zero = true;
                    for (unsigned n = 0; n < 0x2f0; ++n) if (slot[0x64+n]) zero = false;
                    sample->before_zero[i] = zero ? 1u : 0u;
                } else if (event->native_result) sample->native[i] = Word(slot);
            }
        } else if (!event->room || event->source || event->player) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __cdecl Observe(const SdgoBattleHumanUnitEventV1* event, void*) {
    const auto index = InterlockedIncrement(&used)-1;
    if (index < 0 || index >= static_cast<LONG>(kCapacity)) { InterlockedIncrement(&dropped);return; }
    auto& sample = samples[static_cast<unsigned>(index)];
    sample.valid = Copy(event,&sample) ? 1u : 0u;
    InterlockedExchange(&sample.ready,1);
}
DWORD WINAPI Flush(void*) {
    while (!InterlockedCompareExchange(&registered,0,0))
        if (WaitForSingleObject(stopEvent,10) == WAIT_OBJECT_0) return 0;
    FILE* file = nullptr;
    if (_wfopen_s(&file,output.c_str(),L"wb") || !file) return 1;
    std::fprintf(file,"{\"event\":\"observer_ready\",\"run_id\":\"%s\",\"pid\":%lu,\"actor_writes\":false}\n",runId,GetCurrentProcessId());std::fflush(file);
    unsigned cursor = 0;
    while (WaitForSingleObject(stopEvent,10) != WAIT_OBJECT_0) {
        while (cursor < kCapacity && InterlockedCompareExchange(&samples[cursor].ready,0,0)) {
            const auto& s = samples[cursor];
            std::fprintf(file,"{\"event\":\"human_lifecycle\",\"run_id\":\"%s\",\"pid\":%lu,\"thread\":%u,\"type\":%u,\"sequence\":%llu,\"valid\":%s,\"registration_state\":%u,\"owner\":%u,\"rule\":%u,\"mode\":%u,\"selected\":%u,\"native_result\":%d,\"source_units\":[%u,%u,%u,%u],\"native_units\":[%u,%u,%u,%u],\"before_bank_zero\":[%u,%u,%u,%u],\"dropped\":%ld}\n",
                runId,GetCurrentProcessId(),s.thread,s.type,s.sequence,s.valid?"true":"false",s.state,s.owner,s.rule,s.mode,s.selected,s.result,
                s.source[0],s.source[1],s.source[2],s.source[3],s.native[0],s.native[1],s.native[2],s.native[3],
                s.before_zero[0],s.before_zero[1],s.before_zero[2],s.before_zero[3],InterlockedCompareExchange(&dropped,0,0));
            ++cursor;std::fflush(file);
        }
    }
    std::fclose(file);return 0;
}
}
SDGO_MOD_ENTRY {
    if (!api || api->version != 1) return 0;
    api->SettingString(mod,"RunId","",runId,sizeof(runId));if (std::strlen(runId) != 36) return 0;
    for (const char* c = runId; *c; ++c) if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') || *c == '-')) return 0;
    const auto provider = GetModuleHandleW(L"room_random.dll");if (!provider) return 0;
    const auto attach = reinterpret_cast<SdgoBattleRegisterHumanUnitObserverV1Fn>(GetProcAddress(provider,SDGO_BATTLE_HUMAN_UNIT_OBSERVER_EXPORT));
    readStatus = reinterpret_cast<SdgoBattleReadHumanUnitObserverStatusV1Fn>(GetProcAddress(provider,SDGO_BATTLE_HUMAN_UNIT_STATUS_EXPORT));
    if (!attach || !readStatus) return 0;
    output = std::wstring(api->ModDirectory(mod))+L"\\human-lifecycle-"+std::wstring(runId,runId+36)+L".jsonl";
    if (GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES) return 0;
    stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);if (!stopEvent) return 0;
    worker = CreateThread(nullptr,0,&Flush,nullptr,0,nullptr);
    if (!worker) { CloseHandle(stopEvent);return 0; }
    const SdgoBattleHumanUnitObserverSinkV1 sink{sizeof(sink),1,&Observe,nullptr};
    if (!attach(&sink)) {
        SetEvent(stopEvent);WaitForSingleObject(worker,INFINITE);CloseHandle(worker);CloseHandle(stopEvent);return 0;
    }
    InterlockedExchange(&registered,1);return 1;
}
