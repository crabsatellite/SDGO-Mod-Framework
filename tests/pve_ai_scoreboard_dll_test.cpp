// Execute the production PvE TAB roster DLL against a source-backed x86 memory fixture.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sdgo/pve_ai_scoreboard_observer.h"
#include "sdgo/sdgo_api.h"

namespace {

static_assert(sizeof(void*) == 4,
              "PvE scoreboard ABI fixture requires the x86 build");

constexpr size_t kRecordSize = 0x344;
constexpr size_t kPlayerSlotBase = 0x94;
constexpr size_t kPlayerSlotSize = 0x70;
constexpr size_t kPlayerCount = 0x8C;
constexpr size_t kRosterOrder = 0x1D94;
constexpr size_t kPanelRows = 0x150;
constexpr size_t kPanelOffsets = 0x170;
constexpr size_t kPanelState = 0x38;
constexpr size_t kPanelVisibleA = 0x3C;
constexpr size_t kPanelVisibleB = 0x3D;

using RenderHook = void(__fastcall*)(uint8_t*, void*);

unsigned checks = 0;
unsigned hookCalls = 0;
unsigned nativeCalls = 0;
unsigned infoLogs = 0;
RenderHook renderHook = nullptr;
RenderHook textHook = nullptr;
void* hpSite = nullptr;
void* hpObserverSite = nullptr;
SdgoPveAiScoreboardGetStatusV1Fn statusFn = nullptr;
void* unitManagerPointer = nullptr;

std::array<uint8_t, 0x500> app{};
std::array<uint8_t, 0x40> service{};
std::array<uint8_t, 0x80> room{};
std::array<uint8_t, 0x700> mode{};
std::array<uint8_t, 0x2200> unitManager{};
std::array<uint8_t, 0x300> panel{};
std::array<uint8_t, kRecordSize> humanRecords{};
std::array<uint8_t, kRecordSize * 3> aiRecords{};
std::array<std::array<uint8_t, kRecordSize>, 3> copiedRecords{};
std::array<std::array<uint8_t, 0x3240>, 3> units{};
std::array<void*, 16> missionVtable{};
std::array<void*, 16> otherModeVtable{};
std::array<void*, 16> panelVtable{};
std::array<uint8_t, 16> rosterInitMarker{};

std::vector<int> renderedSlots;
std::vector<uint32_t> renderedObjects;
std::vector<int32_t> renderedKills;
std::vector<int32_t> renderedAssists;
std::vector<int32_t> renderedPoints;
std::vector<uint32_t> renderedRecordPilots;
std::vector<uint32_t> renderedRecordNpcData;
std::vector<std::array<float, 3>> renderedOffsets;
std::vector<uint8_t*> renderedHpUnits;
int rowsSeenByNative = -1;
int playerCountSeenByNative = -1;
bool raiseInNative = false;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}

template <typename T>
void Put(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value,
                sizeof(value));
}

template <typename T>
T Get(const void* base, size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const uint8_t*>(base) + offset,
                sizeof(value));
    return value;
}

uint8_t* Slot(int index) {
    return unitManager.data() + kPlayerSlotBase + index * kPlayerSlotSize;
}

int* Order() {
    return reinterpret_cast<int*>(unitManager.data() + kRosterOrder);
}

float* RowOffset(int row) {
    return reinterpret_cast<float*>(panel.data() + kPanelOffsets + row * 12);
}

uint8_t* __cdecl App() { return app.data(); }

uint8_t* __fastcall PlayerSlot(void* self, void*, int index) {
    if (self != unitManager.data() || index < 0 || index >= 12)
        return nullptr;
    return static_cast<uint8_t*>(self) + kPlayerSlotBase +
        index * kPlayerSlotSize;
}

uint8_t* __fastcall FindObject(void* self, void*, uint32_t objectId) {
    if (self != unitManager.data()) return nullptr;
    for (auto& unit : units)
        if (Get<uint32_t>(unit.data(), 0x04) == objectId)
            return unit.data();
    return nullptr;
}

__declspec(naked) void SiteReturn() { __asm { ret } }
// Reproduce the two original HP call-site contexts (EAX result, EBP-40 slot).
__declspec(naked) uint8_t* __cdecl InvokeHpSite(void*, const uint8_t*, uint8_t*) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 40h
        mov eax, [ebp+0Ch]
        mov [ebp-40h], eax
        mov eax, [ebp+10h]
        call dword ptr [ebp+8]
        mov esp, ebp
        pop ebp
        ret
    }
}

void __fastcall NativeRender(uint8_t* owner, void*) {
    ++nativeCalls;
    rowsSeenByNative = Get<int>(owner, kPanelRows);
    playerCountSeenByNative = Get<int>(unitManager.data(), kPlayerCount);
    renderedSlots.clear();
    renderedObjects.clear();
    renderedKills.clear();
    renderedAssists.clear();
    renderedPoints.clear();
    renderedRecordPilots.clear();
    renderedRecordNpcData.clear();
    renderedOffsets.clear();
    renderedHpUnits.clear();
    for (int row = 0; row < rowsSeenByNative; ++row) {
        // The real panel renders both native order arrays (00533B60 and
        // 00533C70). The latter must never reference an unregistered slot.
        const auto* secondary = reinterpret_cast<const int*>(unitManager.data()+0x1C94);
        if (secondary[row] != Order()[row])
            throw std::runtime_error("native secondary TAB order is incomplete");
        const int slotIndex = Order()[row];
        renderedSlots.push_back(slotIndex);
        uint8_t* slot = Slot(slotIndex);
        if (slotIndex > 0) {
            uint8_t* first = InvokeHpSite(hpSite, slot, nullptr);
            uint8_t* second = InvokeHpSite(hpObserverSite, slot, nullptr);
            if (first != second) throw std::runtime_error("HP lookup passes disagree");
            renderedHpUnits.push_back(first);
        }
        renderedObjects.push_back(Get<uint32_t>(slot, 0x04));
        renderedKills.push_back(Get<int32_t>(slot, 0x4C));
        renderedAssists.push_back(Get<int32_t>(slot, 0x54));
        renderedPoints.push_back(Get<int32_t>(slot, 0x58));
        const uint8_t* record = Get<uint8_t*>(slot, 0x44);
        renderedRecordPilots.push_back(
            record ? Get<uint32_t>(record, 0x00) : 0u);
        renderedRecordNpcData.push_back(
            record ? Get<uint32_t>(record, 0x44) : 0u);
        float* offset = RowOffset(row);
        renderedOffsets.push_back({offset[0], offset[1], offset[2]});
    }
    SetLastError(0x2468ACE0);
    if (raiseInNative) RaiseException(0xE0424242, 0, 0, nullptr);
}

void __cdecl Log(SdgoMod*, int level, const char*, ...) {
    if (level == SDGO_LOG_INFO) ++infoLogs;
}

void* __cdecl Symbol(SdgoMod*, const char* name) {
    if (!std::strcmp(name, "game.app"))
        return reinterpret_cast<void*>(&App);
    if (!std::strcmp(name, "game.unit_manager"))
        return &unitManagerPointer;
    if (!std::strcmp(name, "um.player_slot"))
        return reinterpret_cast<void*>(&PlayerSlot);
    if (!std::strcmp(name, "um.find_object"))
        return reinterpret_cast<void*>(&FindObject);
    if (!std::strcmp(name, "CMissionMode.vtbl"))
        return missionVtable.data();
    if (!std::strcmp(name, "hud.battle_roster_init"))
        return rosterInitMarker.data();
    if (!std::strcmp(name, "hud.battle_roster_render"))
        return reinterpret_cast<void*>(0x00533ED0);
    return nullptr;
}

int __cdecl HookCode(SdgoMod*, const char* name, void* replacement,
                     void** original) {
    if (!std::strcmp(name, "hud.battle_roster_render"))
        renderHook = reinterpret_cast<RenderHook>(replacement);
    else if (!std::strcmp(name, "hud.battle_roster_text_render"))
        textHook = reinterpret_cast<RenderHook>(replacement);
    else return 0;
    ++hookCalls;
    *original = reinterpret_cast<void*>(&NativeRender);
    return 1;
}

int __cdecl HookSite(SdgoMod*, const char* name, void* replacement, void** original) {
    if (!std::strcmp(name, "hud.battle_roster_hp_unit")) hpSite = replacement;
    else if (!std::strcmp(name, "hud.battle_roster_hp_observer_unit")) hpObserverSite = replacement;
    else return 0;
    *original = reinterpret_cast<void*>(&SiteReturn);
    return 1;
}

void SetSpan(size_t beginOffset, size_t endOffset, uint8_t* first,
             int count) {
    Put(room.data(), beginOffset, first);
    Put(room.data(), endOffset,
        first ? first + static_cast<size_t>(count) * kRecordSize : nullptr);
}

void SetAi(int index, bool valid) {
    uint8_t* authoritative = aiRecords.data() + index * kRecordSize;
    const uint32_t pilot = 0x80000002u + static_cast<uint32_t>(index);
    const uint32_t connection = 0; // native server-owned AI has no player socket
    const uint32_t npc = 9001u + static_cast<uint32_t>(index);
    const uint8_t team = 1;
    Put(authoritative, 0x00, pilot);
    Put(authoritative, 0x04, connection);
    authoritative[0x1A] = team;
    Put(authoritative, 0x44, npc);
    copiedRecords[index] = {};
    std::memcpy(copiedRecords[index].data(), authoritative, kRecordSize);
    uint8_t* slot = Slot(1 + index);
    const wchar_t name[] = L"AI";
    std::memcpy(slot + 8, name, sizeof(name));
    Put(slot, 0x18, uint32_t{2});
    Put(slot, 0x1C, uint32_t{7});
    Put(slot, 0x04, 1001u + static_cast<uint32_t>(index));
    Put(slot, 0x2C, connection);
    Put(slot, 0x44, copiedRecords[index].data());
    Put(slot, 0x4C, 10 + index);
    Put(slot, 0x54, 20 + index);
    Put(slot, 0x58, 30 + index);
    units[index] = {};
    Put(units[index].data(), 0x04,
        1001u + static_cast<uint32_t>(index));
    units[index][0x20] = team;
    Put(units[index].data(), 0x2B68, valid ? pilot : pilot + 100u);
    Put(units[index].data(), 0x3238, npc);
    Put(units[index].data(), 0x38, 7500);
    Put(units[index].data(), 0x3C, 10000);
}

void SetAiWaitingForRespawn(int index, bool clearObjectId) {
    units[index] = {};
    if (clearObjectId) Put(Slot(1 + index), 0x04, uint32_t{0});
}

void ResetFixture() {
    app = {};
    service = {};
    room = {};
    mode = {};
    unitManager = {};
    panel = {};
    humanRecords = {};
    aiRecords = {};
    copiedRecords = {};
    units = {};
    missionVtable.fill(nullptr);
    otherModeVtable.fill(nullptr);
    panelVtable.fill(nullptr);
    unitManagerPointer = unitManager.data();
    Put(app.data(), 0x4A0, uint32_t{22});
    Put(app.data(), 0x4A4, service.data());
    Put(service.data(), 0x0C, mode.data());
    Put(mode.data(), 0x00, missionVtable.data());
    Put(mode.data(), 0x08, room.data());
    SetSpan(0x1C, 0x20, humanRecords.data(), 1);
    SetSpan(0x3C, 0x40, aiRecords.data(), 3);
    Put(unitManager.data(), kPlayerCount, 1);
    std::fill(Order(), Order() + 64, -1);
    Order()[0] = 0;
    auto* secondary = reinterpret_cast<int*>(unitManager.data()+0x1C94);
    std::fill(secondary,secondary+64,-1);
    secondary[0]=0;
    panelVtable[10] = rosterInitMarker.data();
    Put(panel.data(), 0x00, panelVtable.data());
    Put(panel.data(), kPanelRows, 1);
    RowOffset(0)[0] = -41.5f;
    RowOffset(0)[1] = 2.0f;
    RowOffset(0)[2] = 0.25f;
    Put(Slot(0), 0x04, 44u);
    Put(humanRecords.data(), 0x00, 4001u);
    humanRecords[0x1A] = 1;
    Put(humanRecords.data(), 0x44, 8001u);
    Put(Slot(0), 0x44, humanRecords.data());
    Put(Slot(0), 0x4C, 1);
    Put(Slot(0), 0x54, 2);
    Put(Slot(0), 0x58, 3);
    for (int i = 0; i < 3; ++i) {
        float* offset = RowOffset(1 + i);
        offset[0] = 100.0f + static_cast<float>(i);
        offset[1] = 200.0f + static_cast<float>(i);
        offset[2] = 300.0f + static_cast<float>(i);
        SetAi(i, true);
    }
    renderedSlots.clear();
    renderedObjects.clear();
    renderedKills.clear();
    renderedAssists.clear();
    renderedPoints.clear();
    renderedOffsets.clear();
    rowsSeenByNative = -1;
    playerCountSeenByNative = -1;
    raiseInNative = false;
}

void Call() {
    const unsigned before = nativeCalls;
    renderHook(panel.data(), nullptr);
    Check(nativeCalls == before + 1,
          "native roster render is called exactly once");
}

bool CallExpectingNativeException() {
    bool caught = false;
    __try {
        renderHook(panel.data(), nullptr);
    } __except (GetExceptionCode() == 0xE0424242
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        caught = true;
    }
    return caught;
}

SdgoPveAiScoreboardStatusV1 Status() {
    SdgoPveAiScoreboardStatusV1 value{
        sizeof(SdgoPveAiScoreboardStatusV1),
        SDGO_PVE_AI_SCOREBOARD_STATUS_VERSION};
    Check(statusFn(&value) == 1, "passive status export reads the fixture");
    return value;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("pass the built PvE scoreboard DLL");
        HMODULE module = LoadLibraryW(argv[1]);
        Check(module != nullptr, "actual production DLL loads");
        auto entry = reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(module, "SdgoModLoad"));
        statusFn = reinterpret_cast<SdgoPveAiScoreboardGetStatusV1Fn>(
            GetProcAddress(module, SDGO_PVE_AI_SCOREBOARD_STATUS_EXPORT));
        Check(entry && statusFn,
              "loader and passive status export are present");
        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.host_key = "GOnline-54BE1568-00C97000";
        api.Log = &Log;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        api.HookSite = &HookSite;
        Check(entry(&api, reinterpret_cast<SdgoMod*>(1)) == 1,
              "production Mod binds the verified native render boundary");
        Check(hookCalls == 2 && renderHook && textHook && hpSite && hpObserverSite,
              "native graphics and text passes plus both bounded HP result sites are installed");

        ResetFixture();
        const auto beforeUm = unitManager;
        const auto beforePanel = panel;
        SetLastError(0x13572468);
        Call();
        Check(rowsSeenByNative == 4 &&
                  renderedSlots == std::vector<int>({0, 1, 2, 3}),
              "native TAB loop receives the human and three exact AI slots");
        Check(playerCountSeenByNative == 1 &&
                  Get<int>(unitManager.data(), kPlayerCount) == 1,
              "mission gameplay player count remains human-only");
        Check(renderedHpUnits == std::vector<uint8_t*>({units[0].data(), units[1].data(), units[2].data()}),
              "both native HP passes resolve the exact live registered wingmen");
        Check(InvokeHpSite(hpSite, Slot(1), nullptr) == nullptr &&
                  InvokeHpSite(hpObserverSite, Slot(1), units[0].data()) == units[0].data(),
              "outside roster drawing missing lookups stay missing and native successes remain unchanged");
        Check(renderedObjects ==
                  std::vector<uint32_t>({44u, 1001u, 1002u, 1003u}),
              "native rows receive each registered object without substitution");
        Check(renderedKills == std::vector<int32_t>({1, 10, 11, 12}) &&
                  renderedAssists == std::vector<int32_t>({2, 20, 21, 22}) &&
                  renderedPoints == std::vector<int32_t>({3, 30, 31, 32}),
              "native kill, assist, and point fields remain intact");
        Check(renderedRecordPilots ==
                  std::vector<uint32_t>({4001u, 0x80000002u, 0x80000003u, 0x80000004u}) &&
                  renderedRecordNpcData ==
                  std::vector<uint32_t>({8001u, 9001u, 9002u, 9003u}),
              "native rows receive the exact human and AI identity records");
        Check(renderedOffsets.size() == 4 &&
                  renderedOffsets[1] == renderedOffsets[0] &&
                  renderedOffsets[2] == renderedOffsets[0] &&
                  renderedOffsets[3] == renderedOffsets[0],
              "AI rows inherit the native human row animation state");
        Check(unitManager == beforeUm && panel == beforePanel,
              "row count, order, offsets, slots, and gameplay memory restore after draw");
        Check(GetLastError() == 0x2468ACE0,
              "native render last-error result is preserved");
        const auto active = Status();
        Check(active.active == 1 && active.human_rows == 1 &&
                  active.ai_rows == 3 && active.total_rows == 4 &&
                  active.sync_count >= 1 && active.render_count >= 1,
              "passive receipt reports the exact displayed roster");
        Check(active.panel_visible == 0 && active.panel_state == 0 &&
                  active.visible_render_count == 0,
              "a synchronized hidden TAB panel does not claim a visible render");

        ResetFixture();
        const auto hidden = Status();
        Put(panel.data(), kPanelState, uint32_t{2});
        panel[kPanelVisibleA] = 1;
        panel[kPanelVisibleB] = 1;
        Call();
        const auto visible = Status();
        Check(visible.panel_visible == 1 && visible.panel_state == 2 &&
                  visible.visible_render_count == hidden.visible_render_count + 1,
              "only a completed native render with both TAB flags and state 2 publishes visible evidence");
        const auto textBeforeUm = unitManager;
        const auto textBeforePanel = panel;
        textHook(panel.data(), nullptr);
        const auto textVisible = Status();
        Check(rowsSeenByNative == 4 && textVisible.visible_text_render_count >= 1 &&
                  textVisible.named_ai_rows == 3,
              "the separate native font pass receives all three nonempty AI slot names");
        Check(unitManager == textBeforeUm && panel == textBeforePanel,
              "native text drawing restores the same complete roster memory");

        ResetFixture();
        Put(units[0].data(), 0x38, -180);
        Put(units[1].data(), 0x3C, 0);
        Put(units[2].data(), 0x38, 10001);
        Call();
        Check(renderedHpUnits == std::vector<uint8_t*>({nullptr, nullptr, nullptr}),
              "defeated negative HP, zero maximum and invalid over-maximum rows never reach native bar division");
        Check(Get<int>(units[0].data(), 0x38) == -180 &&
                  Get<int>(units[1].data(), 0x3C) == 0 &&
                  Get<int>(units[2].data(), 0x38) == 10001,
              "invalid HP presentation inputs are rejected without rewriting native health");

        ResetFixture();
        SetAiWaitingForRespawn(0, false);
        SetAiWaitingForRespawn(1, true);
        Call();
        Check(rowsSeenByNative == 4 &&
                  renderedSlots == std::vector<int>({0, 1, 2, 3}),
              "dead and waiting-respawn AI remain in the native TAB roster");
        Check(renderedRecordPilots ==
                  std::vector<uint32_t>({4001u, 0x80000002u, 0x80000003u, 0x80000004u}) &&
                  renderedRecordNpcData ==
                  std::vector<uint32_t>({8001u, 9001u, 9002u, 9003u}),
              "respawning AI keep native name and unit identity records");

        ResetFixture();
        SetAi(1, false);
        Call();
        Check(rowsSeenByNative == 3 &&
                  renderedSlots == std::vector<int>({0, 1, 3}),
              "a stale AI slot is omitted while other exact slots remain visible");
        Check(Get<int>(panel.data(), kPanelRows) == 1 && Order()[1] == -1,
              "partial roster patch is scoped to the native draw call");

        ResetFixture();
        SetAi(0, false);
        SetAi(1, false);
        SetAi(2, false);
        const auto staleUm = unitManager;
        const auto stalePanel = panel;
        Call();
        Check(rowsSeenByNative == 1 && renderedSlots == std::vector<int>({0}),
              "spawn-failed AI slots keep the original human roster");
        Check(unitManager == staleUm && panel == stalePanel,
              "rejected stale slots do not mutate roster state");
        const auto rejected = Status();
        Check(rejected.active == 0 && rejected.rejected_count >= 1,
              "passive receipt records authoritative AI rejected by validation");

        ResetFixture();
        Put(mode.data(), 0x00, otherModeVtable.data());
        Call();
        Check(rowsSeenByNative == 1 &&
                  Get<int>(unitManager.data(), kPlayerCount) == 1,
              "non-mission modes retain their original roster");

        ResetFixture();
        Put(unitManager.data(), kPlayerCount, 4);
        Call();
        Check(rowsSeenByNative == 1,
              "unexpected gameplay count refuses the display-only patch");

        ResetFixture();
        SetSpan(0x3C, 0x40, nullptr, 0);
        Call();
        Check(rowsSeenByNative == 1,
              "missions without authoritative AI retain the original roster");

        ResetFixture();
        const auto sehUm = unitManager;
        const auto sehPanel = panel;
        raiseInNative = true;
        const bool caught = CallExpectingNativeException();
        Check(caught, "native render exception is not swallowed");
        Check(unitManager == sehUm && panel == sehPanel,
              "SEH finalization restores every scoped roster byte");
        Check(InvokeHpSite(hpSite, Slot(1), nullptr) == nullptr,
              "native exceptions also clear the thread-local HP drawing scope");

        std::printf("%u checks passed\n", checks);
        FreeLibrary(module);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
