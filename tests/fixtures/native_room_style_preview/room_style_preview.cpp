#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"
#include "room_style_preview_core.h"
#if ROOM_STYLE_PREVIEW_WITH_OVERLAY
#include "room_style_preview_overlay.h"
#endif
#include "room_style_preview_status.h"

namespace {

using namespace room_style_preview;
using QueueTransitionFn = void(__thiscall*)(uint8_t*, uint32_t, void*);

constexpr uint32_t kSnapshotVersion = 1;
constexpr uint32_t kCurrentAppOffset = 0x4a0;
constexpr uint32_t kProfileRoomIdOffset = 0x784;
constexpr uint32_t kProfileMapIdOffset = 0x764;
constexpr uint32_t kProfileContextOffset = 0x43dc;
constexpr uint32_t kServiceListHeadOffset = 0x44;
constexpr uint32_t kServiceNodeNextOffset = 0;
constexpr uint32_t kServiceNodeObjectOffset = 8;
constexpr uint32_t kMaxServiceNodes = 512;
constexpr uint32_t kApp7RoomIdOffset = 0x3d80;
constexpr uint32_t kApp7MapIdOffset = 0x3dac;
constexpr uint32_t kApp16RoomIdOffset = 0x223c;
constexpr uint32_t kApp16MapIdOffset = 0x6434;

/*
 * Return RVAs immediately after verified native calls to 0048D680.
 * The factory byte remains the authoritative normal/Mission discriminator.
 * 8DB4DF is the ordinary normal room-list path; 7C16C2/7C173B are the
 * ordinary Battle-domain paths; 9F2B3D/9F2BCD are Mission paths.
 */
constexpr uint32_t kAllowedReturnRvas[] = {
    0x003c16c2, 0x003c173b,
    0x004db464, 0x004db4df, 0x004db572,
    0x005f2b3d, 0x005f2bcd
};

constexpr uint32_t kBattleNormalReturnRva = 0x003c173b;
constexpr uint32_t kListNormalReturnRva = 0x004db4df;
constexpr uint32_t kMissionReturnRva = 0x005f2b3d;

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
QueueTransitionFn g_original = nullptr;
uint8_t* g_host_base = nullptr;
void** g_profile_pointer = nullptr;
void** g_service_manager_pointer = nullptr;
void* g_app7_manager_vtable = nullptr;
void* g_app16_manager_vtable = nullptr;
uint32_t* g_battle_normal_room_id = nullptr;
uint32_t* g_list_normal_room_id = nullptr;
uint32_t* g_mission_room_id = nullptr;
Kind g_kind = Kind::Normal;
bool g_preference_2d = false;

volatile LONG g_claimed = 0;
volatile LONG g_transitions = 0;
volatile LONG g_last_decision = ROOM_STYLE_PREVIEW_UNOBSERVED;
volatile LONG g_reject_reason = ROOM_STYLE_PREVIEW_REJECT_NONE;
volatile LONG g_requested_target = 0;
volatile LONG g_expected_refresh = 0;
volatile LONG g_room_id = 0;
volatile LONG g_map_id = 0;
volatile LONG g_factory_type = 0;
volatile LONG g_source_app = 0;
volatile LONG g_caller_rva = 0;
volatile LONG g_payload_address = 0;
volatile LONG g_profile_room_id = 0;
volatile LONG g_profile_map_id = 0;
volatile LONG g_tail_crc32 = 0;
volatile LONG g_authoritative_room_id = 0;
volatile LONG g_original_source_app = 0;
volatile LONG g_source_matches = 0;
volatile LONG g_native_shape_matches = 0;
volatile LONG g_authority_matches = 0;
volatile PVOID g_game_app = nullptr;

bool IsAccessible(const void* pointer, size_t size, bool writable) {
    if (!pointer || size == 0)
        return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (start + size < start)
        return false;
    uintptr_t cursor = start;
    const uintptr_t end = start + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const DWORD protection = mbi.Protect & 0xffu;
        const bool readable =
            protection == PAGE_READONLY || protection == PAGE_READWRITE ||
            protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const bool canWrite =
            protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        if (!readable || (writable && !canWrite))
            return false;
        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = (std::min)(regionEnd, end);
    }
    return true;
}

template<class T>
bool SafeRead(const void* pointer, T* out) {
    if (!out || !IsAccessible(pointer, sizeof(T), false))
        return false;
    __try {
        std::memcpy(out, pointer, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWriteSource(void* pointer, uint16_t value) {
    if (!IsAccessible(pointer, sizeof(value), true))
        return false;
    __try {
        *static_cast<volatile uint16_t*>(pointer) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t ReturnRva(void* returnAddress) {
    if (!g_host_base || !returnAddress)
        return 0xffffffffu;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_host_base);
    const uintptr_t address = reinterpret_cast<uintptr_t>(returnAddress);
    if (address < base || address - base > 0xffffffffu)
        return 0xffffffffu;
    return static_cast<uint32_t>(address - base);
}

bool IsAllowedCaller(uint32_t rva) {
    for (uint32_t allowed : kAllowedReturnRvas) {
        if (rva == allowed)
            return true;
    }
    return false;
}

bool ReadProfile(uint32_t* roomId, uint32_t* mapId) {
    if (!roomId || !mapId || !g_profile_pointer)
        return false;
    void* profile = nullptr;
    if (!SafeRead(g_profile_pointer, &profile) || !profile)
        return false;
    const auto* bytes = static_cast<const uint8_t*>(profile);
    return SafeRead(bytes + kProfileRoomIdOffset, roomId) &&
        SafeRead(bytes + kProfileMapIdOffset, mapId);
}

struct NativeIdentityResult {
    uint32_t authoritative_room_id;
    uint32_t current_app;
    uint32_t profile_context;
    bool profile_context_read;
    bool source_matches;
    bool native_shape_matches;
    bool authority_matches;
};

NativeIdentityResult ValidateNativeIdentity(
    uint8_t* gameApp,
    uint32_t callerRva,
    const NativeTransitionPayload24& payload) {
    NativeIdentityResult result = {};
    if (!gameApp)
        return result;
    SafeRead(gameApp + kCurrentAppOffset, &result.current_app);
    result.source_matches =
        payload.source_app == static_cast<uint16_t>(result.current_app);

    void* profile = nullptr;
    if (g_profile_pointer &&
        SafeRead(g_profile_pointer, &profile) && profile &&
        !SafeRead(static_cast<uint8_t*>(profile) + kProfileContextOffset,
                  &result.profile_context)) {
        /* Keep profile_context_read false. */
    } else if (profile) {
        result.profile_context_read = true;
        result.native_shape_matches =
            NativeShapeMatches(payload, result.profile_context);
    }

    uint32_t* authoritativeRoomPointer = nullptr;
    if (callerRva == kBattleNormalReturnRva &&
        payload.factory_type == kNormalFactory)
        authoritativeRoomPointer = g_battle_normal_room_id;
    else if (callerRva == kListNormalReturnRva &&
             payload.factory_type == kNormalFactory)
        authoritativeRoomPointer = g_list_normal_room_id;
    else if (callerRva == kMissionReturnRva &&
             payload.factory_type == kMissionFactory)
        authoritativeRoomPointer = g_mission_room_id;
    else
        return result;

    uint32_t authoritativeRoom = 0;
    if (!SafeRead(authoritativeRoomPointer, &authoritativeRoom))
        return result;
    result.authoritative_room_id = authoritativeRoom;
    result.authority_matches =
        authoritativeRoom != 0 && authoritativeRoom == payload.room_id;
    return result;
}

void RecordNativeIdentity(
    const NativeTransitionPayload24& payload,
    const NativeIdentityResult& identity) {
    if (InterlockedCompareExchange(&g_transitions, 0, 0) != 0)
        return;
    InterlockedExchange(
        &g_authoritative_room_id,
        static_cast<LONG>(identity.authoritative_room_id));
    InterlockedExchange(&g_original_source_app, payload.source_app);
    InterlockedExchange(&g_source_matches, identity.source_matches ? 1 : 0);
    InterlockedExchange(
        &g_native_shape_matches,
        identity.native_shape_matches ? 1 : 0);
    InterlockedExchange(
        &g_authority_matches,
        identity.authority_matches ? 1 : 0);
}

void RecordPassthrough(const NativeTransitionPayload24* payload,
                       uint32_t callerRva,
                       uint32_t profileRoom,
                       uint32_t profileMap,
                       uint32_t reason) {
    /*
     * Once a preview transition has been accepted, its immutable receipt is
     * the snapshot contract. Later unrelated native transitions must not
     * replace the room/map/source that readiness is validating.
     */
    if (InterlockedCompareExchange(&g_transitions, 0, 0) != 0)
        return;
    if (payload) {
        InterlockedExchange(&g_room_id, static_cast<LONG>(payload->room_id));
        InterlockedExchange(&g_map_id, static_cast<LONG>(payload->map_id));
        InterlockedExchange(&g_factory_type, payload->factory_type);
        InterlockedExchange(&g_source_app, payload->source_app);
        InterlockedExchange(
            &g_tail_crc32,
            static_cast<LONG>(Crc32(&payload->context_43dc, 12)));
    }
    InterlockedExchange(&g_caller_rva, static_cast<LONG>(callerRva));
    InterlockedExchange(&g_profile_room_id, static_cast<LONG>(profileRoom));
    InterlockedExchange(&g_profile_map_id, static_cast<LONG>(profileMap));
    InterlockedExchange(&g_reject_reason, static_cast<LONG>(reason));
    InterlockedExchange(&g_last_decision, ROOM_STYLE_PREVIEW_PASSTHROUGH);
}

void __fastcall QueueTransition(uint8_t* self, void*, uint32_t service, void* payloadPointer) {
    const DWORD savedError = GetLastError();
    const uint32_t callerRva = ReturnRva(_ReturnAddress());
    InterlockedExchangePointer(&g_game_app, self);
    InterlockedExchange(
        &g_payload_address,
        static_cast<LONG>(reinterpret_cast<uintptr_t>(payloadPointer)));

    NativeTransitionPayload24 payload = {};
    uint32_t profileRoom = 0;
    uint32_t profileMap = 0;
    const bool payloadReadable =
        IsAccessible(payloadPointer, sizeof(payload), true) &&
        SafeRead(payloadPointer, &payload);
    ReadProfile(&profileRoom, &profileMap);

    if (!payloadReadable) {
        RecordPassthrough(nullptr, callerRva, profileRoom, profileMap,
                          ROOM_STYLE_PREVIEW_REJECT_UNREADABLE_PAYLOAD);
        SetLastError(savedError);
        g_original(self, service, payloadPointer);
        return;
    }

    PlanInput input = {};
    const NativeIdentityResult identity =
        ValidateNativeIdentity(self, callerRva, payload);
    RecordNativeIdentity(payload, identity);

    input.preference_2d = g_preference_2d;
    input.caller_allowed = IsAllowedCaller(callerRva);
    input.native_identity_valid =
        identity.source_matches &&
        identity.native_shape_matches &&
        identity.authority_matches;
    input.already_used = InterlockedCompareExchange(&g_claimed, 0, 0) != 0;
    input.kind = g_kind;
    input.service = service;
    input.payload = payload;
    const Plan plan = MakePlan(input);
    if (!plan.rewrite) {
        const uint8_t expectedFactory =
            g_kind == Kind::Normal ? kNormalFactory : kMissionFactory;
        if (service == kApp3dWaitRoom &&
            IsAllowedCaller(callerRva) &&
            payload.factory_type == expectedFactory &&
            plan.reject_reason == ROOM_STYLE_PREVIEW_REJECT_NATIVE_IDENTITY_MISMATCH) {
            g_api->Log(
                g_mod, SDGO_LOG_WARN,
                "room style preview reject identity: caller=%08X room=%u authority=%u "
                "source=%u current=%u context=%08X profile_context=%08X context_read=%u "
                "pad02=%04X pad11=%02X%02X%02X aux=%08X checks=%u/%u/%u",
                callerRva, payload.room_id, identity.authoritative_room_id,
                payload.source_app, identity.current_app,
                payload.context_43dc, identity.profile_context,
                identity.profile_context_read ? 1u : 0u,
                payload.reserved_02,
                payload.reserved_11[0], payload.reserved_11[1],
                payload.reserved_11[2], payload.auxiliary,
                identity.source_matches ? 1u : 0u,
                identity.native_shape_matches ? 1u : 0u,
                identity.authority_matches ? 1u : 0u);
        }
        RecordPassthrough(&payload, callerRva, profileRoom, profileMap,
                          plan.reject_reason);
        SetLastError(savedError);
        g_original(self, service, payloadPointer);
        return;
    }
    if (InterlockedCompareExchange(&g_claimed, 1, 0) != 0) {
        RecordPassthrough(&payload, callerRva, profileRoom, profileMap,
                          ROOM_STYLE_PREVIEW_REJECT_ALREADY_USED);
        SetLastError(savedError);
        g_original(self, service, payloadPointer);
        return;
    }
    if (!SafeWriteSource(payloadPointer, plan.source_tag)) {
        InterlockedExchange(&g_claimed, 0);
        RecordPassthrough(&payload, callerRva, profileRoom, profileMap,
                          ROOM_STYLE_PREVIEW_REJECT_WRITE_FAILED);
        SetLastError(savedError);
        g_original(self, service, payloadPointer);
        return;
    }

    InterlockedExchange(&g_requested_target, static_cast<LONG>(plan.target_service));
    InterlockedExchange(&g_expected_refresh, static_cast<LONG>(plan.refresh_opcode));
    InterlockedExchange(&g_room_id, static_cast<LONG>(payload.room_id));
    InterlockedExchange(&g_map_id, static_cast<LONG>(payload.map_id));
    InterlockedExchange(&g_factory_type, payload.factory_type);
    InterlockedExchange(&g_source_app, plan.source_tag);
    InterlockedExchange(&g_caller_rva, static_cast<LONG>(callerRva));
    InterlockedExchange(&g_profile_room_id, static_cast<LONG>(profileRoom));
    InterlockedExchange(&g_profile_map_id, static_cast<LONG>(profileMap));
    InterlockedExchange(
        &g_tail_crc32,
        static_cast<LONG>(Crc32(&payload.context_43dc, 12)));
    InterlockedExchange(&g_reject_reason, ROOM_STYLE_PREVIEW_REJECT_NONE);
    InterlockedExchange(&g_last_decision, ROOM_STYLE_PREVIEW_REWRITTEN);
    InterlockedIncrement(&g_transitions);

    g_api->Log(
        g_mod, SDGO_LOG_INFO,
        "room style preview: APP18 -> APP%u room=%u map=%u caller_rva=%08X payload=%p",
        plan.target_service, payload.room_id, payload.map_id, callerRva, payloadPointer);
    SetLastError(savedError);
    g_original(self, plan.target_service, payloadPointer);
}

void* ResolveUniqueManager(void* expectedVtable) {
    if (!expectedVtable || !g_service_manager_pointer)
        return nullptr;
    void* manager = nullptr;
    if (!SafeRead(g_service_manager_pointer, &manager) || !manager)
        return nullptr;
    uint8_t* head = nullptr;
    if (!SafeRead(static_cast<uint8_t*>(manager) + kServiceListHeadOffset, &head) || !head)
        return nullptr;
    uint8_t* node = nullptr;
    if (!SafeRead(head + kServiceNodeNextOffset, &node))
        return nullptr;
    void* matched = nullptr;
    uint32_t visited = 0;
    while (node && node != head && visited++ < kMaxServiceNodes) {
        void* service = nullptr;
        void* vtable = nullptr;
        uint8_t* next = nullptr;
        if (!SafeRead(node + kServiceNodeObjectOffset, &service) ||
            !SafeRead(node + kServiceNodeNextOffset, &next))
            return nullptr;
        if (service && SafeRead(service, &vtable) && vtable == expectedVtable) {
            if (matched && matched != service)
                return nullptr;
            matched = service;
        }
        node = next;
    }
    if (!node || visited >= kMaxServiceNodes)
        return nullptr;
    return matched;
}

void FillReadiness(RoomStylePreviewSnapshotV1* snapshot) {
    uint8_t* gameApp =
        static_cast<uint8_t*>(InterlockedCompareExchangePointer(&g_game_app, nullptr, nullptr));
    uint32_t actualApp = 0;
    if (gameApp)
        SafeRead(gameApp + kCurrentAppOffset, &actualApp);
    snapshot->actual_app = actualApp;

    void* expectedVtable = nullptr;
    uint32_t roomOffset = 0;
    uint32_t mapOffset = 0;
    if (snapshot->requested_target == kApp2dNormal) {
        expectedVtable = g_app7_manager_vtable;
        roomOffset = kApp7RoomIdOffset;
        mapOffset = kApp7MapIdOffset;
    } else if (snapshot->requested_target == kApp2dMission) {
        expectedVtable = g_app16_manager_vtable;
        roomOffset = kApp16RoomIdOffset;
        mapOffset = kApp16MapIdOffset;
    }
    uint8_t* manager =
        static_cast<uint8_t*>(ResolveUniqueManager(expectedVtable));
    snapshot->manager_found = manager ? 1u : 0u;
    if (manager) {
        uint32_t managerRoom = 0;
        uint32_t managerMap = 0;
        if (SafeRead(manager + roomOffset, &managerRoom) &&
            SafeRead(manager + mapOffset, &managerMap) &&
            managerRoom == snapshot->room_id &&
            managerMap == snapshot->map_id)
            snapshot->manager_ids_match = 1;
    }
    snapshot->ready =
        snapshot->requested_target != 0 &&
        snapshot->actual_app == snapshot->requested_target &&
        snapshot->manager_found && snapshot->manager_ids_match;
}

bool EqualAsciiNoCase(const char* lhs, const char* rhs) {
    if (!lhs || !rhs)
        return false;
    while (*lhs && *rhs) {
        char a = *lhs++;
        char b = *rhs++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return *lhs == '\0' && *rhs == '\0';
}

} // namespace

extern "C" __declspec(dllexport)
int __cdecl SdgoRoomStylePreviewGetSnapshotV1(
    RoomStylePreviewSnapshotV1* out,
    uint32_t outSize) {
    if (!out || outSize < sizeof(RoomStylePreviewSnapshotV1))
        return 0;
    RoomStylePreviewSnapshotV1 snapshot = {};
    snapshot.size = sizeof(snapshot);
    snapshot.version = kSnapshotVersion;
    snapshot.requested_target =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_requested_target, 0, 0));
    snapshot.room_id =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_room_id, 0, 0));
    snapshot.map_id =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_map_id, 0, 0));
    snapshot.transitions =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_transitions, 0, 0));
    snapshot.refresh_count = 0;
    snapshot.expected_refresh_opcode =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_expected_refresh, 0, 0));
    snapshot.last_decision =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_last_decision, 0, 0));
    snapshot.reject_reason =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_reject_reason, 0, 0));
    snapshot.factory_type =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_factory_type, 0, 0));
    snapshot.source_app =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_source_app, 0, 0));
    snapshot.caller_rva =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_caller_rva, 0, 0));
    snapshot.payload_address =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_payload_address, 0, 0));
    snapshot.profile_room_id =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_profile_room_id, 0, 0));
    snapshot.profile_map_id =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_profile_map_id, 0, 0));
    snapshot.tail_crc32 =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_tail_crc32, 0, 0));
    snapshot.reserved[0] =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_authoritative_room_id, 0, 0));
    snapshot.reserved[1] =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_original_source_app, 0, 0));
    snapshot.reserved[2] =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_source_matches, 0, 0));
    snapshot.reserved[3] =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_native_shape_matches, 0, 0));
    snapshot.reserved[4] =
        static_cast<uint32_t>(InterlockedCompareExchange(
            &g_authority_matches, 0, 0));
    FillReadiness(&snapshot);
    std::memcpy(out, &snapshot, sizeof(snapshot));
    return 1;
}

SDGO_MOD_ENTRY {
    static_assert(sizeof(RoomStylePreviewSnapshotV1) == 104, "snapshot ABI");
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, SettingString) + sizeof(api->SettingString) ||
        !api->Symbol || !api->HookCode || !api->SettingString ||
        !api->ModDirectory || !api->Log)
        return 0;
    g_api = api;
    g_mod = mod;
    g_host_base = static_cast<uint8_t*>(api->host_base);

    char preference[16] = {};
    char kind[16] = {};
    api->SettingString(mod, "Preference", "3d", preference, sizeof(preference));
    api->SettingString(mod, "PreviewKind", "normal", kind, sizeof(kind));
    g_preference_2d = EqualAsciiNoCase(preference, "2d");
    if (EqualAsciiNoCase(kind, "normal"))
        g_kind = Kind::Normal;
    else if (EqualAsciiNoCase(kind, "mission"))
        g_kind = Kind::Mission;
    else {
        api->Log(mod, SDGO_LOG_ERROR, "room style preview: PreviewKind must be normal or mission");
        return 0;
    }

    g_profile_pointer =
        static_cast<void**>(api->Symbol(mod, "preview.profile_pointer"));
    g_service_manager_pointer =
        static_cast<void**>(api->Symbol(mod, "preview.service_managed_pointer"));
    g_app7_manager_vtable =
        api->Symbol(mod, "preview.app7_manager_vtable");
    g_app16_manager_vtable =
        api->Symbol(mod, "preview.app16_manager_vtable");
    g_battle_normal_room_id = static_cast<uint32_t*>(
        api->Symbol(mod, "preview.battle_normal_room_id"));
    g_list_normal_room_id = static_cast<uint32_t*>(
        api->Symbol(mod, "preview.list_normal_room_id"));
    g_mission_room_id = static_cast<uint32_t*>(
        api->Symbol(mod, "preview.mission_room_id"));
    if (!g_profile_pointer || !g_service_manager_pointer ||
        !g_app7_manager_vtable || !g_app16_manager_vtable ||
        !g_battle_normal_room_id || !g_list_normal_room_id ||
        !g_mission_room_id)
        return 0;

    if (!g_preference_2d) {
        InterlockedExchange(&g_reject_reason, ROOM_STYLE_PREVIEW_REJECT_DISABLED);
        api->Log(mod, SDGO_LOG_INFO, "room style preview: Preference=3d, native APP18 path retained");
        return 1;
    }
#if ROOM_STYLE_PREVIEW_WITH_OVERLAY
    if (!room_style_preview_overlay::Initialize(api, mod))
        return 0;
#endif
    if (!api->HookCode(
            mod, "preview.queue_transition",
            reinterpret_cast<void*>(&QueueTransition),
            reinterpret_cast<void**>(&g_original)))
        return 0;
    api->Log(
        mod, SDGO_LOG_INFO,
        "room style preview active: one-shot kind=%s, exact APP18/profile match required",
        g_kind == Kind::Normal ? "normal" : "mission");
    return 1;
}
