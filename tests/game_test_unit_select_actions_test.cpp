// OFFLINE-ONLY ABI, identity, request, and reply tests for
// UnitSelectActions.
//
// The fixture models the shipped x86 unitmgr/profile/inventory layouts and
// invokes production code against original-method stubs. It never starts
// GOnline, contacts a server, writes a database, grants a unit, or proves
// gameplay acceptance.

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <type_traits>

#include "../mods/game_test_client/unit_select_actions.h"

namespace
{
constexpr uint64_t kTrainingUid = 100001;
constexpr uint64_t kSecondUid = 100002;
constexpr uint64_t kThirdUid = 100003;
constexpr uint64_t kTargetUid = 100004;
constexpr uint64_t kSeventhUid = 100007;
constexpr uint64_t kThirtyFirstUid = 100031;
constexpr size_t kFixtureSlots = 1200;

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using LoginDispatchDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);
using ActiveWaitReplyDetour =
    void(__fastcall*)(
        void*,
        void*,
        const void*);

int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x800] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;
uint8_t g_serviceManagerObject = 0;
void* g_serviceManager = &g_serviceManagerObject;
uint8_t g_unitManager[0x200] = {};
void* g_unitManagerVtable[32] = {};
void* g_wrongVtable[32] = {};
uint8_t g_unitRoot = 0;
uint8_t g_unitList[0x280] = {};
uint8_t g_pageControls[4][0x180] = {};
uint8_t g_rows[kFixtureSlots][0x40] = {};
uint8_t g_rowControls[kFixtureSlots][0x180] = {};
uint8_t g_unitInfo[0x80] = {};
void* g_unitInfoObject = g_unitInfo;
uint8_t g_inventory[kFixtureSlots][0x19] = {};

NetworkSendDetour g_networkSendDetour = nullptr;
LoginDispatchDetour g_loginDispatchDetour = nullptr;
ActiveWaitReplyDetour g_replyDetour = nullptr;

bool g_serviceVisible = true;
bool g_returnWrongVtable = false;
bool g_listVisible = true;
bool g_rowsReady = true;
bool g_fullReady = true;
bool g_acceptSelection = true;
bool g_sendRequest = true;
bool g_syncReply = false;
bool g_replyHookFails = false;
bool g_descriptorValid = true;
bool g_networkPacketValid = true;
bool g_pageControlsAvailable = true;
bool g_pageCallbackPending = false;
bool g_pageCallbackAbiValid = true;
const char* g_missingSymbol = nullptr;
int g_networkResult = 1;
int g_selectedIndex = -1;
unsigned g_mouseCalls = 0;
unsigned g_itemLookupCalls = 0;
unsigned g_businessCalls = 0;
unsigned g_replyOriginalCalls = 0;
unsigned g_serviceLookups = 0;
unsigned g_controlLookups = 0;
unsigned g_pageCallbackCalls = 0;
uint32_t g_lastControlKind = UINT32_MAX;
wchar_t g_lastPageControl[32] = {};

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(
        &bits,
        &value,
        sizeof(bits));
    return bits;
}

uint32_t PointerBits(const void* value)
{
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(
            value));
}

void DestroyNativeString(void* storage)
{
    if (!storage)
        return;
    auto* bytes =
        static_cast<uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            bytes + 0x14);
    if (capacity >= 8) {
        void* allocation =
            *reinterpret_cast<void**>(bytes);
        if (allocation) {
            HeapFree(
                GetProcessHeap(),
                0,
                allocation);
        }
    }
    std::memset(storage, 0, 0x1C);
}

bool WriteNativeString(
    void* storage,
    const wchar_t* value)
{
    if (!storage || !value)
        return false;
    DestroyNativeString(storage);
    auto* bytes =
        static_cast<uint8_t*>(storage);
    const size_t length =
        std::wcslen(value);
    if (length < 8) {
        std::memcpy(
            bytes,
            value,
            (length + 1) *
                sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) *
            sizeof(wchar_t);
        auto* allocation =
            static_cast<wchar_t*>(
                HeapAlloc(
                    GetProcessHeap(),
                    HEAP_ZERO_MEMORY,
                    byteCount));
        if (!allocation)
            return false;
        std::memcpy(
            allocation,
            value,
            byteCount);
        *reinterpret_cast<wchar_t**>(
            bytes) = allocation;
        *reinterpret_cast<uint32_t*>(
            bytes + 0x14) =
                static_cast<uint32_t>(
                    length);
    }
    *reinterpret_cast<uint32_t*>(
        bytes + 0x10) =
            static_cast<uint32_t>(
                length);
    return true;
}

const wchar_t* ReadNativeString(
    const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(
            storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            bytes + 0x14);
    return capacity < 8
        ? reinterpret_cast<
              const wchar_t*>(bytes)
        : *reinterpret_cast<
              wchar_t* const*>(bytes);
}

void WriteBrief(
    int index,
    uint32_t unitId,
    uint64_t uid,
    uint16_t slot)
{
    if (index < 0 ||
        index >=
            static_cast<int>(
                kFixtureSlots))
        return;
    std::memset(
        g_inventory[index],
        0,
        sizeof(g_inventory[index]));
    std::memcpy(
        g_inventory[index],
        &unitId,
        sizeof(unitId));
    std::memcpy(
        g_inventory[index] + 4,
        &uid,
        sizeof(uid));
    std::memcpy(
        g_inventory[index] + 12,
        &slot,
        sizeof(slot));
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(
    const void*,
    char* output,
    int)
{
    if (output)
        std::memset(output, '0', 32);
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void* network,
    void*,
    void* stream,
    const void* packet,
    int size,
    char flag)
{
    const auto* bytes =
        static_cast<const uint8_t*>(
            packet);
    const bool valid =
        network == g_network &&
        stream == &g_stream &&
        flag == 0 &&
        bytes &&
        size == 14 &&
        *reinterpret_cast<
            const uint16_t*>(bytes) ==
            0x04CF;
    if (!valid)
        g_networkPacketValid = false;
    return g_networkResult;
}

int __fastcall FakeLoginDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 0;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    return WriteNativeString(
               self,
               value)
        ? self
        : nullptr;
}

void __fastcall FakeNativeWStringDtor(
    void* self,
    void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeNicknameDialogCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* object,
    int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(
    void*,
    void*)
{
}

void __fastcall FakeNicknameDialogConfirm(
    void*,
    void*)
{
}

void __fastcall FakeUnitBriefBySlot(
    void* self,
    void*,
    void* output,
    uint16_t slot)
{
    if (self != g_unitInfo ||
        !output ||
        !slot) {
        return;
    }
    if (slot > kFixtureSlots) {
        std::memset(
            output,
            0,
            sizeof(g_inventory[0]));
        return;
    }
    std::memcpy(
        output,
        g_inventory[slot - 1],
        sizeof(g_inventory[0]));
}

void __fastcall FakeUnitFullBySlot(void*,void*,void* output,uint16_t slot)
{
    std::memset(output,0,0x99);
    if(!g_fullReady || slot<1 || slot>kFixtureSlots)return;
    auto* full=static_cast<uint8_t*>(output);
    std::memcpy(full,g_inventory[slot-1],4);
    std::memcpy(full+8,g_inventory[slot-1]+4,8);
}

void* __fastcall FakeServiceFindByName(
    void* self,
    void*,
    const void* descriptor)
{
    ++g_serviceLookups;
    if (self != g_serviceManager ||
        !descriptor) {
        g_descriptorValid = false;
        return nullptr;
    }
    const auto* bytes =
        static_cast<const uint8_t*>(
            descriptor);
    const wchar_t* name =
        ReadNativeString(
            bytes + sizeof(uint32_t));
    if (*reinterpret_cast<
            const uint32_t*>(bytes) != 1 ||
        !name ||
        std::wcscmp(
            name,
            L"unitmgr") != 0) {
        g_descriptorValid = false;
        return nullptr;
    }
    return g_serviceVisible
        ? g_unitManager
        : nullptr;
}

const wchar_t* const kPageControlNames[4] = {
    L"LeftBtn",
    L"RightBtn",
    L"LeftOneBtn",
    L"RightOneBtn",
};

void* PageControlByName(
    const wchar_t* name)
{
    if (!name)
        return nullptr;
    for (size_t index = 0;
         index < 4;
         ++index) {
        if (std::wcscmp(
                name,
                kPageControlNames[index]) == 0)
            return g_pageControls[index];
    }
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t kind)
{
    ++g_controlLookups;
    g_lastControlKind = kind;
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (root != &g_unitRoot ||
        !name)
        return nullptr;
    if (kind == 7)
        return g_listVisible &&
                   std::wcscmp(
                       name,
                       L"UnitList") == 0
            ? g_unitList
            : nullptr;
    if (kind == 4 &&
        g_pageControlsAvailable)
        return PageControlByName(name);
    return nullptr;
}

void RefreshOccupied(uint32_t page)
{
    for (uint32_t index = 0;
         index < 6;
         ++index) {
        const uint32_t slot =
            page * 6 +
            index;
        g_unitManager[0xD4 + index] =
            slot < kFixtureSlots &&
                    *reinterpret_cast<uint32_t*>(
                        g_inventory[slot]) != 0
                ? 1
                : 0;
    }
}

uint32_t __stdcall FakeUnitManagerCallback(
    void* service,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_pageCallbackCalls;
    const wchar_t* name =
        ReadNativeString(nativeName);
    const bool valid =
        service == g_unitManager &&
        event == 0x101 &&
        control &&
        name &&
        nativeName ==
            static_cast<uint8_t*>(control) +
                0x118 &&
        PageControlByName(name) == control &&
        *reinterpret_cast<void**>(
            static_cast<uint8_t*>(control) +
            0x15C) == &g_unitRoot;
    if (!valid) {
        g_pageCallbackAbiValid = false;
        return 0;
    }
    std::wcsncpy(
        g_lastPageControl,
        name,
        31);
    g_lastPageControl[31] = L'\0';
    if (g_pageCallbackPending)
        return 0;

    auto& group =
        *reinterpret_cast<int32_t*>(
            g_unitManager + 0x6C);
    const int32_t maximumGroup =
        *reinterpret_cast<int32_t*>(
            g_unitManager + 0x70);
    auto& page =
        *reinterpret_cast<int32_t*>(
            g_unitManager + 0x74);
    const int32_t maximumPage =
        *reinterpret_cast<int32_t*>(
            g_unitManager + 0x78);
    int32_t nextPage = page;
    if (std::wcscmp(
            name,
            L"LeftBtn") == 0) {
        if (group <= 0)
            return 0;
        --group;
        nextPage = group * 5;
    } else if (std::wcscmp(
                   name,
                   L"RightBtn") == 0) {
        if (group >= maximumGroup)
            return 0;
        ++group;
        nextPage = group * 5;
    } else if (std::wcscmp(
                   name,
                   L"LeftOneBtn") == 0) {
        if (page <= 0)
            return 0;
        nextPage = page - 1;
    } else if (std::wcscmp(
                   name,
                   L"RightOneBtn") == 0) {
        if (page >= maximumPage)
            return 0;
        nextPage = page + 1;
    } else {
        g_pageCallbackAbiValid = false;
        return 0;
    }
    page = nextPage;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x68) = -1;
    RefreshOccupied(
        static_cast<uint32_t>(page));
    return 0;
}

uint8_t __fastcall FakeUnitListMouseBusiness(
    void* self,
    void*,
    uint32_t message,
    int32_t x,
    int32_t y,
    uint32_t,
    uint32_t)
{
    ++g_mouseCalls;
    if (self != g_unitList)
        return 0;
    int hit = -1;
    for (size_t index = 0;
         index < kFixtureSlots;
         ++index) {
        const float nativeLeft =
            *reinterpret_cast<float*>(
                g_rowControls[index] +
                0x148);
        const float nativeTop =
            *reinterpret_cast<float*>(
                g_rowControls[index] +
                0x14C);
        const int32_t left =
            static_cast<int32_t>(
                nativeLeft);
        const int32_t top =
            static_cast<int32_t>(
                nativeTop);
        const int32_t width =
            *reinterpret_cast<int32_t*>(
                g_rowControls[index] +
                0x154);
        const int32_t height =
            *reinterpret_cast<int32_t*>(
                g_rowControls[index] +
                0x158);
        if (x >= left &&
            x < left + width &&
            y >= top &&
            y < top + height) {
            hit =
                static_cast<int>(index);
            break;
        }
    }
    if (message == 0x201) {
        g_selectedIndex = hit;
        return hit >= 0 ? 1 : 0;
    }
    if (message != 0x202 ||
        hit < 0 ||
        hit != g_selectedIndex)
        return 0;
    if (!g_acceptSelection)
        return 1;
    for (auto& row : g_rows)
        row[0x25] = 0;
    g_rows[hit][0x25] = 1;
    return 1;
}

void* __fastcall FakeUnitListItemByIndex(
    void* self,
    void*,
    int index)
{
    ++g_itemLookupCalls;
    if (self != g_unitList ||
        !g_rowsReady ||
        index < 0 ||
        index >=
            static_cast<int>(
                kFixtureSlots))
        return nullptr;
    return g_rows[index];
}

void FireReply(
    uint16_t status,
    uint16_t activeSlot)
{
    uint8_t packet[0x20] = {};
    *reinterpret_cast<uint16_t*>(
        packet) = 0x09B4;
    *reinterpret_cast<uint16_t*>(
        packet + 6) = status;
    *reinterpret_cast<uint16_t*>(
        packet + 0x0C) =
            activeSlot;
    if (g_replyDetour) {
        g_replyDetour(
            g_unitManager,
            nullptr,
            packet);
    }
}

uint32_t __fastcall FakeSelectBusiness(
    void* self,
    void*)
{
    ++g_businessCalls;
    if (self != g_unitManager ||
        !g_sendRequest ||
        !g_networkSendDetour)
        return 0;
    uint8_t packet[14] = {};
    *reinterpret_cast<uint16_t*>(
        packet) = 0x04CF;
    g_networkSendDetour(
        g_network,
        nullptr,
        &g_stream,
        packet,
        sizeof(packet),
        0);
    if (g_syncReply)
        FireReply(0, 4);
    return 0;
}

void __fastcall FakeActiveWaitReply(
    void* self,
    void*,
    const void* packet)
{
    ++g_replyOriginalCalls;
    if (self != g_unitManager ||
        !packet)
        return;
    const auto* bytes =
        static_cast<const uint8_t*>(
            packet);
    const uint16_t status =
        *reinterpret_cast<
            const uint16_t*>(
                bytes + 6);
    const uint16_t slot =
        *reinterpret_cast<
            const uint16_t*>(
                bytes + 0x0C);
    if (!status) {
        *reinterpret_cast<uint16_t*>(
            g_profile + 0x1AD) =
                slot;
    }
}

void* LookupSymbol(const char* name)
{
    if (!name ||
        (g_missingSymbol &&
         std::strcmp(
             name,
             g_missingSymbol) == 0))
        return nullptr;
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(
            &FakeGameApp);
    if (std::strcmp(
            name,
            "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginPacketCtor);
    if (std::strcmp(
            name,
            "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(
            &FakePasswordMd5Hex);
    if (std::strcmp(
            name,
            "flow.network") == 0)
        return reinterpret_cast<void*>(
            &FakeNetwork);
    if (std::strcmp(
            name,
            "flow.network_send") == 0)
        return reinterpret_cast<void*>(
            &FakeNetworkSend);
    if (std::strcmp(
            name,
            "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(
            &FakeLoginDispatch);
    if (std::strcmp(
            name,
            "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(
            name,
            "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringCtor);
    if (std::strcmp(
            name,
            "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNativeWStringDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogCtor);
    if (std::strcmp(
            name,
            "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(
            &FakeEnqueueUiObject);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogDtor);
    if (std::strcmp(
            name,
            "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(
            &FakeNicknameDialogConfirm);
    if (std::strcmp(
            name,
            "flow.unit_info_pointer") == 0)
        return &g_unitInfoObject;
    if (std::strcmp(
            name,
            "flow.unit_brief_by_slot") == 0)
        return reinterpret_cast<void*>(
            &FakeUnitBriefBySlot);
    if (std::strcmp(
            name,
            "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(
            name,
            "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(
            &FakeServiceFindByName);
    if (std::strcmp(
            name,
            "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(
            &FakeUiFindControl);
    if (std::strcmp(
            name,
            "flow.battleship_lobby_manager_vtable") ==
        0)
        return g_unitManagerVtable;
    if (std::strcmp(
            name,
            "flow.battleship_lobby_select_business") ==
        0)
        return reinterpret_cast<void*>(
            &FakeUnitManagerCallback);
    if (std::strcmp(
            name,
            "flow.unit_list_mouse_business") == 0)
        return reinterpret_cast<void*>(
            &FakeUnitListMouseBusiness);
    if (std::strcmp(
            name,
            "flow.unit_list_item_by_index") == 0)
        return reinterpret_cast<void*>(
            &FakeUnitListItemByIndex);
    if (std::strcmp(
            name,
            "flow.unit_active_waitunit_business") == 0)
        return reinterpret_cast<void*>(
            &FakeSelectBusiness);
    if (std::strcmp(
            name,
            "flow.unit_active_waitunit_reply") == 0)
        return reinterpret_cast<void*>(
            &FakeActiveWaitReply);
    return nullptr;
}

void* ApiSymbol(
    SdgoMod*,
    const char* name)
{
    if(!std::strcmp(name,"flow.unit_full_by_slot"))return reinterpret_cast<void*>(&FakeUnitFullBySlot);
    return LookupSymbol(name);
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    void* target = LookupSymbol(name);
    if (!target || !detour || !original)
        return 0;
    if (g_replyHookFails &&
        std::strcmp(
            name,
            "flow.unit_active_waitunit_reply") == 0)
        return 0;
    *original = target;
    if (std::strcmp(
            name,
            "flow.network_send") == 0) {
        g_networkSendDetour =
            reinterpret_cast<
                NetworkSendDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "flow.login_service_dispatch") ==
               0) {
        g_loginDispatchDetour =
            reinterpret_cast<
                LoginDispatchDetour>(
                detour);
    } else if (std::strcmp(
                   name,
                   "flow.unit_active_waitunit_reply") ==
               0) {
        g_replyDetour =
            reinterpret_cast<
                ActiveWaitReplyDetour>(
                detour);
    }
    return 1;
}

void ResetFixture(
    uint16_t activeSlot = 1,
    int capacity = 4)
{
    DestroyNativeString(
        g_unitList + 0x118);
    for (auto& control : g_pageControls)
        DestroyNativeString(
            control + 0x118);
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_unitManager,
        0,
        sizeof(g_unitManager));
    std::memset(
        g_unitList,
        0,
        sizeof(g_unitList));
    std::memset(
        g_pageControls,
        0,
        sizeof(g_pageControls));
    std::memset(
        g_rows,
        0,
        sizeof(g_rows));
    std::memset(
        g_rowControls,
        0,
        sizeof(g_rowControls));
    std::memset(
        g_unitInfo,
        0,
        sizeof(g_unitInfo));
    std::memset(
        g_inventory,
        0,
        sizeof(g_inventory));

    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 5;
    *reinterpret_cast<uint16_t*>(
        g_profile + 0x1AD) =
            activeSlot;
    *reinterpret_cast<void***>(
        g_unitManager) =
            g_returnWrongVtable
                ? g_wrongVtable
                : g_unitManagerVtable;
    *reinterpret_cast<void**>(
        g_unitManager + 0x64) =
            &g_unitRoot;
    const int32_t maximumPage =
        capacity > 0
            ? (capacity - 1) / 6
            : 0;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x6C) = 0;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x70) =
            maximumPage / 5;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x74) = 0;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x78) =
            maximumPage;

    *reinterpret_cast<void**>(
        g_unitList + 0x15C) =
            &g_unitRoot;
    *reinterpret_cast<float*>(
        g_unitList + 0x148) = 37.75f;
    *reinterpret_cast<float*>(
        g_unitList + 0x14C) = 19.5f;
    *reinterpret_cast<int32_t*>(
        g_unitList + 0x220) = -1;
    *reinterpret_cast<int32_t*>(
        g_unitList + 0x224) = -1;
    g_unitList[0x162] = 1;
    g_unitList[0x167] = 1;
    WriteNativeString(
        g_unitList + 0x118,
        L"UnitList");
    for (size_t index = 0;
         index < 4;
         ++index) {
        *reinterpret_cast<void**>(
            g_pageControls[index] +
            0x15C) =
                &g_unitRoot;
        g_pageControls[index][0x162] = 1;
        g_pageControls[index][0x167] = 1;
        WriteNativeString(
            g_pageControls[index] +
                0x118,
            kPageControlNames[index]);
    }
    for (size_t index = 0;
         index < kFixtureSlots;
         ++index) {
        *reinterpret_cast<void**>(
            g_rows[index] + 0x1C) =
                g_rowControls[index];
        *reinterpret_cast<void**>(
            g_rowControls[index] +
            0x15C) =
                g_unitList;
        *reinterpret_cast<float*>(
            g_rowControls[index] +
            0x148) =
                125.75f;
        *reinterpret_cast<float*>(
            g_rowControls[index] +
            0x14C) =
                50.25f +
                static_cast<float>(
                    index) *
                    30.0f;
        *reinterpret_cast<int32_t*>(
            g_rowControls[index] +
            0x154) = 20;
        *reinterpret_cast<int32_t*>(
            g_rowControls[index] +
            0x158) = 20;
    }

    *reinterpret_cast<int32_t*>(
        g_unitInfo + 0x0C) =
            capacity;
    g_unitInfo[0x20] = 1;
    WriteBrief(0, 10003, kTrainingUid, 1);
    if (capacity > 1)
        WriteBrief(1, 22040, kSecondUid, 2);
    if (capacity > 2)
        WriteBrief(2, 22042, kThirdUid, 3);
    if (capacity > 3)
        WriteBrief(3, 22097, kTargetUid, 4);
    if (capacity > 6)
        WriteBrief(6, 24402, kSeventhUid, 7);
    RefreshOccupied(0);

    *reinterpret_cast<void**>(
        g_network + 0x14) =
            &g_stream;
    g_serviceVisible = true;
    g_listVisible = true;
    g_rowsReady = true;
    g_fullReady = true;
    g_acceptSelection = true;
    g_sendRequest = true;
    g_syncReply = false;
    g_descriptorValid = true;
    g_networkPacketValid = true;
    g_pageControlsAvailable = true;
    g_pageCallbackPending = false;
    g_pageCallbackAbiValid = true;
    g_networkResult = 1;
    g_selectedIndex = -1;
    g_mouseCalls = 0;
    g_itemLookupCalls = 0;
    g_businessCalls = 0;
    g_replyOriginalCalls = 0;
    g_serviceLookups = 0;
    g_controlLookups = 0;
    g_pageCallbackCalls = 0;
    g_lastControlKind = UINT32_MAX;
    std::memset(
        g_lastPageControl,
        0,
        sizeof(g_lastPageControl));
}

void TestBindFailures(
    SdgoApi& api,
    NativeActions& native)
{
    g_missingSymbol =
        "flow.unit_list_mouse_business";
    UnitSelectActions missing;
    Check(
        !missing.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "binding fails closed when the native list mouse-business symbol is absent");
    g_missingSymbol = nullptr;

    g_missingSymbol =
        "flow.battleship_lobby_select_business";
    UnitSelectActions missingPageCallback;
    Check(
        !missingPageCallback.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "binding fails closed when the shipped unitmgr page callback is absent");
    g_missingSymbol = nullptr;

    void* const originalCallback =
        g_unitManagerVtable[
            0x38 / sizeof(void*)];
    g_unitManagerVtable[
        0x38 / sizeof(void*)] =
            reinterpret_cast<void*>(
                &FakeSelectBusiness);
    UnitSelectActions mismatchedPageCallback;
    Check(
        !mismatchedPageCallback.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "binding verifies the original stdcall callback at unitmgr vtable plus 0x38");
    g_unitManagerVtable[
        0x38 / sizeof(void*)] =
            originalCallback;

    g_replyHookFails = true;
    UnitSelectActions hookFailure;
    Check(
        !hookFailure.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            &native),
        "binding fails closed when the real reply consumer cannot be observed");
    g_replyHookFails = false;
}

void TestDelayedSuccess(
    UnitSelectActions& actions)
{
    actions.Reset();
    ResetFixture();
    g_rows[0][0x25] = 1;
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kSelecting,
        "native list down/up plus active-wait business starts one real request");
    auto state = actions.State();
    Check(
        state.initial_slot == 1 &&
            state.initial_uid == kTrainingUid &&
            state.target_slot == 4 &&
            state.target_uid == kTargetUid &&
            state.target_unit_id == 22097,
        "inventory resolution preserves the initial and requested native identities");
    Check(
        state.request_opcode == 0x04CF &&
            state.request_size == 14 &&
            state.request_result == 1 &&
            state.select_invocations == 1 &&
            state.native_row_selected == 1 &&
            state.native_click_x == 126 &&
            state.native_click_y == 141 &&
            state.native_root_pointer ==
                PointerBits(&g_unitRoot) &&
            state.native_list_pointer ==
                PointerBits(g_unitList) &&
            state.native_row_pointer ==
                PointerBits(g_rows[3]) &&
            state.native_row_control_pointer ==
                PointerBits(
                    g_rowControls[3]) &&
            state.native_row_getter_return ==
                1 &&
            state.native_row_x_bits ==
                FloatBits(125.75f) &&
            state.native_row_y_bits ==
                FloatBits(140.25f) &&
            state.native_row_x_finite == 1 &&
            state.native_row_y_finite == 1 &&
            state.native_list_pressed_index ==
                -1 &&
            state.native_list_selected_index ==
                -1 &&
            state.unit_manager_page == 0 &&
            state.native_target_occupied == 1 &&
            state.native_row_selected_before ==
                0 &&
            state.native_mouse_down_accepted ==
                1 &&
            state.native_mouse_up_accepted ==
                1 &&
            g_selectedIndex == 3 &&
            g_rows[0][0x25] == 0 &&
            g_rows[3][0x25] == 1 &&
            g_networkPacketValid,
        "request acceptance requires native old-row clearing, the shipped opcode/ABI size, and transport success");
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kWaitingForReply &&
            g_businessCalls == 1,
        "polling cannot resend while the genuine server reply is absent");

    FireReply(0, 4);
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kComplete,
        "completion follows the original successful reply and active UID change");
    state = actions.State();
    Check(
        state.reply_sequence == 1 &&
            state.reply_opcode == 0x09B4 &&
            state.reply_status == 0 &&
            state.reply_packet_slot == 4 &&
            state.reply_current_slot == 4 &&
            state.reply_current_uid ==
                kTargetUid &&
            state.current_uid == kTargetUid &&
            state.target_matches == 1 &&
            g_replyOriginalCalls == 1,
        "reply evidence is captured after the original consumer updates profile state");
}

void TestSynchronousReply(
    UnitSelectActions& actions)
{
    actions.Reset();
    ResetFixture();
    g_syncReply = true;
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kComplete,
        "a synchronous original reply is newer than the pre-call baseline");
    Check(
        actions.State().reply_sequence == 1 &&
            actions.State().current_uid ==
                kTargetUid,
        "synchronous completion retains the reply and resulting native UID");
}

void TestReplyFailures(
    UnitSelectActions& actions)
{
    actions.Reset();
    ResetFixture();
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kSelecting,
        "rejection fixture sends its original request");
    FireReply(37, 4);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kReplyRejected &&
            actions.State().current_uid ==
                kTrainingUid,
        "a nonzero native reply status is rejected without a profile change");

    actions.Reset();
    ResetFixture();
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kSelecting,
        "mismatch fixture sends its original request");
    FireReply(0, 3);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kReplyMismatch &&
            actions.State().current_uid ==
                kThirdUid,
        "a successful reply for another slot cannot satisfy the requested UID");
}

void TestPreRequestBoundaries(
    UnitSelectActions& actions)
{
    actions.Reset();ResetFixture();g_fullReady=false;
    Check(actions.Poll(kTargetUid)==UnitSelectActions::kWaitingForFullUnit &&
        g_mouseCalls==0 && g_businessCalls==0,
        "brief ownership alone cannot select before the native full-page reply");
    g_fullReady=true;
    Check(actions.Poll(kTargetUid)==UnitSelectActions::kSelecting && g_businessCalls==1,
        "the genuine full-record identity releases the pending selection");

    actions.Reset();
    ResetFixture();
    g_rowsReady = false;
    Check(actions.Poll(kTargetUid) == UnitSelectActions::kWaitingForListItems &&
              g_mouseCalls == 0 && g_businessCalls == 0,
          "constructed native list waits for its asynchronous rows without selecting or sending");
    g_rowsReady = true;
    Check(actions.Poll(kTargetUid) == UnitSelectActions::kSelecting &&
              g_businessCalls == 1,
          "the same attempt selects through the original business after rows arrive");

    actions.Reset();
    ResetFixture();
    g_acceptSelection = false;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            g_businessCalls == 0,
        "the business method is not called when native mouse selection leaves the row unselected");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<float*>(
        g_rowControls[3] + 0x148) =
            std::numeric_limits<
                float>::quiet_NaN();
    const uint32_t nanBits =
        FloatBits(
            *reinterpret_cast<float*>(
                g_rowControls[3] +
                0x148));
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            actions.State().
                    native_row_pointer ==
                PointerBits(g_rows[3]) &&
            actions.State().
                    native_row_control_pointer ==
                PointerBits(
                    g_rowControls[3]) &&
            actions.State().
                    native_row_getter_return ==
                1 &&
            actions.State().
                    native_row_x_bits ==
                nanBits &&
            actions.State().
                    native_row_y_bits ==
                FloatBits(140.25f) &&
            actions.State().
                    native_row_x_finite ==
                0 &&
            actions.State().
                    native_row_y_finite ==
                1 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a NaN native row coordinate is rejected before integer conversion or mouse dispatch");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<void**>(
        g_rows[3] + 0x1C) = nullptr;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            actions.State().
                    native_row_pointer ==
                PointerBits(g_rows[3]) &&
            actions.State().
                    native_row_getter_return ==
                1 &&
            actions.State().
                    native_row_control_pointer ==
                0 &&
            actions.State().
                    native_row_x_bits ==
                0 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a missing row control retains the read-only row/getter diagnostics and fails before coordinate reads");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<float*>(
        g_rowControls[3] + 0x14C) =
            std::numeric_limits<
                float>::infinity();
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "an infinite native row coordinate is rejected before mouse dispatch");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<float*>(
        g_rowControls[3] + 0x148) =
            2147483648.0f;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a finite native row coordinate outside the int32 click domain is rejected");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<float*>(
        g_rowControls[3] + 0x14C) =
            -std::numeric_limits<
                float>::max();
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kNativeSelectionRejected &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a finite native row coordinate below the int32 click domain is rejected");

    actions.Reset();
    ResetFixture();
    g_networkResult = 0;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kRequestRejected &&
            actions.State().request_result == 0,
        "a failed original wire send cannot become an accepted selection request");

    actions.Reset();
    ResetFixture(4);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kAlreadySelected &&
            g_businessCalls == 0 && actions.State().target_matches &&
            !actions.State().request_invoked && !actions.State().reply_consumed,
        "an already active UID is reported separately from a proven change");

    actions.Reset();
    ResetFixture();
    Check(
        actions.Poll(999999) ==
                UnitSelectActions::
                    kTargetMissing &&
            g_businessCalls == 0,
        "an unowned configured UID never reaches native selection business");

    actions.Reset();
    ResetFixture();
    WriteBrief(1, 22040, kTargetUid, 2);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kTargetDuplicate &&
            g_businessCalls == 0,
        "duplicate UID inventory evidence fails closed");

    actions.Reset();
    ResetFixture();
    WriteBrief(3, 22097, kTargetUid, 3);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kInventoryInvalid &&
            g_businessCalls == 0,
        "a brief whose embedded slot disagrees with its native slot is malformed");

    actions.Reset();
    ResetFixture(1, 1200);
    WriteBrief(6, 0, 0, 7);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::kSelecting &&
            actions.State().inventory_ready == 1 &&
            actions.State().inventory_capacity == 1200 &&
            actions.State().inventory_bad_slot == 0 &&
            actions.State().inventory_bad_unit_id == 0 &&
            actions.State().inventory_bad_uid == 0 &&
            actions.State().inventory_bad_record_slot == 0 &&
            actions.State().target_slot == 4 &&
            g_businessCalls == 1,
        "the real 1200-slot capacity accepts the target plus native missing and preallocated empty briefs");

    actions.Reset();
    ResetFixture();
    g_unitInfo[0x20] = 2;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kWaitingForInventory &&
            actions.State().inventory_ready == 2 &&
            actions.State().inventory_capacity == 0 &&
            g_businessCalls == 0,
        "a nonzero loading state is not mistaken for the exact native ready value one");

    actions.Reset();
    ResetFixture(1, 1200);
    WriteBrief(4, 0, kSecondUid, 0);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kInventoryInvalid &&
            actions.State().inventory_capacity == 1200 &&
            actions.State().inventory_bad_slot == 5 &&
            actions.State().inventory_bad_unit_id == 0 &&
            actions.State().inventory_bad_uid ==
                kSecondUid &&
            actions.State().inventory_bad_record_slot == 0 &&
            g_businessCalls == 0,
        "an empty-looking record with a nonzero UID remains malformed and publishes its raw identity");

    actions.Reset();
    ResetFixture(1, 1200);
    WriteBrief(6, 0, 0, 8);
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kInventoryInvalid &&
            actions.State().inventory_bad_slot == 7 &&
            actions.State().inventory_bad_unit_id == 0 &&
            actions.State().inventory_bad_uid == 0 &&
            actions.State().inventory_bad_record_slot == 8 &&
            g_businessCalls == 0,
        "a preallocated empty record must retain the exact queried native slot");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<int32_t*>(
        g_unitInfo + 0x0C) =
            static_cast<int32_t>(UINT16_MAX) + 1;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kInventoryInvalid &&
            actions.State().inventory_capacity ==
                static_cast<int32_t>(
                    UINT16_MAX) + 1 &&
            g_businessCalls == 0,
        "capacity above the native uint16 slot domain still fails closed before scanning");

    actions.Reset();
    ResetFixture(1, 7);
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 1 &&
            std::wcscmp(
                g_lastPageControl,
                L"RightOneBtn") == 0 &&
            g_pageCallbackAbiValid &&
            g_pageCallbackCalls == 1 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "an off-page target enters the shipped right-page callback instead of reporting a writable page mismatch");
}

void TestNativePageNavigation(
    UnitSelectActions& actions)
{
    actions.Reset();
    ResetFixture(1, 7);
    g_fullReady = false;
    const auto firstNavigation =
        actions.Poll(kSeventhUid);
    const auto firstNavigationState =
        actions.State();
    Check(
        firstNavigation ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 1 &&
            firstNavigationState.
                    unit_manager_page_raw == 0 &&
            firstNavigationState.
                    unit_manager_max_page_raw == 1 &&
            firstNavigationState.
                    unit_manager_page_group_raw == 0 &&
            firstNavigationState.
                    unit_manager_max_page_group_raw == 0 &&
            firstNavigationState.target_page == 1 &&
            std::wcscmp(
                g_lastPageControl,
                L"RightOneBtn") == 0 &&
            g_pageCallbackAbiValid &&
            g_pageCallbackCalls == 1 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "the original stdcall RightOneBtn callback owns the first six-slot page transition");
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kWaitingForFullUnit &&
            g_pageCallbackCalls == 1 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a callback-owned target page is still incomplete until its matching full record arrives");
    g_fullReady = true;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::kSelecting &&
            g_selectedIndex == 6 &&
            g_businessCalls == 1,
        "the page reply full cache releases the existing native row-selection and active-wait path");

    actions.Reset();
    ResetFixture(7, 7);
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x74) = 1;
    RefreshOccupied(1);
    Check(
        actions.Poll(kSecondUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 0 &&
            std::wcscmp(
                g_lastPageControl,
                L"LeftOneBtn") == 0 &&
            g_pageCallbackCalls == 1 &&
            g_businessCalls == 0,
        "an earlier target page uses the shipped LeftOneBtn direction without selecting early");

    actions.Reset();
    ResetFixture(1, 31);
    WriteBrief(
        30,
        31031,
        kThirtyFirstUid,
        31);
    g_fullReady = false;
    Check(
        actions.Poll(kThirtyFirstUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x6C) == 1 &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 5 &&
            std::wcscmp(
                g_lastPageControl,
                L"RightBtn") == 0 &&
            g_pageCallbackCalls == 1,
        "a target in another five-page group uses RightBtn instead of six one-page requests");
    Check(
        actions.Poll(kThirtyFirstUid) ==
                UnitSelectActions::
                    kWaitingForFullUnit &&
            g_pageCallbackCalls == 1,
        "cross-group navigation also waits for the target page full cache");
    g_fullReady = true;
    Check(
        actions.Poll(kThirtyFirstUid) ==
                UnitSelectActions::kSelecting &&
            g_selectedIndex == 30,
        "cross-group completion selects the original absolute UnitList row");

    actions.Reset();
    ResetFixture(2, 31);
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x6C) = 1;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x74) = 5;
    RefreshOccupied(5);
    Check(
        actions.Poll(kTrainingUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x6C) == 0 &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 0 &&
            std::wcscmp(
                g_lastPageControl,
                L"LeftBtn") == 0,
        "reverse cross-group navigation uses the shipped LeftBtn business");

    actions.Reset();
    ResetFixture(1, 7);
    g_pageCallbackPending = true;
    uint8_t profileBefore[
        sizeof(g_profileStorage)] = {};
    uint8_t inventoryBefore[
        sizeof(g_inventory)] = {};
    uint8_t pageStateBefore[0x14] = {};
    std::memcpy(
        profileBefore,
        g_profileStorage,
        sizeof(profileBefore));
    std::memcpy(
        inventoryBefore,
        g_inventory,
        sizeof(inventoryBefore));
    std::memcpy(
        pageStateBefore,
        g_unitManager + 0x68,
        sizeof(pageStateBefore));
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            g_pageCallbackCalls == 1 &&
            std::memcmp(
                pageStateBefore,
                g_unitManager + 0x68,
                sizeof(pageStateBefore)) == 0 &&
            std::memcmp(
                profileBefore,
                g_profileStorage,
                sizeof(profileBefore)) == 0 &&
            std::memcmp(
                inventoryBefore,
                g_inventory,
                sizeof(inventoryBefore)) == 0 &&
            g_mouseCalls == 0 &&
            g_businessCalls == 0,
        "a pending native page request can reject the callback without any adapter page, profile, or inventory write");
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kNavigatingUnitPage &&
            g_pageCallbackCalls == 2 &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 0,
        "polling waits and retries the real callback while native pending state owns admission");

    actions.Reset();
    ResetFixture(1, 7);
    *reinterpret_cast<void**>(
        g_pageControls[3] +
        0x15C) =
            g_unitManager;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kPageControlMismatch &&
            g_pageCallbackCalls == 0 &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 0,
        "a page button owned by another root is rejected before the callback");

    actions.Reset();
    ResetFixture(1, 7);
    WriteNativeString(
        g_pageControls[3] +
            0x118,
        L"WrongPageBtn");
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kPageControlMismatch &&
            g_pageCallbackCalls == 0,
        "a page control with the wrong native name cannot select a direction");

    actions.Reset();
    ResetFixture(1, 7);
    g_pageControls[3][0x162] = 0;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kPageControlMissing &&
            g_pageCallbackCalls == 0,
        "a hidden native page button remains unavailable");

    actions.Reset();
    ResetFixture(1, 7);
    g_pageControls[3][0x167] = 0;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kPageControlMissing &&
            g_pageCallbackCalls == 0,
        "a disabled native page button remains unavailable");

    actions.Reset();
    ResetFixture(1, 7);
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x78) = 0;
    const auto uninitializedResult =
        actions.Poll(kSeventhUid);
    const auto uninitializedState =
        actions.State();
    Check(
        uninitializedResult ==
                UnitSelectActions::
                    kWaitingForUnitPageState &&
            uninitializedState.
                    unit_manager_page_raw == 0 &&
            uninitializedState.
                    unit_manager_max_page_raw == 0 &&
            uninitializedState.
                    unit_manager_page_group_raw == 0 &&
            uninitializedState.
                    unit_manager_max_page_group_raw == 0 &&
            uninitializedState.target_page == 1 &&
            g_pageCallbackCalls == 0 &&
            *reinterpret_cast<int32_t*>(
                g_unitManager + 0x74) == 0,
        "constructor-default pagination fields wait for the native inventory-derived initialization");

    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x78) = 1;
    g_rowsReady = false;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kWaitingForListItems &&
            g_pageCallbackCalls == 0,
        "matching small-inventory page maxima still wait for the complete native UnitList");

    g_rowsReady = true;
    *reinterpret_cast<int32_t*>(
        g_unitManager + 0x74) = 2;
    Check(
        actions.Poll(kSeventhUid) ==
                UnitSelectActions::
                    kTargetPageMismatch &&
            actions.State().
                    unit_manager_page_raw == 2 &&
            actions.State().
                    unit_manager_max_page_raw == 1 &&
            g_pageCallbackCalls == 0,
        "an initialized native page outside its confirmed maximum remains a hard mismatch");
}

void TestIdentityAndLifecycleBoundaries(
    UnitSelectActions& actions)
{
    actions.Reset();
    ResetFixture();
    g_returnWrongVtable = true;
    *reinterpret_cast<void***>(
        g_unitManager) =
            g_wrongVtable;
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::
                kUnitManagerMismatch,
        "a service named unitmgr with another vtable is rejected");
    g_returnWrongVtable = false;

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<void**>(
        g_unitList + 0x15C) =
            g_unitManager;
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::
                kUnitListMismatch,
        "a UnitList control owned by another native root is rejected");

    actions.Reset();
    ResetFixture();
    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 6;
    Check(
        actions.Poll(kTargetUid) ==
                UnitSelectActions::
                    kWaitingForHangar &&
            g_serviceLookups == 0,
        "ordinary selection waits for native app state 5 before touching unitmgr");

    actions.Reset();
    ResetFixture();
    Check(
        actions.Poll(kTargetUid) ==
            UnitSelectActions::kSelecting,
        "target-change fixture starts one owned-unit request");
    Check(
        actions.Poll(kSecondUid) ==
                UnitSelectActions::
                    kInvalidTarget &&
            g_businessCalls == 1,
        "the configured UID cannot change while a real request is outstanding");
}

void TestResultNames()
{
    Check(
        std::strcmp(
            UnitSelectActions::Name(
                UnitSelectActions::kComplete),
            "complete") == 0 &&
            std::strcmp(
                UnitSelectActions::Name(
                    UnitSelectActions::
                        kReplyMismatch),
                "reply-mismatch") == 0 &&
            std::strcmp(
                UnitSelectActions::Name(
                    UnitSelectActions::
                        kWaitingForUnitPageState),
                "waiting-for-unit-page-state") ==
                0 &&
            std::strcmp(
                UnitSelectActions::Name(
                    static_cast<
                        UnitSelectActions::Result>(
                        0xFFFF)),
                "unknown") == 0,
        "result names remain stable for receipts");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: UnitSelectActions x86 ABI/state regression; no game, server, or database");

    static_assert(
        std::is_standard_layout<
            UnitSelectActions::
                Observation>::value,
        "observation must remain a plain receipt");
    static_assert(
        sizeof(uint64_t) == 8,
        "native UID receipt requires 64-bit storage");

    std::memset(
        g_unitManagerVtable,
        0,
        sizeof(g_unitManagerVtable));
    std::memset(
        g_wrongVtable,
        0,
        sizeof(g_wrongVtable));
    g_unitManagerVtable[
        0x38 / sizeof(void*)] =
            reinterpret_cast<void*>(
                &FakeUnitManagerCallback);
    ResetFixture();

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title =
        "offline-unit-select";
    config.room_mode = 6;
    config.map_id = 0;

    NativeActions native;
    const bool nativeBound =
        native.Bind(
            &api,
            reinterpret_cast<SdgoMod*>(1),
            config);
    Check(
        nativeBound &&
            g_networkSendDetour &&
            g_loginDispatchDetour,
        "NativeActions transport observer bound to offline stubs");

    if (nativeBound) {
        TestBindFailures(api, native);

        UnitSelectActions actions;
        const bool actionsBound =
            actions.Bind(
                &api,
                reinterpret_cast<
                    SdgoMod*>(1),
                &native);
        Check(
            actionsBound &&
                g_replyDetour,
            "unit selection bound the original 0x09B4 reply consumer");
        if (actionsBound) {
            TestDelayedSuccess(actions);
            TestSynchronousReply(actions);
            TestReplyFailures(actions);
            TestPreRequestBoundaries(actions);
            TestNativePageNavigation(actions);
            TestIdentityAndLifecycleBoundaries(
                actions);
            TestResultNames();
        }
    }

    DestroyNativeString(
        g_unitList + 0x118);
    for (auto& control : g_pageControls)
        DestroyNativeString(
            control + 0x118);
    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE UNIT SELECT FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts(
        "OFFLINE UNIT SELECT PASS");
    return 0;
}
