#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include "sdgo/sdgo_api.h"
#include "sdgo/ace_ai_observer.h"
#include "sdgo/ace_ai_client.h"
#include "sdgo/ai_start_units.h"
#include "pilot_catalog.h"

namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
bool g_active = false;
battle_ai::ace::PilotCatalog g_catalog;
using AppFn = uint8_t*(__cdecl*)();
using StepFn = void(__thiscall*)(void*);
using StringCtorFn = void*(__thiscall*)(void*, const wchar_t*, size_t);
using AppendLineFn = void(__thiscall*)(void*, const void*);
AppFn g_app = nullptr;
StepFn g_lineCtor = nullptr;
StepFn g_lineDtor = nullptr;
StringCtorFn g_stringCtor = nullptr;
AppendLineFn g_appendLine = nullptr;
using NameplateFn = void(__thiscall*)(uint8_t*, const void*, uint32_t, uint32_t);
static_assert(sizeof(std::wstring) == 24, "native x86 string layout changed");
NameplateFn g_originalNameplate = nullptr;
struct NameplateBinding {
    uint8_t* unit = nullptr;
    uint32_t object = 0;
    uint32_t pilot = 0;
    std::wstring name;
};
std::array<NameplateBinding, 12> g_nameplates{};
uint32_t g_nameplateGeneration = 0;
using InitUnitDataFn = void(__thiscall*)(uint8_t*,const void*,uint32_t);
InitUnitDataFn g_originalInitUnitData = nullptr;
SdgoAiStartUnitV1Fn g_startUnit = nullptr;
uint32_t g_nativeMaximumCorrections = 0;

bool NameplateMatches(const NameplateBinding& binding, const uint8_t* unit) {
    __try {
        if (!g_active || !unit || binding.unit != unit ||
            *reinterpret_cast<const uint32_t*>(unit + 4) != binding.object ||
            *reinterpret_cast<const uint32_t*>(unit + 0x2B68) != binding.pilot ||
            (binding.pilot & 0xC0000000u) != 0xC0000000u)
            return false;
        const uint32_t npc = *reinterpret_cast<const uint32_t*>(unit + 0x3238);
        return npc >= 3604001 && npc <= 3607000;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void RestoreAdvertisedMaximum(uint8_t* unit) {
    __try {
        for(const auto& binding:g_nameplates) {
            if(!NameplateMatches(binding,unit)) continue;
            if(!g_startUnit) {
                const HMODULE match=GetModuleHandleW(L"match_ai.dll");
                if(match)g_startUnit=reinterpret_cast<SdgoAiStartUnitV1Fn>(
                    GetProcAddress(match,SDGO_AI_START_UNIT_EXPORT));
            }
            if(!g_startUnit)return;
            const uint32_t npc=*reinterpret_cast<uint32_t*>(unit+0x3238);
            const int32_t form=*reinterpret_cast<int32_t*>(unit+0x2EE4);
            SdgoAiStartUnitV1 request{sizeof(request),SDGO_AI_START_UNIT_VERSION,
                binding.pilot,npc,form,g_nameplateGeneration,0,0};
            if(g_startUnit(&request)!=1 || request.generation!=g_nameplateGeneration ||
               !request.max_hp || request.max_hp>0x7fffffffu ||
               request.pilot_id!=binding.pilot || request.npc_id!=npc || request.form!=form ||
               !NameplateMatches(binding,unit))return;
            // InitUnitData restores a machine's original maximum on internal form
            // changes. The server-start value is authoritative; current HP is not
            // part of that packet and must remain exactly as the native path left it.
            const uint32_t previous=*reinterpret_cast<uint32_t*>(unit+0x3C);
            *reinterpret_cast<uint32_t*>(unit+0x3C)=request.max_hp;
            if(previous!=request.max_hp && ++g_nativeMaximumCorrections<=16 && g_api && g_api->Log)
                g_api->Log(g_mod,SDGO_LOG_INFO,
                    "native Ace form maximum restored: pilot=%08lX form=%ld old=%lu server=%lu",
                    binding.pilot,form,previous,request.max_hp);
            return;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall InitUnitData(uint8_t* unit,void*,const void* definition,uint32_t options) {
    g_originalInitUnitData(unit,definition,options);
    RestoreAdvertisedMaximum(unit);
}

// Native CBaseGameUnit message 6 owns this renderer. The original creates and
// destroys its own nameplate texture at unit+0xE5C and consumes the string now.
// Keep the caller's colour and pilot arguments, including on later respawns.
void __fastcall Nameplate(uint8_t* unit, void*, const void* text,
                          uint32_t colour, uint32_t pilot) {
    for (const auto& binding : g_nameplates) {
        if (NameplateMatches(binding, unit)) {
            g_originalNameplate(unit, &binding.name, colour, pilot);
            return;
        }
    }
    g_originalNameplate(unit, text, colour, pilot);
}

int SetNameplate(uint8_t* unit, const wchar_t* name, uint32_t generation) {
    if (!g_active || !g_originalNameplate || !unit || !name || !generation ||
        generation < g_nameplateGeneration) return 0;
    const size_t length = wcsnlen_s(name, 65);
    if (!length || length >= 65) return 0;
    NameplateBinding identity;
    identity.unit = unit;
    identity.object = *reinterpret_cast<uint32_t*>(unit + 4);
    identity.pilot = *reinterpret_cast<uint32_t*>(unit + 0x2B68);
    if (!identity.object || !NameplateMatches(identity, unit)) return 0;
    if (generation != g_nameplateGeneration) {
        for (auto& binding : g_nameplates) binding = {};
        g_nameplateGeneration = generation;
    }
    NameplateBinding* selected = nullptr;
    for (auto& binding : g_nameplates)
        if (binding.unit == unit) { selected = &binding; break; }
    if (!selected)
        for (auto& binding : g_nameplates)
            if (!binding.unit) { selected = &binding; break; }
    if (!selected) return 0;
    identity.name.assign(name, length);
    *selected = std::move(identity);
    auto* plate = *reinterpret_cast<uint8_t**>(unit + 0xE5C);
    if (!plate) return 0; // A later native message 6 will consume the binding.
    const uint32_t colour = *reinterpret_cast<uint32_t*>(plate + 0x148);
    g_originalNameplate(unit, &selected->name, colour, selected->pilot);
    return *reinterpret_cast<uint8_t**>(unit + 0xE5C) ? 1 : 0;
}
SdgoAceAiAnnouncementSnapshotV1 g_announcementLatest{};
SdgoAceAiAnnouncementSnapshotV1 g_announcements[2]{};
SRWLOCK g_announcementLock = SRWLOCK_INIT;
volatile LONG g_announcementSequence = 0;
volatile LONG g_announcementCount = 0;
volatile LONG g_announcementFailureKinds = 0;

void LogAnnouncementFailure(LONG kind, uint32_t phase, uint32_t count) {
    const LONG prior = InterlockedOr(&g_announcementFailureKinds, kind);
    if (!(prior & kind) && g_api && g_api->Log)
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "native Ace chat append unavailable: reason=%ld phase=%lu count=%lu",
                   kind, static_cast<unsigned long>(phase), static_cast<unsigned long>(count));
}

void PublishAnnouncement(uint32_t colour, uint32_t before, uint32_t after,
                         const wchar_t* text) {
    SdgoAceAiAnnouncementSnapshotV1 receipt{};
    receipt.size = sizeof(receipt);
    receipt.version = SDGO_ACE_AI_OBSERVER_VERSION;
    receipt.sequence = static_cast<uint32_t>(
        InterlockedIncrement(&g_announcementSequence));
    receipt.source = SDGO_ACE_AI_ANNOUNCEMENT_PVP;
    receipt.colour = colour;
    receipt.feed_count_before = before;
    receipt.feed_count_after = after;
    receipt.successful_count = static_cast<uint32_t>(
        InterlockedIncrement(&g_announcementCount));
    wcsncpy_s(receipt.text, text, _TRUNCATE);
    AcquireSRWLockExclusive(&g_announcementLock);
    g_announcementLatest = receipt;
    g_announcements[colour - 1] = receipt;
    ReleaseSRWLockExclusive(&g_announcementLock);
}

template<class T> bool Bind(T& value, const char* symbol) {
    value = reinterpret_cast<T>(g_api->Symbol(g_mod, symbol));
    return value != nullptr;
}

bool AppendAnnouncement(uint8_t* mode, uint32_t colour) {
    if (!mode || (colour != 1 && colour != 2) || !g_active ||
        !g_app || !g_lineCtor || !g_lineDtor || !g_stringCtor || !g_appendLine)
        return false;
    const wchar_t* line = colour == 1 ? L"红队出现了王牌驾驶员！" : L"蓝队出现了王牌驾驶员！";
    uint8_t* game = g_app();
    uint8_t* service = game ? *reinterpret_cast<uint8_t**>(game + 0x4A4) : nullptr;
    if (!service || *reinterpret_cast<uint8_t**>(service + 0x0C) != mode ||
        *reinterpret_cast<uint32_t*>(service + 0x3C) != 3) {
        LogAnnouncementFailure(1, service ? *reinterpret_cast<uint32_t*>(service + 0x3C) : 0, 0);
        return false;
    }
    void* head = *reinterpret_cast<void**>(service + 0x94);
    const uint32_t before = *reinterpret_cast<uint32_t*>(service + 0x98);
    if (!head || before > 256) {
        LogAnnouncementFailure(2, 3, before);
        return false;
    }
    alignas(4) uint8_t record[0x68]{};
    bool constructed = false;
    bool appended = false;
    __try {
        g_lineCtor(record);
        constructed = true;
        g_stringCtor(record + 0x38, line, wcslen(line));
        *reinterpret_cast<uint32_t*>(record + 0x54) = 1000;
        *reinterpret_cast<uint32_t*>(record + 0x58) = 0x70D9FF;
        *reinterpret_cast<uint32_t*>(record + 0x5C) = 0x70D9FF;
        *reinterpret_cast<uint32_t*>(record + 0x60) = 0x70D9FF;
        *reinterpret_cast<uint16_t*>(record + 0x64) = 10;
        g_appendLine(service + 0x94, record);
        appended = *reinterpret_cast<void**>(service + 0x94) &&
            *reinterpret_cast<uint32_t*>(service + 0x98) == before + 1;
    } __finally {
        if (constructed) g_lineDtor(record);
    }
    if (appended) PublishAnnouncement(colour, before, before + 1, line);
    else LogAnnouncementFailure(4, 3, *reinterpret_cast<uint32_t*>(service + 0x98));
    return appended;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiActiveV1() {
    return g_active ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiSetNameplateV1(
    uint8_t* unit, const wchar_t* name, uint32_t generation) {
    __try { return SetNameplate(unit, name, generation); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiPilotNameV1(
    uint32_t unitId, wchar_t* output, uint32_t capacity) {
    if (!g_active || !output || capacity == 0) return 0;
    const battle_ai::ace::PilotEntry* pilot = g_catalog.Find(unitId);
    if (!pilot || pilot->displayName.size() + 1 > capacity) return 0;
    std::memcpy(output, pilot->displayName.c_str(),
                (pilot->displayName.size() + 1) * sizeof(wchar_t));
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiAnnounceTeamV1(
    uint8_t* mode, uint32_t colour) {
    __try { return AppendAnnouncement(mode, colour) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogAnnouncementFailure(8, 0, GetExceptionCode());
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoAceAiGetAnnouncementSnapshotV1(SdgoAceAiAnnouncementSnapshotV1* output) {
    if (!output || output->size < sizeof(*output) ||
        output->version != SDGO_ACE_AI_OBSERVER_VERSION) return 0;
    const uint32_t requestedColour = output->colour;
    AcquireSRWLockShared(&g_announcementLock);
    const SdgoAceAiAnnouncementSnapshotV1 snapshot =
        requestedColour >= 1 && requestedColour <= 2
            ? g_announcements[requestedColour - 1]
            : g_announcementLatest;
    ReleaseSRWLockShared(&g_announcementLock);
    if (!snapshot.sequence) return 0;
    *output = snapshot;
    return 1;
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    g_active = false;
    std::string error;
    if (!g_catalog.Load(api, mod, error)) {
        api->Log(mod, SDGO_LOG_ERROR, "Ace pilot catalog unavailable: %s", error.c_str());
        return 0;
    }
    if (!Bind(g_app, "game.app") ||
        !Bind(g_lineCtor, "dialogue.battle_chat.ctor") ||
        !Bind(g_lineDtor, "dialogue.battle_chat.dtor") ||
        !Bind(g_stringCtor, "flow.native_wstring_ctor") ||
        !Bind(g_appendLine, "dialogue.battle_chat.append") ||
        !api->HookCode(mod, "ace.unit_nameplate", reinterpret_cast<void*>(&Nameplate),
                       reinterpret_cast<void**>(&g_originalNameplate)) ||
        !api->HookCode(mod,"ace.unit_init_data",reinterpret_cast<void*>(&InitUnitData),
                       reinterpret_cast<void**>(&g_originalInitUnitData)))
        return 0;
    g_active = true;
    api->Log(mod, SDGO_LOG_INFO, "ready: %u Ace pilot identities",
             static_cast<unsigned>(g_catalog.Size()));
    return 1;
}
