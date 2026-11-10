#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/ace_ai_observer.h"
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
SdgoAceAiAnnouncementSnapshotV1 g_announcement{};
SRWLOCK g_announcementLock = SRWLOCK_INIT;
volatile LONG g_announcementSequence = 0;
volatile LONG g_announcementCount = 0;

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
    g_announcement = receipt;
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
        *reinterpret_cast<uint32_t*>(service + 0x3C) != 3)
        return false;
    void* head = *reinterpret_cast<void**>(service + 0x94);
    const uint32_t before = *reinterpret_cast<uint32_t*>(service + 0x98);
    if (!head || before > 256) return false;
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
    return appended;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoAceAiActiveV1() {
    return g_active ? 1 : 0;
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
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoAceAiGetAnnouncementSnapshotV1(SdgoAceAiAnnouncementSnapshotV1* output) {
    if (!output || output->size < sizeof(*output) ||
        output->version != SDGO_ACE_AI_OBSERVER_VERSION) return 0;
    AcquireSRWLockShared(&g_announcementLock);
    const SdgoAceAiAnnouncementSnapshotV1 snapshot = g_announcement;
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
        !Bind(g_appendLine, "dialogue.battle_chat.append"))
        return 0;
    g_active = true;
    api->Log(mod, SDGO_LOG_INFO, "ready: %u Ace pilot identities",
             static_cast<unsigned>(g_catalog.Size()));
    return 1;
}
