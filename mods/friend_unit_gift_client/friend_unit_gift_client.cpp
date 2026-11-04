#include <windows.h>

#include "client_model.h"
#include "client_runtime_policy.h"
#include "picker_paging.h"
#include "native_ui.h"
#include "card_layout.h"
#include "native_inventory.h"
#include "native_reply_queue.h"
#include "transport.h"

#include "sdgo/friend_unit_gift_client_observer.h"
#include "sdgo/game_test_page_observer.h"
#include "sdgo/storage_extension.h"
#include "sdgo/localization.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace sdgo::friend_unit_gift;
using namespace sdgo::friend_unit_gift::client;

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(
        &value,
        static_cast<const std::uint8_t*>(pointer) + offset,
        sizeof(value));
    return value;
}

template <class T>
void Put(void* pointer, std::size_t offset, T value) {
    std::memcpy(
        static_cast<std::uint8_t*>(pointer) + offset,
        &value,
        sizeof(value));
}

template <class T>
bool Bind(T& output, const char* name);

using NativeSession = std::uint8_t*(__cdecl*)();
using NativeApp = std::uint8_t*(__cdecl*)();
using FriendInit = void(__thiscall*)(void*, std::uint32_t);
using FriendCallback =
    void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
using UnitDetailInit = runtime::UnitDetailInit;
using UnitDetailEvent =
    void(__stdcall*)(void*, std::uint32_t, void*, std::uintptr_t);
using UnitDetailRefresh = void(__thiscall*)(void*);
using StorageInit = void(__thiscall*)(void*);
using StorageEvent = std::uint32_t(__thiscall*)(
    void*, void*, std::uint32_t, std::uintptr_t, void*);
using StoragePacket = std::uint32_t(__thiscall*)(
    void*, std::uintptr_t, std::uintptr_t, std::uintptr_t,
    std::uint32_t, const std::uint8_t*);
using ComposerUpdate = void(__thiscall*)(void*);
using ComposerCallback =
    void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
using NativeAlloc = void*(__cdecl*)(std::size_t);
using NativeFree = void(__cdecl*)(void*);
using NativeEnqueue = void*(__cdecl*)(void*, std::uint8_t);
using NativeClose = void(__cdecl*)(void*, std::uint8_t);
using NativeStep = void(__thiscall*)(void*);
using NativeComboClear = void(__thiscall*)(void*);
using NativeItemCtor = void*(__thiscall*)(void*, void*);
using NativeComboAdd = std::uint32_t(__thiscall*)(
    void*, const ui::NativeString*, void*);
using NativeEditGet = ui::NativeString*(__thiscall*)(void*);
using NativeEditSet = void(__thiscall*)(
    void*, const ui::NativeString*, int);
using NativeGetUnit = void*(__thiscall*)(
    void*, void*, std::uint16_t);
using GridData = void*(__cdecl*)();
using GridFind = std::uint8_t(__thiscall*)(
    void*, const std::uint8_t**, std::uint32_t, int, char);
using ServiceFindByName = void*(__thiscall*)(void*, const void*);
using NativeWStringCtor = void*(__thiscall*)(void*, const wchar_t*);
using NativeWStringDtor = void(__thiscall*)(void*);
using NativeBusiness = void(__thiscall*)(void*);
using TreeFindByData = void*(__thiscall*)(void*, void*);
using TreeItemData = void*(__thiscall*)(void*);
using TreeSelect = void(__thiscall*)(void*, void*);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Transport g_transport;
Model g_model;
NativeInventorySync g_inventory_sync;
NativeReplyQueue g_native_replies;
NativeUnitDetailQueue g_native_details;
using SyncFull = void(__thiscall*)(void*,const void*);
SyncFull g_sync_full = nullptr;
using UnitCardCtor = void*(__thiscall*)(void*,const void*,std::uint32_t);
using UnitCardDtor = void(__thiscall*)(void*);
UnitCardCtor g_unit_card_ctor=nullptr;
UnitCardDtor g_unit_card_dtor=nullptr;
std::uint64_t g_unit_card_requested=0;
std::uint64_t g_provenance_target_uid=0;
std::uint64_t g_provenance_target_session=0;
std::uint64_t g_provenance_target_deadline=0;
std::uint64_t g_provenance_next_action=0;
using NativeAddControl = std::uint32_t(__thiscall*)(void*,const ui::NativeString*,int,int,int,int);
NativeAddControl g_add_combo=nullptr;
NativeAddControl g_add_edit=nullptr;
using NativeSetFont=void(__thiscall*)(void*,std::uint32_t);
NativeSetFont g_set_font=nullptr;
bool g_picker_selecting=false;
bool g_picker_seen=false;
bool g_picker_navigation_sent=false;
std::uint64_t g_picker_target_uid=0;
std::uint64_t g_picker_next_action=0;
using ManagementCallback=std::uint32_t(__stdcall*)(void*,std::uint32_t,std::uintptr_t,void*);
ManagementCallback g_management_callback=nullptr;
FriendCallback g_downmenu_callback=nullptr;
FriendCallback g_operator_callback=nullptr;
void* g_downmenu_vtable=nullptr;
void* g_operator_vtable=nullptr;
void* g_management_pager_vtable=nullptr;
using ManagementPageChange=void(__thiscall*)(void*,int,std::uint8_t);
ManagementPageChange g_management_page_change=nullptr;
ManagementCallback g_management_pager_callback=nullptr;
void* g_composer_note=nullptr;
void* g_composer_primary=nullptr;
void* g_composer_cancel=nullptr;
void* g_composer_notice=nullptr;
bool g_composer_opening=false;
bool g_composer_controls=false;
std::uint64_t g_composer_card_uid=0;
bool g_composer_received=false;
Detail g_received_detail{};
std::uint64_t g_composer_reload_uid=0;
std::array<std::uint16_t,kMaximumNoteCodeUnits> g_composer_saved_note{};
std::uint16_t g_composer_saved_note_size=0;
std::atomic<unsigned> g_native_detail_receipts{0};
void* g_receive_decoded_original = nullptr;
const DWORD g_receive_default_mxcsr = 0x1F80;
volatile LONG g_receive_carrier_observations = 0;
NativeInventorySync::Delete g_inventory_delete = nullptr;
NativeBusiness g_inventory_view = nullptr;
NativeBusiness g_inventory_redraw = nullptr;
NativeBusiness g_inventory_page = nullptr;
NativeBusiness g_storage_labels=nullptr;
NativeBusiness g_storage_render=nullptr;
void* g_inventory_service_vtable = nullptr;
ui::Bindings g_ui;
ui::FriendEntry g_friend_main;
ui::FriendEntry g_friend_three_d;
ui::FriendEntry g_friend_community;
bool g_community_friend = false;
ui::StorageView g_storage_view;
ui::UnitDetailView g_unit_detail_view;
NativeSession g_session = nullptr;
NativeApp g_app = nullptr;
FriendInit g_friend_main_init = nullptr;
FriendInit g_friend_three_d_init = nullptr;
FriendInit g_friend_community_init = nullptr;
using CommunityDtor=void*(__thiscall*)(void*,std::uint32_t);
CommunityDtor g_friend_community_dtor = nullptr;
using CommunityCallback=std::uint32_t(__stdcall*)(void*,std::uint32_t,std::uintptr_t,void*);
CommunityCallback g_friend_community_callback = nullptr;
FriendCallback g_home_upmenu_callback = nullptr;
void* g_home_upmenu_vtable = nullptr;
void* g_home_community_vtable = nullptr;
std::uint32_t g_home_context_target = 0;
bool g_home_context_opened = false;
FriendCallback g_friend_callback = nullptr;
UnitDetailInit g_unit_detail_init = nullptr;
UnitDetailEvent g_unit_detail_event = nullptr;
UnitDetailRefresh g_unit_detail_refresh = nullptr;
void* g_unit_detail_service_vtable = nullptr;
StorageInit g_storage_init = nullptr;
StorageEvent g_storage_event = nullptr;
StoragePacket g_storage_packet = nullptr;
ComposerUpdate g_composer_update = nullptr;
ComposerCallback g_composer_callback = nullptr;
NativeAlloc g_alloc = nullptr;
NativeFree g_free = nullptr;
NativeEnqueue g_enqueue = nullptr;
NativeClose g_close = nullptr;
NativeStep g_step = nullptr;
NativeComboClear g_combo_clear = nullptr;
NativeItemCtor g_item_ctor = nullptr;
NativeComboAdd g_combo_add = nullptr;
NativeEditGet g_edit_get = nullptr;
NativeEditSet g_edit_set = nullptr;
NativeGetUnit g_brief = nullptr;
NativeGetUnit g_full = nullptr;
GridData g_grid_data = nullptr;
GridFind g_grid_find = nullptr;
std::uint8_t** g_unit_info = nullptr;
std::uint8_t** g_profile = nullptr;
void** g_service_manager = nullptr;
ServiceFindByName g_find_service = nullptr;
NativeWStringCtor g_wstring_ctor = nullptr;
NativeWStringDtor g_wstring_dtor = nullptr;
std::uint8_t** g_player_data = nullptr;
void* g_messenger_vtable = nullptr;
void* g_messenger_service_vtable = nullptr;
NativeBusiness g_open_messenger = nullptr;
NativeBusiness g_open_friend_context = nullptr;
TreeFindByData g_tree_find = nullptr;
TreeItemData g_tree_data = nullptr;
TreeSelect g_tree_select = nullptr;
const SdgoStorageExtensionApiV1* g_storage_api = nullptr;
UINT_PTR g_timer = 0;
DWORD g_ui_thread = 0;
void* g_composer = nullptr;
std::uint32_t g_receiver_id = 0;
std::int32_t g_friend_context_x = -1;
std::int32_t g_friend_context_y = -1;
std::wstring g_receiver_name;
std::atomic<std::uint64_t> g_request_sequence{1};
bool g_storage_broker = false;
bool g_storage_fallback = false;
bool g_native_ready = false;
bool g_game_thread_ready = false;
bool g_tick_active = false;
std::vector<UnitSnapshot> g_units;
enum class ComposerPhase : std::uint8_t {
    selecting,
    previewing,
    confirming,
    sending,
    outcome_unknown,
    complete,
};
ComposerPhase g_composer_phase = ComposerPhase::selecting;
UnitSnapshot g_selected_unit{};
std::uint64_t g_selected_gift_id = 0;
std::uint32_t g_storage_local_page = 0;
Command g_storage_list_command = Command::list_inbox;
bool g_returns_needed = true;
bool g_returns_active = false;
bool g_inventory_was_visible = false;
bool g_populate_after_inventory = false;
std::uint32_t g_returns_next_page = 0;
std::uint64_t g_returns_session = 0;
std::uint64_t g_returns_retry_at = 0;
std::uint64_t g_returns_poll_at = 0;
std::uint64_t g_selection_detail_uid = 0;
Command g_uncertain_command = Command::hello;
RequestId g_uncertain_request_id{};
bool g_has_uncertain_operation = false;
enum class ProvenanceOrigin : std::uint8_t {
    none,
    storage,
    unit_detail,
};
ProvenanceOrigin g_provenance_origin = ProvenanceOrigin::none;
std::uint64_t g_provenance_request_uid = 0;

struct ObserverState {
    std::uint64_t run_hash = 0;
    std::uint32_t action_sequence = 0;
    std::uint32_t reply_sequence = 0;
    std::uint32_t last_action = 0;
    std::uint32_t last_accepted_operation = 0;
    std::uint32_t last_reply_command = 0;
    WireStatus last_reply_status = WireStatus::disconnected;
    RequestId last_reply_request_id{};
    std::uint64_t last_reply_gift_id = 0;
    std::uint64_t last_reply_unit_uid = 0;
};
ObserverState g_observer;

void RenderStorageRows();
bool ControlReady(void* control);
bool CreateComposerCard(std::uint64_t uid);
void PopulateComposer(void* service);
void RefreshComposerSummary();
bool OpenReceivedInfo(const Detail& detail);
void CloseComposer();
bool BeginGiftAction(Command command);
void* ResolveService(const wchar_t* name,const void* expectedVtable);
void RefreshUnitDetailView();
bool OpenHomeFriendContext(std::uint32_t receiver);
bool ArmGameThread();
std::uint64_t CurrentSessionKey() noexcept;
void RefreshNativeInventoryView();

bool __stdcall CaptureDecodedReply(
    const std::uint8_t* decoded,const std::uint8_t* outer,std::uint32_t size) noexcept {
    const auto error=GetLastError();
    bool consumed=false;
    __try {
        if(decoded && outer && size>=2 && size<=0x100000) {
            auto plainSize=size;
            const auto outerOpcode=wire::Read16(outer);
            if(outerOpcode==0x65 || outerOpcode==0x66) {
                plainSize=size>=8 ? wire::Read16(outer+6) : 0;
                if(plainSize>size-8)plainSize=0;
            }
            if(plainSize>=2 && wire::Read16(decoded)==kNativeReplyOpcode &&
                InterlockedIncrement(&g_receive_carrier_observations)<=5 && g_api && g_api->Log) {
                int magicOffset=-1;
                for(std::size_t at=0;at+8<=plainSize && at<=32;++at)
                    if(!std::memcmp(decoded+at,kEnvelopeMagic.data(),8)){magicOffset=static_cast<int>(at);break;}
                g_api->Log(g_mod,SDGO_LOG_INFO,"native decoded storage carrier bytes=%u magic_offset=%d",plainSize,magicOffset);
            }
            consumed=g_native_replies.Capture(decoded,plainSize);
            if (plainSize>=2 && wire::Read16(decoded)==0x0699) {
                const bool tracked=g_native_details.Observe(decoded,plainSize);
                if (g_native_detail_receipts.fetch_add(1)<5)
                    g_api->Log(g_mod,SDGO_LOG_INFO,"native unit detail reply observed: bytes=%u tracked=%u",plainSize,tracked?1u:0u);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {consumed=false;}
    SetLastError(error);return consumed;

}

// This site is AFTER the one original decode call, so other Mods may retain
// their receiver/decoder entry hooks. Nulling only a consumed result follows
// the receiver's existing null-result exit; all other packets are unchanged.
__declspec(naked) void ReceiveDecodedSite() {
    __asm {pushfd}
    __asm {pushad}
    __asm {mov eax,esp}
    __asm {sub esp,0x210}
    __asm {and esp,0xfffffff0}
    __asm {mov [esp+0x200],eax}
    __asm {fxsave [esp]}
    __asm {fninit}
    __asm {ldmxcsr dword ptr [g_receive_default_mxcsr]}
    __asm {cld}
    __asm {mov eax,[esp+0x200]}
    __asm {push dword ptr [ebp+0x18]}
    __asm {push dword ptr [ebp+0x14]}
    __asm {push dword ptr [eax+0x1C]}
    __asm {call CaptureDecodedReply}
    __asm {test al,al}
    __asm {jz pass_native}
    __asm {mov eax,[esp+0x200]}
    __asm {mov dword ptr [eax+0x1C],0}
pass_native:
    __asm {fxrstor [esp]}
    __asm {mov esp,[esp+0x200]}
    __asm {popad}
    __asm {popfd}
    __asm {jmp dword ptr [g_receive_decoded_original]}
}

bool __cdecl RequestNativeDetail(std::uint64_t uid, void*) {
    g_native_details.Expect(CurrentSessionKey(),uid);
    const bool sent=g_transport.RequestNativeUnitDetails(uid);
    if (!sent) g_native_details.Expect(0,0);
    return sent;
}

void ApplyNativeDetails() noexcept {
    __try {
        NativeUnitDetailQueue::Record record{};
        while (g_native_details.Pop(CurrentSessionKey(),&record)) {
            if (!g_unit_info || !*g_unit_info || !g_sync_full || !g_brief) continue;
            auto* info=*g_unit_info;
            const int capacity=Read<int>(info,0x0C);
            const auto slot=Read<std::uint16_t>(record.data(),4);
            const auto uid=Read<std::uint64_t>(record.data(),8);
            if (Read<std::uint8_t>(info,0x20)!=1 || !Read<std::uint32_t>(info,4) ||
                capacity<1 || capacity>1200 || !slot || slot>capacity || g_inventory_sync.HasFull(uid)) continue;
            std::uint8_t brief[25]{};
            g_brief(info,brief,slot);
            const auto existing=Read<std::uint64_t>(brief,4);
            if (Read<std::uint32_t>(brief) && existing && existing!=uid) continue;
            g_sync_full(info,record.data());
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void __cdecl NativeInventoryChanged(void*) {
    RefreshNativeInventoryView();
}

void ScheduleInventoryMutation(Command command, std::uint64_t uid) {
    if (!uid) return;
    g_inventory_sync.SetSession(CurrentSessionKey());
    const auto effect = runtime::LocalInventoryEffect(command, WireStatus::success);
    if (effect == runtime::InventoryEffect::remove_local)
        g_inventory_sync.QueueRemoval(uid);
    else if (effect == runtime::InventoryEffect::add_local)
        g_inventory_sync.QueueDetail(uid);
}

ui::NativeString NativeView(const std::wstring& value) {
    ui::NativeString string;
    string.length = static_cast<std::uint32_t>(value.size());
    if (value.size() < 8) {
        std::memcpy(
            string.data.small,
            value.c_str(),
            (value.size() + 1) * sizeof(wchar_t));
    } else {
        string.data.pointer = value.c_str();
        string.capacity = string.length;
    }
    return string;
}

std::uint64_t CurrentSessionKey() noexcept {
    if (!g_session) return 0;
    __try {
        auto* session = g_session();
        if (!session) return 0;
        const int connection = Read<int>(session, 0x14);
        if (connection < 0) return 0;
        return (static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(session))
                << 32u) |
               static_cast<std::uint32_t>(connection);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

RequestId NewRequestId() noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const auto sequence =
        g_request_sequence.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t first =
        static_cast<std::uint64_t>(counter.QuadPart) ^
        (sequence * 0x9E3779B97F4A7C15ull);
    std::uint64_t second =
        GetTickCount64() ^
        (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32u) ^
        reinterpret_cast<std::uintptr_t>(&g_transport);
    RequestId id;
    std::memcpy(id.bytes.data(), &first, sizeof(first));
    std::memcpy(
        id.bytes.data() + sizeof(first), &second, sizeof(second));
    if (Empty(id)) id.bytes[0] = 1;
    return id;
}

std::uint64_t CurrentClientRunHash() noexcept {
    char run[64]{};
    char role[16]{};
    const DWORD runLength =
        GetEnvironmentVariableA("SDGO_GAME_TEST_RUN", run, sizeof(run));
    const DWORD roleLength =
        GetEnvironmentVariableA("SDGO_GAME_TEST_ROLE", role, sizeof(role));
    if (!runLength || runLength >= sizeof(run) ||
        !roleLength || roleLength >= sizeof(role)) {
        return 0;
    }
    return sdgo::game_test_page_observer::ClientRunHash(
        run, runLength, role, roleLength);
}

std::uint64_t SyncObserver() noexcept {
    const auto runHash = CurrentClientRunHash();
    if (g_observer.run_hash != runHash) {
        g_observer = {};
        g_observer.run_hash = runHash;
    }
    return runHash;
}

void RefreshAvailability() {
    const auto session = CurrentSessionKey();
    if (g_transport.Ready()) {
        g_model.SyncCapabilities(
            session,
            g_transport.Capabilities(),
            g_transport.ServerEpoch());
    } else {
        g_model.SyncCapabilities(session, 0, 0);
    }
    const bool offer =
        g_native_ready && g_model.CanOffer() &&
        !g_transport.MutationUncertain() &&
        !g_inventory_sync.Busy() &&
        g_inventory_sync.error() == NativeInventorySync::Error::none;
    const bool resolve =
        g_native_ready && g_model.CanResolveExisting();
    g_friend_main.SetAvailable(offer);
    g_friend_three_d.SetAvailable(offer);
    g_friend_community.SetAvailable(offer && g_community_friend);
    g_storage_view.SetAvailable(resolve);
    // A late stock inbox refresh can reset the borrowed native row controls.
    // Restore the active gift presentation on the normal UI timer as well as
    // after packets, including when a background inventory query finishes.
    static std::uint32_t storageState=~0u;
    if (g_storage_view.custom_view()) {
        const std::uint32_t state=(g_model.pending()?1u:0u) |
            (g_inventory_sync.Busy()?2u:0u) |
            (g_inventory_sync.error()!=NativeInventorySync::Error::none?4u:0u) |
            (g_transport.MutationUncertain()?8u:0u);
        const auto* first=g_model.row(static_cast<std::size_t>(g_storage_local_page)*5u);
        if (state!=storageState || (first &&
            !ControlReady(g_storage_view.ControlFor(g_storage_view.RowActionForGift(first->gift_id))))) {
            RenderStorageRows();storageState=state;
        }
    } else storageState=~0u;
    RefreshUnitDetailView();
}

bool BeginRequest(Request request) {
    if (Empty(request.request_id)) {
        if (request.command == Command::receipt) {
            request.request_id = g_transport.UncertainRequestId();
            if (Empty(request.request_id)) return false;
        } else {
            request.request_id = NewRequestId();
        }
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (!g_model.Begin(request, now)) return false;
    if (g_transport.Send(request)) return true;
    g_model.Fail(
        CurrentSessionKey(),
        request.command,
        request.request_id,
        WireStatus::send_failed);
    return false;
}

bool SnapshotUnitsUnsafe(std::vector<UnitSnapshot>* output) {
    if (!output || !g_unit_info || !g_profile ||
        !*g_unit_info || !*g_profile ||
        !g_brief || !g_full) {
        return false;
    }
    auto* info = *g_unit_info;
    if (Read<std::uint8_t>(info, 0x20) != 1) return false;
    const int capacity = Read<int>(info, 0x0C);
    if (capacity <= 0 || capacity > 1200) return false;
    const auto active = Read<std::uint16_t>(*g_profile, 0x1AD);

    output->clear();
    for (int slot = 1; slot <= capacity; ++slot) {
        std::uint8_t brief[25]{};
        g_brief(info, brief, static_cast<std::uint16_t>(slot));
        const auto unitNumber = Read<std::uint32_t>(brief);
        const auto uid = Read<std::uint64_t>(brief, 4);
        const auto nativeSlot = Read<std::uint16_t>(brief, 12);
        if (!unitNumber || !uid || nativeSlot != slot ||
            nativeSlot == active || !runtime::NativeBriefAllowsGift(brief)) {
            continue;
        }
        UnitSnapshot unit;
        unit.uid = uid;
        unit.unit_number = unitNumber;
        unit.slot = nativeSlot;
        unit.level = brief[14];
        g_full(info, unit.full_record.data(), nativeSlot);
        const bool fullReady = Read<std::uint32_t>(unit.full_record.data()) == unitNumber &&
            Read<std::uint16_t>(unit.full_record.data(), 4) == nativeSlot &&
            Read<std::uint64_t>(unit.full_record.data(), 8) == uid;
        if (fullReady && (unit.full_record[6] != 0 || unit.full_record[137] != 0)) {
            continue;
        }
        if (!fullReady) unit.full_record.fill(0);
        output->push_back(unit);
    }
    return true;
}

bool ReadNativeUnitName(std::uint32_t id,wchar_t* output) noexcept {
    __try {
        const std::uint8_t* row=nullptr;
        auto* grid=g_grid_data ? g_grid_data() : nullptr;
        if (!grid || !g_grid_find || !g_grid_find(grid,&row,id,2,1) || !row ||
            Read<std::uint32_t>(row)!=id) return false;
        for (std::size_t i=0;i<50;++i) {
            output[i]=static_cast<wchar_t>(Read<std::uint16_t>(row,0xCD+2*i));
            if (!output[i]) return i!=0;
        }
        output[50]=0;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring UnitDisplayName(std::uint32_t id) {
    wchar_t native[51]{};
    const std::wstring source=ReadNativeUnitName(id,native) ? native : L"机体名称暂不可用";
    std::wstring translated;
    return sdgo::localization::TryLookup(sdgo::localization::Kind::UnitName,id,source,&translated)
        ? translated : source;
}

bool ReadNativeUnitIcon(std::uint32_t id,wchar_t* output) noexcept {
    __try {
        const std::uint8_t* row=nullptr;
        auto* grid=g_grid_data ? g_grid_data():nullptr;
        if (!grid || !g_grid_find || !g_grid_find(grid,&row,id,2,1) || !row) return false;
        for (std::size_t i=0;i<128;++i) {
            output[i]=static_cast<wchar_t>(Read<std::uint16_t>(row,0x12F6+i*2));
            if (!output[i]) return i!=0;
        }
        output[128]=0;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
std::wstring UnitIconPath(std::uint32_t id) {
    wchar_t path[129]{};return ReadNativeUnitIcon(id,path) ? path:L"";
}
void RestoreNativeStorage(void* service) {
    if (g_storage_labels && g_storage_render) {g_storage_labels(service);g_storage_render(service);}
}

bool SnapshotUnits(std::vector<UnitSnapshot>* output) {
    __try {
        return SnapshotUnitsUnsafe(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (output) output->clear();
        return false;
    }
}

void LogUnavailableUnit(std::uint64_t uid) noexcept {
    __try {
        if (!g_unit_info || !*g_unit_info || !g_brief || !g_full) return;
        auto* info=*g_unit_info;
        const int capacity=Read<int>(info,0x0C);
        if (capacity<1 || capacity>1200) return;
        for (int slot=1;slot<=capacity;++slot) {
            std::uint8_t brief[25]{};
            g_brief(info,brief,static_cast<std::uint16_t>(slot));
            if (Read<std::uint64_t>(brief,4)!=uid) continue;
            std::uint8_t full[153]{};
            g_full(info,full,static_cast<std::uint16_t>(slot));
            g_api->Log(g_mod,SDGO_LOG_WARN,
                "native gift selection record: uid=%llu unit=%u slot=%u level=%u flags=%u/%u/%u/%u full_selected=%u full_locked=%u full_uid=%llu",
                static_cast<unsigned long long>(uid),Read<std::uint32_t>(brief),
                Read<std::uint16_t>(brief,12),brief[14],brief[15],brief[16],brief[17],brief[18],
                full[6],full[137],static_cast<unsigned long long>(Read<std::uint64_t>(full,8)));
            return;
        }
        g_api->Log(g_mod,SDGO_LOG_WARN,"native gift requested unit is absent from the brief cache: uid=%llu",
                   static_cast<unsigned long long>(uid));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

bool CurrentUnit(
    std::uint16_t slot,
    UnitSnapshot* output) {
    if (!output) return false;
    std::vector<UnitSnapshot> current;
    if (!SnapshotUnits(&current)) return false;
    for (const auto& unit : current) {
        if (unit.slot == slot) {
            *output = unit;
            return true;
        }
    }
    return false;
}

bool FindOwnedUnitUnsafe(
    std::uint64_t uid,
    UnitSnapshot* output) {
    if (!uid || !output || !g_unit_info || !*g_unit_info ||
        !g_full) {
        return false;
    }
    auto* info = *g_unit_info;
    if (Read<std::uint8_t>(info, 0x20) != 1) return false;
    const int capacity = Read<int>(info, 0x0C);
    if (capacity <= 0 || capacity > 1200) return false;
    for (int slot = 1; slot <= capacity; ++slot) {
        UnitSnapshot candidate;
        g_full(
            info,
            candidate.full_record.data(),
            static_cast<std::uint16_t>(slot));
        candidate.unit_number =
            Read<std::uint32_t>(candidate.full_record.data());
        candidate.slot =
            Read<std::uint16_t>(candidate.full_record.data(), 4);
        candidate.uid =
            Read<std::uint64_t>(candidate.full_record.data(), 8);
        if (!candidate.unit_number ||
            candidate.slot != slot ||
            candidate.uid != uid) {
            continue;
        }
        *output = candidate;
        return true;
    }
    return false;
}

bool FindOwnedUnit(
    std::uint64_t uid,
    UnitSnapshot* output) {
    __try {
        return FindOwnedUnitUnsafe(uid, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (output) *output = {};
        return false;
    }
}

bool ShowNativeUnitCard(std::uint64_t uid) {
    UnitSnapshot unit;
    if (!FindOwnedUnit(uid,&unit) || !g_alloc || !g_free || !g_enqueue ||
        !g_unit_card_ctor || !g_unit_card_dtor) return false;
    auto* temporary=g_alloc(0x148);
    if (!temporary) return false;
    g_unit_card_ctor(temporary,unit.full_record.data(),0);
    auto* live=g_enqueue(temporary,1);
    g_unit_card_dtor(temporary);g_free(temporary);
    return live!=nullptr;
}

bool OwnBriefContains(std::uint64_t uid) noexcept {
    __try {
        if (!uid || !g_unit_info || !*g_unit_info || !g_brief) return false;
        auto* info=*g_unit_info;
        const int capacity=Read<int>(info,0x0C);
        if (Read<std::uint8_t>(info,0x20)!=1 || capacity<1 || capacity>1200) return false;
        for (int slot=1;slot<=capacity;++slot) {
            std::uint8_t brief[25]{};g_brief(info,brief,static_cast<std::uint16_t>(slot));
            if (Read<std::uint32_t>(brief) && Read<std::uint64_t>(brief,4)==uid) return true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

bool ReadNote(
    void* service,
    std::array<char16_t, kMaximumNoteCodeUnits>* note,
    std::uint16_t* codeUnits) {
    if (!service || !note || !codeUnits || !g_edit_get) return false;
    auto* root = Read<void*>(service, 0x6C);
    auto* edit = g_ui.FindNamed(root, L"DescEdit", 8);
    if (!edit) return false;
    auto* text = g_edit_get(edit);
    if (!text || text->length > kMaximumNoteCodeUnits) return false;
    const wchar_t* source =
        text->capacity < 8 ? text->data.small : text->data.pointer;
    if (text->length && !source) return false;
    note->fill(0);
    for (std::size_t i = 0; i < text->length; ++i) {
        (*note)[i] = static_cast<char16_t>(source[i]);
    }
    if (!wire::ValidUtf16(note->data(), text->length)) return false;
    *codeUnits = static_cast<std::uint16_t>(text->length);
    return true;
}

bool WriteNativeNote(void* edit,const wchar_t* text) {
    if (!edit || !text || !g_wstring_ctor || !g_wstring_dtor || !g_edit_set) return false;
    alignas(4) std::uint8_t native[0x1C]{};
    bool constructed=false;
    __try {
        g_wstring_ctor(native,text);constructed=true;
        g_edit_set(edit,reinterpret_cast<const ui::NativeString*>(native),0);
        return true;
    } __finally {
        if (constructed) g_wstring_dtor(native);
    }
}

bool SetNote(
    void* service,
    const std::uint16_t* note,
    std::uint16_t codeUnits) {
    if (!service || !note || !g_edit_set ||
        codeUnits > kMaximumNoteCodeUnits) {
        return false;
    }
    const auto* utf16 = reinterpret_cast<const char16_t*>(note);
    if (!wire::ValidUtf16(utf16, codeUnits)) return false;
    auto* root = Read<void*>(service, 0x6C);
    auto* edit = g_ui.FindNamed(root, L"DescEdit", 8);
    if (!edit || !g_ui.SetNoteLimit(edit)) return false;
    std::wstring text;
    text.reserve(codeUnits);
    for (std::uint16_t index = 0; index < codeUnits; ++index) {
        text.push_back(static_cast<wchar_t>(note[index]));
    }
    if (!WriteNativeNote(edit,text.c_str())) return false;
    std::array<char16_t, kMaximumNoteCodeUnits> actual{};
    std::uint16_t actualUnits = 0;
    return ReadNote(service, &actual, &actualUnits) &&
           actualUnits == codeUnits &&
           std::memcmp(actual.data(), note,
               codeUnits * sizeof(char16_t)) == 0;
}

void SetComposerPrimary(
    const std::wstring& caption,
    bool enabled) {
    if (!g_composer) return;
    auto* primary = g_composer_primary;
    g_ui.SetState(primary, true, enabled && !g_composer_reload_uid);
    g_ui.SetText(primary, caption);
}

void SetComposerNotice(const std::wstring& message) {
    if (!g_composer) return;
    if (g_composer_notice) g_ui.SetText(g_composer_notice, message, 1);
}

void RefreshComposerSummary() {
    if (!g_composer) return;
    auto* root=Read<void*>(g_composer,0x6C);
    if (!root) return;
    g_ui.SetText(g_ui.FindNamed(root,L"TitleStatic",3),(g_composer_received ? L"赠送者：":L"赠送给 ")+g_receiver_name,1);
    g_ui.SetText(g_ui.FindNamed(root,L"UnitNameStatic",3),
        g_selected_unit.uid ? g_ui.UnitCaption(g_selected_unit.unit_number,g_selected_unit.level) : L"正在读取机体…");
    g_ui.SetState(g_ui.FindNamed(root,L"UnitNameStatic",3),true,false);
    g_ui.SetState(g_ui.FindNamed(root,L"SkillTitleStatic1",3),false,false);
    for (const auto* key:{L"LockIcon0",L"LockIcon1",L"AttributeSkillIcon0",L"AttributeSkillIcon1",L"AttributeSkillIcon2",L"AttributeSkillIcon3",L"SkillFrameIcon0",L"SkillFrameIcon1",L"SkillFrameIcon2",L"SkillFrameIcon3",L"GBlindIcon",L"GBlindStatic",L"AttributeSkillBlindIcon"}) {
        auto* control=g_ui.FindNamed(root,key,std::wcscmp(key,L"GBlindStatic")==0 ? 3 : 1);
        g_ui.SetState(control,false,false);
    }
    if (g_composer_received) {
        for (const auto* key:{L"SkillTitleStatic0",L"SkillStatic0",L"SkillStatic1",L"SkillStatic2",L"SkillStatic3"})
            g_ui.SetState(g_ui.FindNamed(root,key,3),false,false);
        for (const auto* key:{L"SkillIcon0",L"SkillIcon1",L"SkillIcon2",L"SkillIcon3"})
            g_ui.SetState(g_ui.FindNamed(root,key,1),false,false);
    }

}

void SetComposerEditable(bool enabled) {
    if (g_composer_note) g_ui.SetState(g_composer_note,true,enabled);
}

bool SelectComposerUnit(std::uint64_t uid) {
    if (!g_composer || !g_selected_unit.uid || (uid && uid!=g_selected_unit.uid)) return false;
    UnitSnapshot current;
    if (!CurrentUnit(g_selected_unit.slot,&current) || current.uid!=g_selected_unit.uid) return false;
    g_selected_unit=current;
    g_model.CancelOffer();
    return g_inventory_sync.HasFull(current.uid);
}

std::wstring StatusText(WireStatus status) {
    switch (status) {
    case WireStatus::success:
        return L"服务器已确认。";
    case WireStatus::disabled:
        return L"服务器已关闭新的机体赠送。";
    case WireStatus::not_mutual_friend:
        return L"双方当前不是互相好友。";
    case WireStatus::unit_unavailable:
    case WireStatus::unit_ineligible:
        return L"该机体当前不能赠送。";
    case WireStatus::capacity_full:
        return L"对方机库容量不足。";
    case WireStatus::returning_waiting_capacity:
        return L"退回机体正在托管，等待发送方机库容量。";
    case WireStatus::timed_out:
        return L"服务器回应超时；不会自动重发。";
    case WireStatus::outcome_unknown:
        return L"结果待确认；不会自动重发，请重新连接后查询原请求结果。";
    case WireStatus::send_failed:
    case WireStatus::transport_unavailable:
        return L"赠送通道当前不可用。";
    default:
        return L"服务器拒绝了此操作。";
    }
}

std::wstring FormatUnixMillis(std::uint64_t value) {
    constexpr std::uint64_t kUnixToFileTimeMillis =
        11644473600000ull;
    if (value >
        (UINT64_MAX / 10000ull) -
            kUnixToFileTimeMillis) {
        return std::to_wstring(value);
    }
    ULARGE_INTEGER native{};
    native.QuadPart =
        (value + kUnixToFileTimeMillis) * 10000ull;
    FILETIME fileTime{};
    fileTime.dwLowDateTime = native.LowPart;
    fileTime.dwHighDateTime = native.HighPart;
    SYSTEMTIME time{};
    if (!FileTimeToSystemTime(&fileTime, &time)) {
        return std::to_wstring(value);
    }
    wchar_t text[32]{};
    swprintf_s(
        text,
        L"%04u-%02u-%02u %02u:%02u UTC",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute);
    return text;
}

std::wstring ProvenanceText(const Detail& provenance) {
    std::wstring peer;
    peer.reserve(provenance.peer_name_code_units);
    for (std::size_t index = 0;
         index < provenance.peer_name_code_units;
         ++index) {
        peer.push_back(static_cast<wchar_t>(
            provenance.peer_name[index]));
    }
    if (peer.empty()) peer = L"未知发送方";

    std::wstring note;
    note.reserve(provenance.note_code_units);
    for (std::size_t index = 0;
         index < provenance.note_code_units;
         ++index) {
        note.push_back(static_cast<wchar_t>(
            provenance.note[index]));
    }
    if (note.empty()) note = L"无留言";

    std::wstring result =
        L"来源：" + peer +
        L"；赠送时间：" +
        FormatUnixMillis(provenance.offered_unix_millis);
    if (provenance.accepted_unix_millis) {
        result +=
            L"；接收时间：" +
            FormatUnixMillis(
                provenance.accepted_unix_millis);
    }
    result += L"；留言：" + note;
    return result;
}

std::wstring ProvenanceDetailText(
    const Detail& provenance) {
    std::wstring peer;
    peer.reserve(provenance.peer_name_code_units);
    for (std::size_t index = 0;
         index < provenance.peer_name_code_units;
         ++index) {
        peer.push_back(static_cast<wchar_t>(
            provenance.peer_name[index]));
    }
    if (peer.empty()) peer = L"未知发送方";

    std::wstring note;
    note.reserve(provenance.note_code_units);
    for (std::size_t index = 0;
         index < provenance.note_code_units;
         ++index) {
        note.push_back(static_cast<wchar_t>(
            provenance.note[index]));
    }
    if (note.empty()) note = L"无留言";

    std::wstring result =
        L"发送方：" + peer +
        L"\n赠送时间：" +
        FormatUnixMillis(provenance.offered_unix_millis);
    if (provenance.accepted_unix_millis) {
        result +=
            L"\n接收时间：" +
            FormatUnixMillis(
                provenance.accepted_unix_millis);
    }
    result += L"\n\n留言：\n" + note;
    return result;
}

void RefreshUnitDetailViewUnsafe() {
    auto* service = g_unit_detail_view.service();
    if (!service) return;
    if (!g_unit_detail_service_vtable ||
        Read<void*>(service) !=
            g_unit_detail_service_vtable) {
        g_unit_detail_view.Detach(service);
        return;
    }
    const auto unitUid =
        Read<std::uint64_t>(service, 0x9C);
    const bool capable =
        g_native_ready && g_transport.Ready() &&
        g_model.CanReadProvenance();
    const auto& pending = g_model.pending_request();
    const bool waiting =
        pending &&
        pending->command == Command::provenance &&
        pending->unit_uid == unitUid;
    const bool busy =
        pending.has_value() ||
        g_transport.MutationUncertain();

    std::wstring caption = L"查看赠送来源";
    const auto& cached = g_model.provenance();
    if (cached.present &&
        cached.unit_uid == unitUid) {
        caption = g_unit_detail_view.detail_visible()
                      ? L"收起赠送来源"
                      : L"展开赠送来源";
    } else if (waiting) {
        caption = L"正在读取赠送来源…";
    }
    g_unit_detail_view.SetUnit(
        unitUid,
        capable,
        capable && !busy,
        caption);
}

void RefreshUnitDetailView() {
    __try {
        RefreshUnitDetailViewUnsafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

const GiftSummary* SelectedGift() {
    if (!g_selected_gift_id) return nullptr;
    for (std::size_t index = 0; index < g_model.row_count(); ++index) {
        const auto* row = g_model.row(index);
        if (row && row->gift_id == g_selected_gift_id) return row;
    }
    return nullptr;
}

void RenderStorageRows() {
    std::array<GiftSummary, 5> rows{};
    const std::size_t first =
        static_cast<std::size_t>(g_storage_local_page) * rows.size();
    std::size_t shown = 0;
    for (; shown < rows.size(); ++shown) {
        const auto* row = g_model.row(first + shown);
        if (!row) break;
        rows[shown] = *row;
    }
    const std::uint32_t displayPage =
        (g_model.page() ? g_model.page() - 1u : 0u) * 4u +
        g_storage_local_page + 1u;
    g_storage_view.RenderRows(
        rows.data(),
        shown,
        displayPage,
        g_model.total_count(),
        g_selected_gift_id,
        !g_model.pending() && !g_transport.MutationUncertain() &&
            !g_inventory_sync.Busy() &&
            g_inventory_sync.error() == NativeInventorySync::Error::none,
        g_has_uncertain_operation &&
            g_transport.Ready() &&
            !g_model.pending());
}

bool BeginStorageList(Command command, std::uint32_t page) {
    if (command != Command::list_inbox &&
        command != Command::list_returned) {
        return false;
    }
    g_inventory_sync.Retry();
    g_returns_needed = true;
    g_returns_retry_at = 0;
    Request request;
    request.command = command;
    request.page = page ? page : 1;
    if (!BeginRequest(request)) return false;
    g_storage_list_command = command;
    g_storage_local_page = 0;
    g_selected_gift_id = 0;
    g_storage_view.ShowStatus(
        command == Command::list_inbox
            ? L"正在读取收到的机体礼物…"
            : L"正在读取退回给发送方的机体…",
        true);
    RenderStorageRows();
    return true;
}

bool BeginGiftAction(Command command) {
    const auto* row = SelectedGift();
    if (!row || g_model.pending() ||
        g_transport.MutationUncertain() ||
        (command != Command::provenance &&
         (g_inventory_sync.Busy() ||
          g_inventory_sync.error() != NativeInventorySync::Error::none))) {
        return false;
    }
    Request request;
    request.command = command;
    if (command == Command::provenance) {
        request.unit_uid = row->unit_uid;
    } else {
        request.gift_id = row->gift_id;
    }
    if (command == Command::provenance) {
        g_provenance_origin = ProvenanceOrigin::storage;
        g_provenance_request_uid = request.unit_uid;
    }
    if (!BeginRequest(request)) {
        if (command == Command::provenance) {
            g_provenance_origin = ProvenanceOrigin::none;
            g_provenance_request_uid = 0;
        }
        return false;
    }
    g_storage_view.ShowStatus(
        command == Command::provenance || command == Command::detail
            ? L"正在读取机体信息…"
            : L"正在处理赠礼…",
        true);
    RenderStorageRows();
    return true;
}

bool BeginUnitDetailProvenance(std::uint64_t unitUid) {
    if (!unitUid || g_model.pending() ||
        !g_model.CanReadProvenance() ||
        !g_transport.Ready()) {
        return false;
    }
    Request request;
    request.command = Command::provenance;
    request.unit_uid = unitUid;
    g_provenance_origin = ProvenanceOrigin::unit_detail;
    g_provenance_request_uid = unitUid;
    if (!BeginRequest(request)) {
        g_provenance_origin = ProvenanceOrigin::none;
        g_provenance_request_uid = 0;
        return false;
    }
    RefreshUnitDetailView();
    return true;
}

bool BeginReceiptQuery() {
    if (!g_has_uncertain_operation ||
        Empty(g_uncertain_request_id) ||
        g_model.pending() ||
        !g_transport.Ready()) {
        return false;
    }
    Request request;
    request.command = Command::receipt;
    request.request_id = g_uncertain_request_id;
    if (!BeginRequest(request)) return false;
    if (g_composer &&
        g_uncertain_command == Command::offer) {
        SetComposerPrimary(L"正在查询原请求…", false);
        SetComposerNotice(
            L"正在按原请求编号查询结果；不会提交新的转移。");
    } else {
        g_storage_view.ShowStatus(
            L"正在按原请求编号查询结果；不会重复处理机体。",
            true);
        RenderStorageRows();
    }
    return true;
}

void __cdecl OnTransport(
    const TransportEvent* event,
    void*) {
    if (!event) return;
    const auto session = CurrentSessionKey();
    if (event->kind == EventKind::reply && event->reply) {
        const bool matched = g_model.Receive(session, *event->reply);
        if (!matched) {
            g_model.SyncCapabilities(
                session,
                g_transport.Capabilities(),
                g_transport.ServerEpoch());
        } else {
            if (event->status == WireStatus::success)
                ScheduleInventoryMutation(event->command, event->reply->unit_uid);
            if (g_composer_received && event->reply->gift_id==g_received_detail.gift_id &&
                (event->command==Command::accept || event->command==Command::return_to_sender || event->command==Command::takeback) &&
                (event->status==WireStatus::success || event->status==WireStatus::returning_waiting_capacity)) CloseComposer();
            if (event->command == Command::inventory_returns) {
                g_inventory_sync.SetSession(CurrentSessionKey());
                g_returns_active = false;
                if (event->status == WireStatus::success) {
                    for (std::size_t i=0;i<event->reply->row_count;++i)
                        g_inventory_sync.QueueDetail(event->reply->rows[i].unit_uid);
                    if (static_cast<std::uint64_t>(event->reply->page)*kMaximumRows < event->reply->total_count)
                        g_returns_next_page = event->reply->page + 1;
                    g_populate_after_inventory = true;
                } else {
                    g_returns_needed = true;
                    g_returns_retry_at = GetTickCount64()+10000;
                }
            }
            if (SyncObserver()) {
                ++g_observer.reply_sequence;
                g_observer.last_reply_command =
                    static_cast<std::uint32_t>(event->command);
                g_observer.last_reply_status = event->status;
                g_observer.last_reply_request_id = event->request_id;
                g_observer.last_reply_gift_id = event->reply->gift_id;
                g_observer.last_reply_unit_uid = event->reply->unit_uid;
            }
            if (event->command==Command::detail) {
                if (event->status==WireStatus::success && g_storage_view.custom_view() &&
                    g_selected_gift_id==g_model.detail().gift_id) OpenReceivedInfo(g_model.detail());
                else g_storage_view.ShowStatus(StatusText(event->status),true);
            }
            if (event->command == Command::preview_offer) {
                if (event->status == WireStatus::success &&
                    g_model.HasOfferConfirmation()) {
                    g_composer_phase = ComposerPhase::confirming;
                    SetComposerPrimary(L"确认赠送", true);
                    SetComposerNotice(
                        L"服务器预检通过：向 " + g_receiver_name +
                        L" 赠送 "+g_ui.UnitCaption(g_selected_unit.unit_number,g_selected_unit.level)+
                        L"。再次点击才会提交。");
                } else {
                    g_composer_phase = ComposerPhase::selecting;
                    SetComposerEditable(true);
                    g_model.CancelOffer();
                    SetComposerPrimary(L"服务器预检", true);
                    SetComposerNotice(StatusText(event->status));
                }
            } else if (event->command == Command::offer) {
                if (event->status == WireStatus::outcome_unknown) {
                    g_has_uncertain_operation = true;
                    g_uncertain_command = Command::offer;
                    g_uncertain_request_id = event->request_id;
                    g_composer_phase = ComposerPhase::outcome_unknown;
                    SetComposerPrimary(
                        L"查询原请求结果", g_transport.Ready());
                    SetComposerNotice(StatusText(event->status));
                } else {
                    g_has_uncertain_operation = false;
                    g_uncertain_request_id = {};
                    g_composer_phase = ComposerPhase::complete;
                    SetComposerPrimary(L"服务器已确认", false);
                    SetComposerNotice(StatusText(event->status));
                    if (event->status == WireStatus::success) {
                        g_observer.last_accepted_operation =
                            static_cast<std::uint32_t>(Command::offer);
                    }
                }
            } else if (event->command == Command::receipt) {
                if (event->status == WireStatus::outcome_unknown ||
                    event->status == WireStatus::database_unavailable ||
                    event->status == WireStatus::transport_unavailable) {
                    g_composer_phase = ComposerPhase::outcome_unknown;
                    SetComposerPrimary(
                        L"查询原请求结果", g_transport.Ready());
                    SetComposerNotice(StatusText(event->status));
                } else {
                    const auto resolvedCommand = g_uncertain_command;
                    if (g_has_uncertain_operation && event->status == WireStatus::success)
                        ScheduleInventoryMutation(resolvedCommand, event->reply->unit_uid);
                    if (g_has_uncertain_operation &&
                        (event->status == WireStatus::success ||
                         event->status ==
                             WireStatus::returning_waiting_capacity)) {
                        g_observer.last_accepted_operation =
                            static_cast<std::uint32_t>(
                                resolvedCommand);
                    }
                    g_has_uncertain_operation = false;
                    g_uncertain_request_id = {};
                    if (resolvedCommand == Command::offer) {
                        g_composer_phase = ComposerPhase::complete;
                        SetComposerPrimary(
                            L"已取得原请求结果", false);
                        SetComposerNotice(StatusText(event->status));
                    } else {
                        g_storage_view.ShowStatus(
                            StatusText(event->status), true);
                        BeginStorageList(
                            g_storage_list_command,
                            std::max<std::uint32_t>(
                                1, g_model.page()));
                    }
                }
            } else if (
                (event->command == Command::accept ||
                 event->command == Command::return_to_sender ||
                 event->command == Command::takeback) &&
                event->status == WireStatus::outcome_unknown) {
                g_has_uncertain_operation = true;
                g_uncertain_command = event->command;
                g_uncertain_request_id = event->request_id;
                g_storage_view.ShowStatus(
                    StatusText(WireStatus::outcome_unknown), true);
                RenderStorageRows();
            } else if (
                event->command == Command::accept ||
                event->command == Command::return_to_sender ||
                event->command == Command::takeback) {
                g_has_uncertain_operation = false;
                g_uncertain_request_id = {};
                g_storage_view.ShowStatus(
                    StatusText(event->status), true);
                if (event->status == WireStatus::success ||
                    event->status ==
                        WireStatus::returning_waiting_capacity) {
                    g_observer.last_accepted_operation =
                        static_cast<std::uint32_t>(
                            event->command);
                    BeginStorageList(
                        g_storage_list_command,
                        std::max<std::uint32_t>(1, g_model.page()));
                } else {
                    RenderStorageRows();
                }
            }
            if (event->command == Command::list_inbox ||
                event->command == Command::list_returned) {
                if (!SelectedGift()) g_selected_gift_id = 0;
                g_storage_view.ShowStatus(
                    L"机体赠礼：" + std::to_wstring(g_model.total_count()) + L" 台。点击机体图标查看信息。",
                    true);
                RenderStorageRows();
            } else if (
                event->command == Command::provenance) {
                if (g_provenance_origin ==
                    ProvenanceOrigin::unit_detail) {
                    const auto& provenance =
                        g_model.provenance();
                    if (provenance.present &&
                        provenance.unit_uid ==
                            g_provenance_request_uid &&
                        g_unit_detail_view.unit_uid() ==
                            g_provenance_request_uid) {
                        OpenReceivedInfo(provenance);
                    }
                    RefreshUnitDetailView();
                } else {
                    const auto& provenance =
                        g_model.provenance();
                    g_storage_view.ShowStatus(
                        provenance.present ? L"赠礼信息已读取。" : StatusText(event->status),
                        true);
                    RenderStorageRows();
                }
                g_provenance_origin =
                    ProvenanceOrigin::none;
                g_provenance_request_uid = 0;
            }
        }
    } else if (event->kind == EventKind::request_failed) {
        if (event->command==Command::inventory_returns) {
            g_returns_active=false;g_returns_needed=true;
            g_returns_retry_at=GetTickCount64()+10000;
        }
        const bool matched = g_model.Fail(
            session,
            event->command,
            event->request_id,
            event->status);
        const bool mutationCommand =
            event->command == Command::offer ||
            event->command == Command::accept ||
            event->command == Command::return_to_sender ||
            event->command == Command::takeback;
        if (matched &&
            (event->command == Command::preview_offer ||
             mutationCommand ||
             event->command == Command::receipt)) {
            if (g_transport.MutationUncertain() ||
                event->status == WireStatus::outcome_unknown) {
                g_has_uncertain_operation = true;
                g_uncertain_command = g_transport.UncertainCommand();
                g_uncertain_request_id =
                    g_transport.UncertainRequestId();
                g_composer_phase = ComposerPhase::outcome_unknown;
                SetComposerPrimary(
                    L"查询原请求结果", g_transport.Ready());
                if (mutationCommand &&
                    event->command != Command::offer) {
                    g_storage_view.ShowStatus(
                        StatusText(WireStatus::outcome_unknown), true);
                    RenderStorageRows();
                }
            } else {
                g_composer_phase = ComposerPhase::selecting;
                SetComposerEditable(true);
                g_model.CancelOffer();
                SetComposerPrimary(L"服务器预检", true);
            }
            SetComposerNotice(
                g_transport.MutationUncertain()
                    ? StatusText(WireStatus::outcome_unknown)
                    : StatusText(event->status));
        }
        if (event->command == Command::list_inbox ||
            event->command == Command::list_returned) {
            g_storage_view.ShowStatus(
                StatusText(event->status), true);
            RenderStorageRows();
        } else if (
            matched &&
            event->command == Command::provenance) {
            if (g_provenance_origin ==
                ProvenanceOrigin::unit_detail) {
                if (g_unit_detail_view.unit_uid() ==
                    g_provenance_request_uid) {
                    g_unit_detail_view.SetUnit(
                        g_provenance_request_uid,
                        g_model.CanReadProvenance(),
                        g_model.CanReadProvenance() &&
                            g_transport.Ready(),
                        StatusText(event->status));
                }
            } else {
                g_storage_view.ShowStatus(
                    StatusText(event->status), true);
                RenderStorageRows();
            }
            g_provenance_origin =
                ProvenanceOrigin::none;
            g_provenance_request_uid = 0;
        }
    }
    RefreshAvailability();
}

bool CreateComposerCard(std::uint64_t uid) {
    UnitSnapshot unit;
    if (!FindOwnedUnit(uid,&unit) || !g_unit_card_ctor || !g_unit_card_dtor) return false;
    auto* temporary=g_alloc(0x148);
    if (!temporary) return false;
    g_unit_card_ctor(temporary,unit.full_record.data(),0);
    auto* live=g_enqueue(temporary,1);
    g_unit_card_dtor(temporary);g_free(temporary);
    if (!live) return false;
    auto* old=g_composer;
    if (old && g_composer_note) {
        std::array<char16_t,kMaximumNoteCodeUnits> note{};
        std::uint16_t length=0;
        if (ReadNote(old,&note,&length)) {
            std::memcpy(g_composer_saved_note.data(),note.data(),length*sizeof(char16_t));
            g_composer_saved_note_size=length;
        }
    }
    g_composer=live;g_composer_card_uid=uid;g_composer_reload_uid=0;
    g_composer_opening=false;g_composer_controls=false;
    g_composer_note=nullptr;
    g_composer_primary=nullptr;g_composer_cancel=nullptr;g_composer_notice=nullptr;

    if (old) g_close(old,1);
    return true;
}

bool OpenComposer() {
    runtime::AuthenticatedContext context;
    if (!g_native_ready || !g_model.CanOffer() || g_composer || g_picker_selecting ||
        !runtime::ReadAuthenticatedContext(g_app ? g_app():nullptr,g_profile,&context) ||
        (context.app_state!=5 && context.app_state!=6)) return false;
    g_picker_selecting=true;g_picker_seen=false;g_picker_navigation_sent=false;
    g_picker_target_uid=0;g_picker_next_action=0;
    g_selected_unit={};g_composer_saved_note_size=0;
    g_composer_phase=ComposerPhase::selecting;g_composer_received=false;
    return true;
}

bool PickerControlReady(void* control) {
    return control && Read<std::uint8_t>(control,0x162) && Read<std::uint8_t>(control,0x167);
}

std::uint32_t __stdcall ManagementPagerEventHook(void* service,std::uint32_t event,std::uintptr_t name,void* control) {
    if (g_picker_selecting && event==0x101 &&
        Read<void*>(service)==g_management_pager_vtable && PickerControlReady(control)) {
        int button=-1;
        if (control==Read<void*>(service,0x48)) button=10;
        else if (control==Read<void*>(service,0x4C)) button=11;
        else for (int i=0;i<10;++i) {
            if (control==Read<void*>(service,0x50+static_cast<unsigned>(i)*4) ||
                control==Read<void*>(service,0xA0+static_cast<unsigned>(i)*4)) {button=i;break;}
        }
        if (button>=0) {
            const int current=Read<int>(service,0xDC),pages=Read<int>(service,0xD4);
            const int target=sdgo::friend_unit_gift::client::paging::TargetPage(current,pages,button);
            if (target>=0 && g_management_page_change) {
                // Browsing the gift picker changes only the native view. It
                // must not wait on unrelated friend/unit-detail requests.
                const auto steps=sdgo::friend_unit_gift::client::paging::Steps(current,target);
                g_picker_target_uid=0;g_picker_next_action=0;
                for (unsigned i=0;i<steps.count;++i)
                    g_management_page_change(service,steps.index[i],steps.forward[i]);
            }
            return 0;
        }
    }
    return g_management_pager_callback(service,event,name,control);
}

std::uint32_t __stdcall ManagementEventHook(void* service,std::uint32_t event,std::uintptr_t name,void* control) {
    if (!g_picker_selecting || event!=0x803 || Read<void*>(service)!=g_inventory_service_vtable)
        return g_management_callback(service,event,name,control);
    auto* root=Read<void*>(service,0x44);
    if (!root || !Read<std::uint8_t>(root,0x162)) return 0;
    for (std::size_t index=0;index<36;++index) {
        if (Read<void*>(service,0xE0+index*4)!=control) continue;
        const auto slot=Read<std::uint16_t>(service,0x7AC+index*2);
        UnitSnapshot unit;
        if (!CurrentUnit(slot,&unit)) {
            g_ui.SetText(g_ui.FindNamed(root,L"TitleStatic",3),L"该机体不能赠送，请双击选择其他机体");
            return 0;
        }
        g_selected_unit=unit;g_picker_target_uid=0;g_picker_selecting=false;
        g_composer_opening=true;g_composer_phase=ComposerPhase::selecting;
        g_inventory_sync.SetSession(CurrentSessionKey());
        if (!g_inventory_sync.HasFull(unit.uid)) g_inventory_sync.QueueDetail(unit.uid);
        return 0;
    }
    return 0;
}

void PumpGiftPicker() {
    if (!g_picker_selecting) return;
    runtime::AuthenticatedContext context;
    if (!runtime::ReadAuthenticatedContext(g_app ? g_app():nullptr,g_profile,&context)) return;
    if (context.app_state==6) {
        if (g_picker_navigation_sent) return;
        auto* menu=ResolveService(L"downmenu",g_downmenu_vtable);
        auto* root=menu ? Read<void*>(menu,0x44):nullptr;
        auto* control=menu ? Read<void*>(menu,0x54):nullptr;
        if (!root || g_ui.FindNamed(root,L"MyRoomRadio",12)!=control || !PickerControlReady(control)) return;
        g_downmenu_callback(menu,0x301,reinterpret_cast<std::uintptr_t>(static_cast<std::uint8_t*>(control)+0x118),control);
        g_picker_navigation_sent=true;return;
    }
    if (context.app_state!=5) return;
    auto* grid=ResolveService(L"UnitIndexingCubbyhole",g_inventory_service_vtable);
    auto* root=grid ? Read<void*>(grid,0x44):nullptr;
    if (!root || !Read<std::uint8_t>(root,0x162)) {
        if (g_picker_seen) {g_picker_selecting=false;g_picker_target_uid=0;return;}
        if (GetTickCount64()<g_picker_next_action) return;
        auto* menu=ResolveService(L"operatormenu",g_operator_vtable);
        auto* control=menu ? Read<void*>(menu,0x80):nullptr;
        auto* parent=control ? Read<void*>(control,0x15C):nullptr;
        if (!parent || g_ui.FindNamed(parent,L"UnitBtn",4)!=control || !PickerControlReady(control)) return;
        g_operator_callback(menu,0x101,reinterpret_cast<std::uintptr_t>(static_cast<std::uint8_t*>(control)+0x118),control);
        g_picker_next_action=GetTickCount64()+1000;return;
    }
    g_picker_seen=true;
    g_ui.SetText(g_ui.FindNamed(root,L"TitleStatic",3),L"双击选择要赠送的机体");
    if (!g_picker_target_uid) return;
    for (std::size_t index=0;index<36;++index) {
        const auto slot=Read<std::uint16_t>(grid,0x7AC+index*2);
        if (!slot || !g_unit_info || !*g_unit_info) continue;
        std::uint8_t brief[25]{};g_brief(*g_unit_info,brief,slot);
        if (Read<std::uint64_t>(brief,4)!=g_picker_target_uid) continue;
        auto* control=Read<void*>(grid,0xE0+index*4);
        if (!PickerControlReady(control) || Read<void*>(control,0x15C)!=root) return;
        ManagementEventHook(grid,0x803,reinterpret_cast<std::uintptr_t>(static_cast<std::uint8_t*>(control)+0x118),control);
        return;
    }
    if (GetTickCount64()<g_picker_next_action) return;
    auto* pager=ResolveService(L"UnitIndexingPage",g_management_pager_vtable);
    if (!pager || !g_management_page_change || !g_unit_info || !*g_unit_info) return;
    const int capacity=Read<int>(*g_unit_info,0xC);
    if (capacity<1 || capacity>1200) return;
    for (int slot=1;slot<=capacity;++slot) {
        std::uint8_t brief[25]{};g_brief(*g_unit_info,brief,static_cast<std::uint16_t>(slot));
        if (Read<std::uint64_t>(brief,4)!=g_picker_target_uid) continue;
        const int page=(slot-1)/36,current=Read<int>(pager,0xDC),pages=Read<int>(pager,0xD4);
        if (page<0 || page>=pages || page==Read<int>(grid,0x838)) return;
        if (page/10!=current/10) g_management_page_change(pager,-1,page>current ? 1u:0u);
        else g_management_page_change(pager,page%10,0);
        g_picker_next_action=GetTickCount64()+500;return;
    }

}

bool AttachComposerControls(void* service) {
    auto* root=Read<void*>(service,0x6C);
    if (!root || !g_add_combo || !g_add_edit) return false;
    const std::wstring noteName=L"DescEdit";
    const auto noteKey=NativeView(noteName);
    // Reuse the game's blank blue input panel. Only this sprite instance is
    // scaled; its cached frames and the original archive remain untouched.
    const std::wstring panelName=L"GiftNotePanel",panelPath=L"sprs\\17459.spr";
    const auto panelKey=NativeView(panelName),panelSkin=NativeView(panelPath);
    const auto stockRect=[&](const wchar_t* name,int type) {
        ui::CardRect rect;auto* control=g_ui.FindNamed(root,name,type);
        if (control) rect={static_cast<int>(Read<float>(control,0x148)),static_cast<int>(Read<float>(control,0x14C)),
            Read<int>(control,0x154),Read<int>(control,0x158)};
        return rect;
    };
    const auto layout=ui::InformationCardLayout(stockRect(L"SkillIcon0",1),stockRect(L"SkillIcon3",1),
        stockRect(L"SkillTitleStatic0",3),stockRect(L"LevelGageIcon",1),g_composer_received);
    if (!layout.valid) return false;
    g_ui.add_icon(root,&panelKey,layout.background.x,layout.background.y,
        layout.background.width,layout.background.height);
    auto* panel=g_ui.FindNamed(root,panelName.c_str(),1);
    if (!panel) return false;
    g_ui.skin(panel,&panelSkin,0,0,0);
    auto** skins=Read<std::uint8_t**>(panel,0x1A4);
    auto* sprite=skins && *skins ? Read<std::uint8_t*>(*skins) : nullptr;
    if (!sprite) return false;
    Put<float>(sprite,0x94,static_cast<float>(layout.background.width)/262.0f);
    Put<float>(sprite,0x98,static_cast<float>(layout.background.height)/61.0f);
    Put<std::uint8_t>(sprite,0xA0,1);
    Put<std::uint8_t>(panel,0x168,0);
    g_ui.SetState(panel,true,false);
    const std::wstring label=g_composer_received ? L"赠送寄语":L"寄语（可选，最多 200 字）";
    auto* noteLabel=g_ui.AddTextButton(root,L"GiftNoteLabel",layout.label.x,layout.label.y,
        layout.label.width,layout.label.height,label);
    g_ui.SetText(noteLabel,label,1);
    g_ui.SetState(noteLabel,true,false);
    g_add_edit(root,&noteKey,layout.note.x,layout.note.y,layout.note.width,layout.note.height);
    g_composer_note=g_ui.FindNamed(root,noteName.c_str(),8);
    if (!g_composer_note) return false;
    Put<std::uint8_t>(g_composer_note,0x168,0);
    auto* nativeName=g_ui.FindNamed(root,L"UnitNameStatic",3);
    auto noteFont=nativeName ? Read<std::uint32_t>(nativeName,0x16C) : 1u;
    g_set_font(g_composer_note,noteFont);
    g_api->Log(g_mod,SDGO_LOG_INFO,"native gift card layout: received=%u note=%d,%d/%dx%d buttons_y=%d footer_bottom=%d",
        g_composer_received?1u:0u,layout.panel.x,layout.panel.y,layout.panel.width,layout.panel.height,
        layout.primary.y,layout.primary.bottom());
    Put<std::uint8_t>(g_composer_note,0x29F,1);
    Put<std::uint32_t>(g_composer_note,0x2A4,2);
    g_ui.SetState(g_composer_note,true,g_ui.SetNoteLimit(g_composer_note));
    const auto addBlue=[&](const wchar_t* name,const ui::CardRect& rect,const std::wstring& caption) {
        auto* control=g_ui.AddTextButton(root,name,rect.x,rect.y,rect.width,rect.height,caption);
        if (!control) return control;
        for (int i=0;i<3;++i) {
            const std::wstring skinPath=L"sprs\\"+std::to_wstring(17530+i)+L".spr";
            const auto skin=NativeView(skinPath);g_ui.skin(control,&skin,0,i==1 ? 4u : 0u,i==1 ? 4u : 0u);
        }
        g_ui.SetState(control,true,true);return control;
    };
    g_composer_primary=addBlue(L"GiftConfirm",layout.primary,L"服务器预检");
    g_composer_cancel=addBlue(L"GiftCancel",layout.cancel,L"取消");
    g_composer_notice=g_ui.AddTextButton(root,L"GiftNotice",25,56,558,15,L"");
    g_ui.SetState(g_composer_notice,true,false);
    // Keep the original native frame, model, weapons, skills and level table.
    // Only the lower-right custom-parts area carries the gift note.
    Put<float>(root,0x148,197.0f);Put<float>(root,0x14C,128.0f);
    g_ui.refresh_geometry(root);
    g_composer_controls=g_composer_primary && g_composer_cancel;
    if (g_composer_saved_note_size) SetNote(service,g_composer_saved_note.data(),g_composer_saved_note_size);
    return g_composer_controls;
}

void PopulateComposer(void* service) {
    if (!service || !g_composer_controls) return;
    if (g_composer_received) {
        std::array<std::uint16_t,kMaximumNoteCodeUnits> note{};
        for (std::size_t i=0;i<g_received_detail.note_code_units;++i) note[i]=g_received_detail.note[i];
        SetNote(service,note.data(),g_received_detail.note_code_units);
        SetComposerEditable(false);RefreshComposerSummary();
        const bool pending=g_received_detail.state==GiftState::pending;
        const bool returned=g_received_detail.direction==ListDirection::returned;
        SetComposerPrimary(returned ? L"取回":pending ? L"接受":L"已接收",(pending||returned)&&!g_model.pending());
        g_ui.SetState(g_composer_cancel,true,!g_model.pending());
        g_ui.SetText(g_composer_cancel,pending ? L"退还":L"关闭");
        std::wstring dates=L"赠送时间："+FormatUnixMillis(g_received_detail.offered_unix_millis);
        if (g_received_detail.accepted_unix_millis)
            dates+=L"    接收时间："+FormatUnixMillis(g_received_detail.accepted_unix_millis);
        SetComposerNotice(dates);
        return;
    }
    g_populate_after_inventory=false;
    RefreshComposerSummary();
    g_ui.SetState(g_composer_note,true,g_ui.SetNoteLimit(g_composer_note));
    g_ui.SetState(g_composer_cancel,true,true);
    g_ui.SetText(g_composer_cancel,L"取消");
    SetComposerPrimary(L"服务器预检",g_selected_unit.uid && !g_inventory_sync.Busy());
    SetComposerNotice(L"核对机体与寄语后进行预检，再确认赠送。");
}

void ForgetComposer() {
    g_composer = nullptr;g_composer_received=false;g_received_detail={};
    g_composer_opening=false;g_composer_reload_uid=0;g_composer_card_uid=0;
    g_composer_controls=false;g_composer_note=nullptr;

    g_composer_primary=nullptr;g_composer_cancel=nullptr;g_composer_notice=nullptr;
    g_units.clear();
    g_selected_unit = {};
    g_selection_detail_uid = 0;
    g_model.CancelOffer();
}

void __fastcall ComposerDtorHook(void* service,void*) {
    if (service==g_composer) ForgetComposer();
    g_unit_card_dtor(service);
}

void CloseComposer() {
    auto* service=g_composer;
    ForgetComposer();
    if (service && g_close) g_close(service,1);
}

bool OpenReceivedInfo(const Detail& detail) {
    if (!detail.present || !detail.unit_number || !detail.unit_uid) return false;
    if (g_composer) {if (!g_composer_received) return false;CloseComposer();}
    UnitSnapshot view;
    if (!FindOwnedUnit(detail.unit_uid,&view)) {
        view.uid=detail.unit_uid;view.unit_number=detail.unit_number;view.level=static_cast<std::uint8_t>(detail.level);
        Put<std::uint32_t>(view.full_record.data(),0,detail.unit_number);
        Put<std::uint64_t>(view.full_record.data(),8,detail.unit_uid);
        Put<std::uint8_t>(view.full_record.data(),16,view.level);
    }
    // The native brief can still carry level zero after an inventory refresh.
    // Gift details identify this instance and contain its authoritative level.
    view.level=static_cast<std::uint8_t>(detail.level);
    auto* temporary=g_alloc(0x148);if (!temporary) return false;
    g_unit_card_ctor(temporary,view.full_record.data(),0);
    auto* live=g_enqueue(temporary,1);g_unit_card_dtor(temporary);g_free(temporary);
    if (!live) return false;
    g_composer=live;g_composer_received=true;g_received_detail=detail;g_selected_unit=view;
    g_receiver_name.assign(detail.peer_name.begin(),detail.peer_name.begin()+detail.peer_name_code_units);
    g_composer_controls=false;g_composer_opening=false;g_composer_card_uid=detail.unit_uid;
    g_composer_primary=nullptr;g_composer_cancel=nullptr;g_composer_note=nullptr;g_composer_notice=nullptr;
    g_composer_saved_note_size=0;g_composer_phase=ComposerPhase::selecting;
    return true;
}

void* FindServiceByName(const wchar_t* name) {
    if (!name || !g_service_manager ||
        !g_find_service || !g_wstring_ctor || !g_wstring_dtor) {
        return nullptr;
    }
    auto* manager = *g_service_manager;
    if (!manager) return nullptr;
    alignas(4) std::uint8_t descriptor[0x20]{};
    Put<std::uint32_t>(descriptor, 0, 1);
    bool constructed = false;
    void* service = nullptr;
    __try {
        g_wstring_ctor(descriptor + 4, name);
        constructed = true;
        service = g_find_service(manager, descriptor);
    } __finally {
        if (constructed) g_wstring_dtor(descriptor + 4);
    }
    return service;
}

void* ResolveService(
    const wchar_t* name,
    const void* expectedVtable) {
    if (!expectedVtable) return nullptr;
    auto* service = FindServiceByName(name);
    if (!service || Read<void*>(service) != expectedVtable) {
        return nullptr;
    }
    return service;
}

void RefreshNativeInventoryView() {
    if (!g_inventory_view || !g_inventory_redraw || !g_inventory_page) return;
    auto* service = ResolveService(L"UnitIndexingCubbyhole", g_inventory_service_vtable);
    if (!service) return;
    auto* panel = Read<void*>(service, 0x44);
    if (!panel || !Read<std::uint8_t>(panel, 0x162)) return;
    g_inventory_view(service);
    g_inventory_redraw(service);
    g_inventory_page(service);
}

void* UnitDetailSkinSource() {
    auto* service =
        FindServiceByName(L"UnitIndexingCubbyhole");
    if (!service) return nullptr;
    auto* root = Read<void*>(service, 0x44);
    auto* source = Read<void*>(service, 0x4C);
    if (!root || !source ||
        Read<void*>(source) != g_ui.button_vtable ||
        Read<void*>(source, 0x15C) != root ||
        g_ui.FindNamed(root, L"UnitDeleteBtn", 4) != source) {
        return nullptr;
    }
    return source;
}

void* FindFriendRecord(std::uint32_t receiver) {
    if (!receiver || !g_player_data || !*g_player_data) return nullptr;
    auto* data = *g_player_data;
    auto* head = Read<std::uint8_t*>(data, 0x7A4);
    const auto count = Read<std::uint32_t>(data, 0x7A8);
    if (!head || count > 100) return nullptr;
    auto* node = Read<std::uint8_t*>(head);
    for (std::uint32_t index = 0;
         index < count && node && node != head;
         ++index) {
        auto* record = Read<std::uint8_t*>(node, 8);
        if (record && Read<std::uint32_t>(record) == receiver) {
            return record;
        }
        node = Read<std::uint8_t*>(node);
    }
    return nullptr;
}

bool OpenFriendContextFor(std::uint32_t receiver) {
    auto* service = ResolveService(
        L"Messenger", g_messenger_service_vtable);
    if (!service) return false;
    auto* messenger = ResolveService(
        L"MessengerPopup", g_messenger_vtable);
    if (!messenger) {
        g_open_messenger(service);
        messenger = ResolveService(
            L"MessengerPopup", g_messenger_vtable);
    }
    if (!messenger ||
        Read<void*>(service, 0x48) != messenger) {
        return false;
    }
    auto* record = FindFriendRecord(receiver);
    if (!record) return false;
    auto* treeRoot = Read<void*>(messenger, 0x50);
    auto* tree = g_ui.FindNamed(treeRoot, L"FriendTree", 0x12);
    if (!tree) return false;
    auto* nativeRoot = Read<void*>(tree, 0x21C);
    auto* item = nativeRoot
                     ? g_tree_find(nativeRoot, record)
                     : nullptr;
    if (!item || g_tree_data(item) != record) return false;
    g_tree_select(tree, item);
    g_open_friend_context(messenger);
    return true;
}

bool OpenHomeFriendContext(std::uint32_t receiver) {
    auto* community=ResolveService(L"ServiceCommunity",g_home_community_vtable);
    if (!community) {
        if (g_home_context_opened) return false;
        auto* header=ResolveService(L"upmenu",g_home_upmenu_vtable);
        if (!header) return false;
        using Dialog=void*(__thiscall*)(void*,const ui::NativeString*);
        const auto name=NativeView(L"dlg");
        auto get=reinterpret_cast<Dialog>(Read<void*>(Read<void*>(header),0x5C));
        auto* root=get(header,&name);
        auto* button=g_ui.FindNamed(root,L"MessengerBtn",4);
        if (!ControlReady(button)) return false;
        g_home_upmenu_callback(header,0x101,reinterpret_cast<std::uintptr_t>(static_cast<std::uint8_t*>(button)+0x118),button);
        g_home_context_opened=true;
        return false;
    }
    if (Read<std::uint32_t>(community,0x44)!=3) return false;
    if (Read<std::uint32_t>(community,0x50)!=2) {
        using SelectTab=void(__thiscall*)(void*,std::uint32_t);
        auto select=reinterpret_cast<SelectTab>(Read<void*>(Read<void*>(community),0x78));
        select(community,2);
        return false;
    }
    auto* first=Read<std::uint8_t*>(community,0x78);
    auto* last=Read<std::uint8_t*>(community,0x7C);
    if (!first || !last || last<first || last-first>8*0x38 || (last-first)%0x38) return false;
    for (unsigned index=0;first+index*0x38<last;++index) {
        if (Read<std::uint32_t>(first+index*0x38)!=receiver) continue;
        auto* control=Read<void*>(community,0x90+index*4);
        if (!ControlReady(control)) return false;
        using RightClick=void(__thiscall*)(void*,void*);
        auto right=reinterpret_cast<RightClick>(g_api->Symbol(g_mod,"friend_unit_gift.community_right_click"));
        if (!right) return false;
        right(community,control);
        return true;
    }
    return false;
}

bool CaptureFriendAndOpen(void* service) {
    auto* record = Read<std::uint8_t*>(service, 0x50);
    if (!record) return false;
    const auto receiver = Read<std::uint32_t>(record);
    if (!receiver) return false;
    char nickname[17]{};
    std::memcpy(nickname, record + 0x0A, 16);
    wchar_t wide[33]{};
    const int count = MultiByteToWideChar(
        CP_ACP, 0, nickname, -1, wide,
        static_cast<int>(std::size(wide)));
    if (count <= 0) return false;
    g_receiver_id = receiver;
    g_receiver_name.assign(wide);
    return OpenComposer();
}

void __fastcall FriendMainInit(
    void* service,
    void*,
    std::uint32_t argument) {
    g_friend_main_init(service, argument);
    __try {
        ArmGameThread();
        const bool attached = g_friend_main.Attach(service, g_ui);
        if (!attached) {
            auto* root = Read<void*>(service,0x4C);
            auto* source = root ? g_ui.FindNamed(root,L"Btn5",4) : nullptr;
            g_api->Log(g_mod,SDGO_LOG_WARN,
                "friend gift context attachment failed: reason=%s root=%p source=%p width=%d height=%d skins_begin=%p skins_end=%p",
                g_ui.last_error,root,source,source ? Read<int>(source,0x154) : 0,
                source ? Read<int>(source,0x158) : 0,
                source ? Read<void*>(source,0x1A4) : nullptr,
                source ? Read<void*>(source,0x1A8) : nullptr);
        }
        RefreshAvailability();
        if (attached) {
            auto* root=Read<void*>(service,0x4C);
            g_friend_context_x=static_cast<std::int32_t>(Read<float>(root,0x148));
            g_friend_context_y=static_cast<std::int32_t>(Read<float>(root,0x14C));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_api->Log(g_mod,SDGO_LOG_WARN,"friend gift context attachment raised a native access fault");
        g_friend_main.Detach(service);
    }
}

void __fastcall FriendThreeDInit(
    void* service,
    void*,
    std::uint32_t argument) {
    g_friend_three_d_init(service, argument);
    __try {
        ArmGameThread();
        const bool attached = g_friend_three_d.Attach(service, g_ui);
        if (!attached) {
            auto* root = Read<void*>(service,0x4C);
            auto* source = root ? g_ui.FindNamed(root,L"Btn5",4) : nullptr;
            g_api->Log(g_mod,SDGO_LOG_WARN,
                "3D friend gift context attachment failed: reason=%s root=%p source=%p width=%d height=%d skins_begin=%p skins_end=%p",
                g_ui.last_error,root,source,source ? Read<int>(source,0x154) : 0,
                source ? Read<int>(source,0x158) : 0,
                source ? Read<void*>(source,0x1A4) : nullptr,
                source ? Read<void*>(source,0x1A8) : nullptr);
        }
        RefreshAvailability();
        if (attached) {
            auto* root=Read<void*>(service,0x4C);
            g_friend_context_x=static_cast<std::int32_t>(Read<float>(root,0x148));
            g_friend_context_y=static_cast<std::int32_t>(Read<float>(root,0x14C));
            auto* bottom=g_ui.FindNamed(root,L"Icon1",1);
            auto* bg=g_ui.FindNamed(root,L"ModFriendUnitGiftBackground",1);
            auto* source=g_ui.FindNamed(root,L"Btn5",4);
            auto* button=g_ui.FindNamed(root,L"ModFriendUnitGift",4);
            g_api->Log(g_mod,SDGO_LOG_INFO,
                "3D friend gift menu geometry: root=%p x=%g y=%g height=%d bottom=%p bottom_y=%g background=%p background_y=%g source_y=%g button_y=%g",
                root,static_cast<double>(Read<float>(root,0x148)),
                static_cast<double>(Read<float>(root,0x14C)),Read<int>(root,0x158),
                bottom,bottom ? static_cast<double>(Read<float>(bottom,0x14C)) : -1.0,
                bg,bg ? static_cast<double>(Read<float>(bg,0x14C)) : -1.0,
                source ? static_cast<double>(Read<float>(source,0x14C)) : -1.0,
                button ? static_cast<double>(Read<float>(button,0x14C)) : -1.0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_api->Log(g_mod,SDGO_LOG_WARN,"3D friend gift context attachment raised a native access fault");
        g_friend_three_d.Detach(service);
    }
}

void __stdcall FriendEvent(
    void* service,
    std::uint32_t event,
    std::uintptr_t second,
    void* control) {
    if (event == 0x101 &&
        (g_friend_main.Click(service, control) ||
         g_friend_three_d.Click(service, control))) {
        __try {
            if (CaptureFriendAndOpen(service)) {
                g_friend_main.Detach(service);
                g_friend_three_d.Detach(service);
                if (g_close) g_close(service,1);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    g_friend_callback(service, event, second, control);
}

void __fastcall CommunityFriendInit(void* service, void*, std::uint32_t argument) {
    g_friend_community_init(service,argument);
    __try {
        ArmGameThread();
        if (g_friend_community.service() && g_friend_community.service()!=service)
            g_friend_community.Detach(g_friend_community.service());
        const bool eligible=Read<std::uint32_t>(service,0x64)==2 &&
            Read<std::uint32_t>(service,0x8C)==7 &&
            Read<std::uint32_t>(service,0x84)!=0;
        g_community_friend=false;
        if (!g_friend_community.AttachCommunity(service,g_ui))
            g_api->Log(g_mod,SDGO_LOG_WARN,"home friend gift attachment failed: %s",g_ui.last_error);
        else g_community_friend=eligible;
        RefreshAvailability();
        auto* first=Read<void*>(service,0x4C);
        auto* root=first ? Read<void*>(first,0x15C):nullptr;
        if (root) {
            g_friend_context_x=static_cast<std::int32_t>(Read<float>(root,0x148));
            g_friend_context_y=static_cast<std::int32_t>(Read<float>(root,0x14C));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_friend_community.Detach(service);g_community_friend=false;
    }
}

void* __fastcall CommunityFriendDtor(void* service, void*, std::uint32_t flags) {
    if (g_friend_community.service()==service) {
        g_friend_community.Detach(service);g_community_friend=false;
    }
    return g_friend_community_dtor(service,flags);
}

std::uint32_t __stdcall CommunityFriendEvent(void* service,std::uint32_t event,std::uintptr_t name,void* control) {
    if (event==0x101 && g_friend_community.Click(service,control)) {
        __try {
            if (g_community_friend && Read<std::uint32_t>(service,0x64)==2 &&
                Read<std::uint32_t>(service,0x8C)==7) {
                const auto* text=reinterpret_cast<const ui::NativeString*>(static_cast<const std::uint8_t*>(service)+0x68);
                if (text->length && text->length<=20 && text->capacity>=text->length) {
                    g_receiver_id=Read<std::uint32_t>(service,0x84);
                    g_receiver_name.assign(text->capacity<8?text->data.small:text->data.pointer,text->length);
                    if (g_receiver_id && OpenComposer()) {
                        g_friend_community.Detach(service);g_close(service,1);
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 0;
    }
    return g_friend_community_callback(service,event,name,control);
}

void __fastcall UnitDetailInitHook(
    void* service,
    void*,
    std::uint32_t argument) {
    runtime::ForwardUnitDetailInit(
        g_unit_detail_init, service, argument);
    __try {
        ArmGameThread();
        if (Read<void*>(service) !=
            g_unit_detail_service_vtable) {
            return;
        }
        auto* root = Read<void*>(service, 0x44);
        if (!g_unit_detail_view.Attach(
                service,
                root,
                UnitDetailSkinSource(),
                g_ui)) {
            g_unit_detail_view.Detach(service);
            return;
        }
        RefreshUnitDetailView();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_unit_detail_view.Detach(service);
    }
}
static_assert(
    std::is_same_v<
        decltype(&UnitDetailInitHook),
        runtime::UnitDetailInitHook>,
    "UnitDetailInit hook must preserve the native ret-4 argument");

void __fastcall UnitDetailRefreshHook(
    void* service,
    void*) {
    g_unit_detail_refresh(service);
    if (service != g_unit_detail_view.service()) return;
    RefreshUnitDetailView();
}

void UnitDetailEventUnsafe(
    void* service,
    void* control) {
    if (!g_unit_detail_view.Click(
            service, control)) {
        return;
    }
    const auto unitUid =
        g_unit_detail_view.unit_uid();
    const auto& cached = g_model.provenance();
    if (cached.present &&
        cached.unit_uid == unitUid) {
        g_unit_detail_view.ToggleDetail();
        RefreshUnitDetailView();
        return;
    }
    if (!BeginUnitDetailProvenance(unitUid)) {
        g_unit_detail_view.SetUnit(
            unitUid,
            g_model.CanReadProvenance(),
            g_model.CanReadProvenance() &&
                g_transport.Ready() &&
                !g_model.pending(),
            StatusText(g_model.last_status()));
    }
}

void __stdcall UnitDetailEventHook(
    void* service,
    std::uint32_t event,
    void* eventData,
    std::uintptr_t identifier) {
    g_unit_detail_event(
        service, event, eventData, identifier);
    if (event != 0x101 || !eventData) return;
    __try {
        UnitDetailEventUnsafe(service, reinterpret_cast<void*>(identifier));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall ComposerUpdateHook(void* service, void*) {
    g_composer_update(service);
    if (service != g_composer) return;
    if (Read<std::uint32_t>(service,0x44)<3 || !Read<void*>(service,0x6C)) return;
    if (!g_composer_controls) {
        if (!AttachComposerControls(service)) { CloseComposer();return; }
        PopulateComposer(service);
    }
    RefreshComposerSummary();
}

void __stdcall ComposerEvent(
    void* service,
    std::uint32_t event,
    std::uintptr_t second,
    void* control) {
    if (service != g_composer) {
        g_composer_callback(service, event, second, control);
        return;
    }
    if (g_composer_received && event==0x101) {
        if (control==Read<void*>(service,0x70)) {CloseComposer();return;}
        g_selected_gift_id=g_received_detail.gift_id;
        if (control==g_composer_primary) {
            if (g_composer_phase==ComposerPhase::outcome_unknown) {BeginReceiptQuery();return;}
            if (BeginGiftAction(g_received_detail.direction==ListDirection::returned ? Command::takeback:Command::accept))
                SetComposerPrimary(L"正在处理…",false);
            return;
        }
        if (control==g_composer_cancel) {
            if (g_received_detail.state==GiftState::pending) {
                if (BeginGiftAction(Command::return_to_sender)) SetComposerPrimary(L"正在退还…",false);
            } else CloseComposer();
            return;
        }
    }
    if (event == 0x101 &&
        (control == g_composer_cancel || control == Read<void*>(service,0x70))) {
        CloseComposer();
        return;
    }
    if (event == 0x101 && control == g_composer_primary) {
        if (g_composer_phase == ComposerPhase::complete &&
            g_inventory_sync.error() != NativeInventorySync::Error::none) {
            g_inventory_sync.Retry();
            SetComposerPrimary(L"正在同步机库…", false);
            SetComposerNotice(L"正在重新读取机体数据；不会再次提交赠送。");
            return;
        }
        if (g_composer_phase == ComposerPhase::selecting) {
            std::array<char16_t, kMaximumNoteCodeUnits> note{};
            std::uint16_t noteCodeUnits = 0;
            if (!SelectComposerUnit(0) || g_selection_detail_uid ||
                g_returns_needed || g_returns_active || g_returns_next_page ||
                g_inventory_sync.Busy() || !g_inventory_sync.HasFull(g_selected_unit.uid) ||
                !ReadNote(service, &note, &noteCodeUnits) ||
                !g_model.PrepareOffer(
                    g_receiver_id,
                    g_selected_unit,
                    note.data(),
                    noteCodeUnits)) {
                SetComposerNotice(
                    L"请选择仍然可用的机体，并检查留言格式。");
                return;
            }
            const auto preview =
                g_model.PreviewOffer(NewRequestId());
            if (!preview || !BeginRequest(*preview)) {
                g_model.CancelOffer();
                SetComposerNotice(StatusText(g_model.last_status()));
                return;
            }
            g_composer_phase = ComposerPhase::previewing;
            SetComposerEditable(false);
            SetComposerPrimary(L"正在预检…", false);
            SetComposerNotice(
                L"正在由服务器核对好友关系、所有权和接收容量。");
            return;
        }
        if (g_composer_phase == ComposerPhase::confirming) {
            UnitSnapshot current;
            if (!CurrentUnit(g_selected_unit.slot, &current)) {
                g_model.CancelOffer();
                g_composer_phase = ComposerPhase::selecting;
                SetComposerEditable(true);
                SetComposerPrimary(L"服务器预检", true);
                SetComposerNotice(
                    L"所选机体已变化，请重新选择并预检。");
                return;
            }
            const auto offer =
                g_model.ConfirmOffer(current, NewRequestId());
            if (!offer || !BeginRequest(*offer)) {
                g_composer_phase =
                    g_transport.MutationUncertain()
                        ? ComposerPhase::outcome_unknown
                        : ComposerPhase::selecting;
                SetComposerPrimary(
                    g_transport.MutationUncertain()
                        ? L"查询原请求结果"
                        : L"服务器预检",
                    g_transport.MutationUncertain()
                        ? g_transport.Ready()
                        : true);
                SetComposerNotice(
                    g_transport.MutationUncertain()
                        ? StatusText(WireStatus::outcome_unknown)
                        : L"所选机体已变化，请重新选择并预检。");
                return;
            }
            g_composer_phase = ComposerPhase::sending;
            SetComposerPrimary(L"正在提交…", false);
            SetComposerNotice(
                L"已提交一次性请求；断线或超时不会自动重发。");
            return;
        }
        if (g_composer_phase == ComposerPhase::outcome_unknown) {
            if (!BeginReceiptQuery()) {
                SetComposerNotice(
                    L"当前还不能查询，请保持此请求不变并等待重新连接。");
            }
            return;
        }
        return;
    }
    g_composer_callback(service,event,second,control);
}

int __cdecl StoragePacketPrefilter(
    std::uint16_t opcode,
    const std::uint8_t* packet,
    std::uint32_t fixedSize,
    void*) {
    return g_transport.OnStoragePacket(
               opcode, packet, fixedSize)
               ? 1
               : 0;
}

void __cdecl StoragePacketPostfilter(
    void* service,
    std::uint16_t,
    const std::uint8_t*,
    std::uint32_t,
    void*) {
    if (service == g_storage_view.service()) {
        if (g_storage_view.custom_view()) RenderStorageRows();
    }
}

void __cdecl StoragePageAttach(
    void* service,
    void* root,
    void*) {
    __try {
        ArmGameThread();
        g_storage_view.Attach(service, root, g_ui);
        RefreshAvailability();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_storage_view.Detach(service);
    }
}

void __cdecl StoragePageDetach(void* service, void*) {
    g_storage_view.Detach(service);
    g_model.LeaveCustomStorageView();
    g_selected_gift_id = 0;
    g_storage_local_page = 0;
}

int __cdecl StorageUiEvent(
    void*,
    void* service,
    std::uint32_t event,
    std::uintptr_t,
    void* control,
    std::uint32_t* result,
    void*) {
    if (event!=0x101 && event!=0x301 && event!=0x801 && event!=0x803 && event!=0x303) return 0;
    const auto action =
        g_storage_view.ActionFor(service, control);
    if (action == ui::StorageView::Action::none) return 0;
    if (result) *result = 0;

    if (action == ui::StorageView::Action::toggle &&
        g_storage_view.custom_view()) {
        g_model.LeaveCustomStorageView();
        g_storage_view.ShowStatus(L"", false);
        g_selected_gift_id = 0;
        return 1;
    }
    if (action == ui::StorageView::Action::toggle) {
        if (!BeginStorageList(Command::list_inbox, 1)) {
            g_storage_view.ShowStatus(
                StatusText(g_model.last_status()), true);
        }
        return 1;
    }
    if (action == ui::StorageView::Action::open_inbox) {
        BeginStorageList(Command::list_inbox,1);return 1;
    }
    if (action == ui::StorageView::Action::open_returned) {
        if (!BeginStorageList(Command::list_returned, 1)) {
            g_storage_view.ShowStatus(
                StatusText(g_model.last_status()), true);
        }
        return 1;
    }

    const auto rowGift=g_storage_view.GiftForControl(control);
    if (rowGift) g_selected_gift_id=rowGift;
    const auto gift = g_storage_view.GiftFor(action);
    if (gift) {
        g_selected_gift_id = gift;
        RenderStorageRows();
        return 1;
    }
    if (action == ui::StorageView::Action::previous_page) {
        if (g_storage_local_page > 0) {
            --g_storage_local_page;
            g_selected_gift_id = 0;
            RenderStorageRows();
        } else if (g_model.page() > 1) {
            BeginStorageList(
                g_storage_list_command, g_model.page() - 1);
        }
        return 1;
    }
    if (action == ui::StorageView::Action::next_page) {
        const auto nextOffset =
            static_cast<std::size_t>(g_storage_local_page + 1u) *
            5u;
        if (nextOffset < g_model.row_count()) {
            ++g_storage_local_page;
            g_selected_gift_id = 0;
            RenderStorageRows();
        } else if (
            static_cast<std::uint64_t>(g_model.page()) *
                kMaximumRows <
            g_model.total_count()) {
            BeginStorageList(
                g_storage_list_command, g_model.page() + 1);
        }
        return 1;
    }
    if (action == ui::StorageView::Action::accept) {
        BeginGiftAction(Command::accept);
        return 1;
    }
    if (action == ui::StorageView::Action::return_to_sender) {
        BeginGiftAction(Command::return_to_sender);
        return 1;
    }
    if (action == ui::StorageView::Action::takeback) {
        BeginGiftAction(Command::takeback);
        return 1;
    }
    if (action == ui::StorageView::Action::provenance) {
        BeginGiftAction(Command::detail);
        return 1;
    }
    if (action == ui::StorageView::Action::receipt) {
        if (!BeginReceiptQuery()) {
            g_storage_view.ShowStatus(
                L"当前没有可查询的待确认请求。", true);
        }
        return 1;
    }
    return 1;
}

int __cdecl BlockClaimAll(void*, void*) {
    return g_model.BlockNativeClaimAll() ? 1 : 0;
}

void __fastcall StorageInitHook(void* service, void*) {
    if (g_storage_view.custom_view()) {
        g_model.LeaveCustomStorageView();
    }
    g_storage_view.Detach(service);
    g_storage_init(service);
    StoragePageAttach(service, Read<void*>(service, 0x70), nullptr);
}

std::uint32_t __fastcall StorageEventHook(
    void* handler,
    void*,
    void* service,
    std::uint32_t event,
    std::uintptr_t second,
    void* control) {
    std::uint32_t result = 0;
    if (StorageUiEvent(
            handler, service, event, second, control, &result, nullptr)) {
        return result;
    }
    return g_storage_event(
        handler, service, event, second, control);
}

std::uint32_t __fastcall StoragePacketHook(
    void* service,
    void*,
    std::uintptr_t a,
    std::uintptr_t b,
    std::uintptr_t c,
    std::uint32_t command,
    const std::uint8_t* data) {
    const auto opcode = static_cast<std::uint16_t>(command);
    const std::uint32_t fixedSize =
        opcode == kNativeReplyOpcode
            ? static_cast<std::uint32_t>(kCarrierBytes)
            : 0;
    if (StoragePacketPrefilter(
            opcode, data, fixedSize, nullptr)) {
        return 0;
    }
    const auto result = g_storage_packet(
        service, a, b, c, command, data);
    StoragePacketPostfilter(
        service, opcode, data, fixedSize, nullptr);
    return result;
}

void PumpInventorySync() {
        const auto session = CurrentSessionKey();
        if (session != g_returns_session) {
            g_returns_session=session;g_returns_needed=true;g_returns_active=false;
            g_returns_next_page=0;g_returns_retry_at=0;g_returns_poll_at=0;g_inventory_was_visible=false;
        }
        auto* inventory = ResolveService(L"UnitIndexingCubbyhole",g_inventory_service_vtable);
        auto* panel = inventory ? Read<void*>(inventory,0x44) : nullptr;
        const bool visible = panel && Read<std::uint8_t>(panel,0x162);
        const auto now=GetTickCount64();
        if (visible && !g_inventory_was_visible) {
            g_returns_needed=true;g_returns_poll_at=now+10000;
        }
        // A friend may return a unit while this same native inventory stays
        // open. Read the existing return feed while idle, without a relog or
        // competing with an active gift card, picker or source request.
        if (visible && !g_composer && !g_picker_selecting &&
            !g_storage_view.custom_view() && !g_provenance_target_uid &&
            now>=g_returns_poll_at) {
            g_returns_needed=true;g_returns_poll_at=now+10000;
        }
        g_inventory_was_visible=visible;
        if (g_transport.Ready() && g_model.CanResolveExisting() &&
            !g_model.pending() && !g_transport.MutationUncertain() &&
            !g_returns_active && (g_returns_needed || g_returns_next_page) &&
            GetTickCount64()>=g_returns_retry_at) {
            Request request;request.command=Command::inventory_returns;
            request.page=g_returns_next_page ? g_returns_next_page : 1;
            if (BeginRequest(request)) {
                g_returns_active=true;g_returns_needed=false;g_returns_next_page=0;
            }
        }
        g_inventory_sync.SetSession(CurrentSessionKey());
        g_inventory_sync.Tick(GetTickCount64());
        if (g_composer && g_selection_detail_uid && !g_inventory_sync.Busy() &&
            g_inventory_sync.error()==NativeInventorySync::Error::none) {
            const auto uid=g_selection_detail_uid;g_selection_detail_uid=0;
            SelectComposerUnit(uid);
        }
        if (g_composer && g_composer_phase==ComposerPhase::selecting &&
            g_populate_after_inventory && !g_returns_needed && !g_returns_active &&
            !g_returns_next_page && !g_inventory_sync.Busy()) {
            g_populate_after_inventory=false;PopulateComposer(g_composer);
        }
        if (g_composer && g_composer_phase == ComposerPhase::complete) {
            const bool failed = g_inventory_sync.error() != NativeInventorySync::Error::none;
            SetComposerPrimary(failed ? L"重试机库同步" :
                g_inventory_sync.Busy() ? L"正在同步机库…" : L"服务器已确认", failed);
            if (failed) SetComposerNotice(L"转移已确认，机库数据尚未同步。可重新读取；不会再次赠送。");
        }
}

void PumpNativeReplies() {
    std::array<std::uint8_t,kCarrierBytes> reply{};
    while(g_native_replies.Pop(&reply))
        g_transport.OnStoragePacket(kNativeReplyOpcode,reply.data(),static_cast<std::uint32_t>(reply.size()));
}

bool ControlReady(void* control);

void PumpOwnedUnitProvenance() {
    if (!g_provenance_target_uid) return;
    const auto now=GetTickCount64();
    if (CurrentSessionKey()!=g_provenance_target_session || now>g_provenance_target_deadline) {
        g_provenance_target_uid=0;return;
    }
    if (now<g_provenance_next_action || g_model.pending() || g_picker_selecting || g_composer ||
        !OwnBriefContains(g_provenance_target_uid)) return;
    auto* service=g_unit_detail_view.service();
    auto* button=g_unit_detail_view.button();
    if (service && g_unit_detail_view.unit_uid()==g_provenance_target_uid && ControlReady(button)) {
        UnitDetailEventHook(service,0x101,static_cast<std::uint8_t*>(button)+0x118,
            reinterpret_cast<std::uintptr_t>(button));
        if (g_model.pending()) g_provenance_target_uid=0;
        return;
    }
    auto* grid=ResolveService(L"UnitIndexingCubbyhole",g_inventory_service_vtable);
    auto* root=grid ? Read<void*>(grid,0x44):nullptr;
    if (!root || !Read<std::uint8_t>(root,0x162) || !g_unit_info || !*g_unit_info) return;
    for (std::size_t index=0;index<36;++index) {
        const auto slot=Read<std::uint16_t>(grid,0x7AC+index*2);
        if (!slot) continue;
        std::uint8_t brief[25]{};g_brief(*g_unit_info,brief,slot);
        if (Read<std::uint64_t>(brief,4)!=g_provenance_target_uid) continue;
        auto* icon=Read<void*>(grid,0xE0+index*4);
        if (!PickerControlReady(icon) || Read<void*>(icon,0x15C)!=root) return;
        // Native left-click selects the owned record and requests its details.
        g_management_callback(grid,0x801,reinterpret_cast<std::uintptr_t>(static_cast<std::uint8_t*>(icon)+0x118),icon);
        g_provenance_next_action=now+1000;return;
    }
    auto* pager=ResolveService(L"UnitIndexingPage",g_management_pager_vtable);
    if (!pager || !g_management_page_change) return;
    const int capacity=Read<int>(*g_unit_info,0xC);
    if (capacity<1 || capacity>1200) return;
    for (int slot=1;slot<=capacity;++slot) {
        std::uint8_t brief[25]{};g_brief(*g_unit_info,brief,static_cast<std::uint16_t>(slot));
        if (Read<std::uint64_t>(brief,4)!=g_provenance_target_uid) continue;
        const int page=(slot-1)/36,current=Read<int>(pager,0xDC),pages=Read<int>(pager,0xD4);
        if (page<0 || page>=pages || page==Read<int>(grid,0x838)) return;
        g_management_page_change(pager,page/10!=current/10 ? -1:page%10,
            page/10!=current/10 && page>current ? 1u:0u);
        g_provenance_next_action=now+500;return;
    }
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_game_thread_ready ||
        GetCurrentThreadId() != g_ui_thread ||
        g_tick_active) {
        return;
    }
    g_tick_active = true;
    __try {
        PumpNativeReplies();
        ApplyNativeDetails();
        g_transport.Tick();
        PumpInventorySync();
        PumpGiftPicker();
        PumpOwnedUnitProvenance();
        if (g_composer_opening && g_inventory_sync.HasFull(g_selected_unit.uid))
            CreateComposerCard(g_selected_unit.uid);
        if (g_composer_reload_uid && g_inventory_sync.HasFull(g_composer_reload_uid))
            CreateComposerCard(g_composer_reload_uid);
        if (g_unit_card_requested && g_inventory_sync.HasFull(g_unit_card_requested)) {
            const auto uid=g_unit_card_requested;g_unit_card_requested=0;ShowNativeUnitCard(uid);
        }
        if (g_transport.State() == ConnectionState::idle &&
            !g_transport.HasOutstanding()) {
            runtime::AuthenticatedContext context;
            auto* app = g_app ? g_app() : nullptr;
            if (runtime::ReadAuthenticatedContext(
                    app, g_profile, &context)) {
                const DWORD lastError = GetLastError();
                g_api->Log(
                    g_mod,
                    SDGO_LOG_INFO,
                    "native gift probe authenticated start "
                    "app_state=%u pilot=%u",
                    context.app_state,
                    context.pilot_id);
                SetLastError(lastError);
                g_transport.BeginProbe();
            }
        }
        RefreshAvailability();
        if (g_home_context_target && OpenHomeFriendContext(g_home_context_target))
            g_home_context_target=0;
    } __finally {
        g_tick_active = false;
    }
}

BOOL CALLBACK FindOwnedTopLevelWindow(HWND window, LPARAM parameter) {
    auto* found = reinterpret_cast<bool*>(parameter);
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process == GetCurrentProcessId() &&
        thread == GetCurrentThreadId() &&
        GetWindow(window, GW_OWNER) == nullptr) {
        *found = true;
        return FALSE;
    }
    return TRUE;
}

bool CurrentThreadOwnsProcessWindow() {
    bool found = false;
    EnumThreadWindows(
        GetCurrentThreadId(),
        &FindOwnedTopLevelWindow,
        reinterpret_cast<LPARAM>(&found));
    return found;
}

bool ArmGameThread() {
    const DWORD current = GetCurrentThreadId();
    if (g_game_thread_ready) return current == g_ui_thread;
    if (!CurrentThreadOwnsProcessWindow()) return false;
    g_ui_thread = current;
    g_timer = SetTimer(nullptr, 0, 250, &Tick);
    g_game_thread_ready = g_timer != 0;
    if (!g_game_thread_ready) {
        g_ui_thread = 0;
        g_api->Log(
            g_mod,
            SDGO_LOG_ERROR,
            "friend unit gift capability timer unavailable");
    }
    return g_game_thread_ready;
}

std::uint8_t* __cdecl GameAppHook() {
    auto* app = g_app();
    const DWORD nativeLastError = GetLastError();
    if (app && !g_game_thread_ready) ArmGameThread();
    SetLastError(nativeLastError);
    return app;
}

bool ControlReady(void* control) {
    if (!control) return false;
    __try {
        return Read<std::uint8_t>(control, 0x162) != 0 &&
               Read<std::uint8_t>(control, 0x163) != 0 &&
               Read<std::uint8_t>(control, 0x167) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeStorageAction(ui::StorageView::Action action) {
    auto* service = g_storage_view.service();
    auto* control = g_storage_view.ControlFor(action);
    if (!service || !ControlReady(control)) return false;
    std::uint32_t result = 0;
    const auto event=Read<std::uint32_t>(control,0x134)==12 ? 0x301u:0x101u;
    return StorageUiEvent(
               nullptr,
               service,
               event,
               0,
               control,
               &result,
               nullptr) != 0;
}

bool InvokeComposerButton(void* control) {
    if (!g_composer || !ControlReady(control)) return false;
    ComposerEvent(g_composer, 0x101, 0, control);
    return true;
}

bool InvokeOwnedUnitProvenance(std::uint64_t unitUid) {
    if (!unitUid ||
        g_picker_selecting || g_composer || !g_management_callback) return false;
    if (g_storage_view.custom_view() &&
        !InvokeStorageAction(ui::StorageView::Action::toggle)) return false;
    g_provenance_target_uid=unitUid;
    g_provenance_target_session=CurrentSessionKey();
    g_provenance_target_deadline=GetTickCount64()+90000;
    g_provenance_next_action=0;
    return true;
}

bool ExecuteObserverAction(
    const SdgoFriendUnitGiftClientActionRequestV1& request) {
    const auto runHash = SyncObserver();
    if (!runHash || request.run_hash != runHash ||
        request.action <=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_NONE ||
        request.action >
            SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_HOME_FRIEND_CONTEXT) {
        return false;
    }
    if (!g_game_thread_ready) {
        if (request.action !=
                SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_FRIEND_CONTEXT ||
            !ArmGameThread()) {
            return false;
        }
    }
    if (GetCurrentThreadId() != g_ui_thread) return false;

    bool accepted = false;
    switch (request.action) {
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_HOME_FRIEND_CONTEXT:
        if (request.receiver_id) {
            g_home_context_target=request.receiver_id;g_home_context_opened=false;accepted=true;
            if (OpenHomeFriendContext(request.receiver_id)) g_home_context_target=0;
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_UNIT_TEMPLATE:
        if (request.unit_uid && OwnBriefContains(request.unit_uid)) {
            if (g_inventory_sync.HasFull(request.unit_uid)) accepted=ShowNativeUnitCard(request.unit_uid);
            else {
                g_inventory_sync.SetSession(CurrentSessionKey());
                g_unit_card_requested=request.unit_uid;
                accepted=g_inventory_sync.QueueDetail(request.unit_uid);
            }
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_FRIEND_CONTEXT:
        accepted =
            request.receiver_id != 0 &&
            OpenFriendContextFor(request.receiver_id);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CLICK_GIFT: {
        ui::FriendEntry* entry = nullptr;
        if (g_friend_community.ready()) entry = &g_friend_community;
        else if (g_friend_main.ready()) entry = &g_friend_main;
        else if (g_friend_three_d.ready()) entry = &g_friend_three_d;
        if (entry && ControlReady(entry->button())) {
            if (entry==&g_friend_community)
                CommunityFriendEvent(entry->service(),0x101,0,entry->button());
            else FriendEvent(entry->service(), 0x101, 0, entry->button());
            accepted = g_composer != nullptr || g_composer_opening || g_picker_selecting;
        }
        break;
    }
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SELECT_UNIT:
        if (g_picker_selecting && request.unit_uid && OwnBriefContains(request.unit_uid)) {
            g_picker_target_uid=request.unit_uid;accepted=true;
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE:
        accepted =
            g_composer && !g_composer_received && g_composer_phase==ComposerPhase::selecting &&
            request.note_code_units <= kMaximumNoteCodeUnits &&
            SetNote(
                g_composer,
                request.note,
                request.note_code_units);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_PREVIEW:
        if (g_composer_phase == ComposerPhase::selecting) {
            accepted = InvokeComposerButton(
                g_composer_primary);
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CONFIRM:
        if (g_composer_phase == ComposerPhase::confirming) {
            accepted = InvokeComposerButton(
                g_composer_primary);
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CANCEL:
        accepted = InvokeComposerButton(
            g_composer ? (g_composer_received ? Read<void*>(g_composer,0x70):g_composer_cancel) : nullptr);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_STORAGE_VIEW:
        if (!g_storage_view.custom_view()) {
            accepted = InvokeStorageAction(
                ui::StorageView::Action::toggle);
        } else accepted=g_storage_view.ready();
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_ORDINARY_STORAGE:
        accepted = g_storage_view.custom_view()
                       ? InvokeStorageAction(ui::StorageView::Action::toggle)
                       : g_storage_view.ready();
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_REFRESH_INBOX:
        if (g_storage_view.custom_view()) {
            accepted = InvokeStorageAction(
                ui::StorageView::Action::toggle);
        } else {
            accepted = true;
        }
        if (accepted) {
            accepted = InvokeStorageAction(
                ui::StorageView::Action::toggle);
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_REFRESH_RETURNED:
        accepted = InvokeStorageAction(
            ui::StorageView::Action::open_returned);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SELECT_GIFT: {
        const auto action =
            g_storage_view.RowActionForGift(request.gift_id);
        accepted =
            action != ui::StorageView::Action::none &&
            InvokeStorageAction(action);
        break;
    }
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_ACCEPT:
        accepted=g_composer_received ?
            ((!request.gift_id || request.gift_id==g_received_detail.gift_id) && InvokeComposerButton(g_composer_primary)) :
            InvokeStorageAction(ui::StorageView::Action::accept);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_RETURN_TO_SENDER:
        accepted=g_composer_received ?
            ((!request.gift_id || request.gift_id==g_received_detail.gift_id) && InvokeComposerButton(g_composer_cancel)) :
            InvokeStorageAction(ui::StorageView::Action::return_to_sender);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_TAKEBACK:
        accepted=g_composer_received ?
            ((!request.gift_id || request.gift_id==g_received_detail.gift_id) && InvokeComposerButton(g_composer_primary)) :
            InvokeStorageAction(ui::StorageView::Action::takeback);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_PROVENANCE:
        accepted = request.unit_uid
                       ? InvokeOwnedUnitProvenance(
                             request.unit_uid)
                       : InvokeStorageAction(
                             ui::StorageView::Action::provenance);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_QUERY_RECEIPT:
        if (g_composer &&
            g_composer_phase == ComposerPhase::outcome_unknown) {
            accepted = InvokeComposerButton(
                g_composer_primary);
        } else {
            accepted = InvokeStorageAction(
                ui::StorageView::Action::receipt);
        }
        break;
    default:
        break;
    }
    if (accepted) {
        ++g_observer.action_sequence;
        g_observer.last_action = request.action;
    }
    return accepted;
}

bool RegisterStorageBroker() {
    const HMODULE module = GetModuleHandleW(L"storage_claim.dll");
    if (!module) return false;
    const auto procedure =
        GetProcAddress(module, SDGO_STORAGE_EXTENSION_API_EXPORT);
    SdgoStorageGetExtensionApiV1Fn getter = nullptr;
    static_assert(sizeof(getter) == sizeof(procedure));
    std::memcpy(&getter, &procedure, sizeof(getter));
    if (!getter) return false;
    g_storage_api = getter();
    if (!g_storage_api ||
        g_storage_api->size < sizeof(SdgoStorageExtensionApiV1) ||
        g_storage_api->version != SDGO_STORAGE_EXTENSION_API_VERSION ||
        !g_storage_api->register_sink) {
        return false;
    }
    static const SdgoStorageExtensionSinkV1 sink{
        sizeof(SdgoStorageExtensionSinkV1),
        SDGO_STORAGE_EXTENSION_SINK_VERSION,
        &g_transport,
        &StoragePacketPrefilter,
        &StoragePageAttach,
        &StoragePageDetach,
        &StorageUiEvent,
        &BlockClaimAll,
        &StoragePacketPostfilter};
    if (!g_storage_api->register_sink(&sink)) return false;
    g_storage_broker = true;
    return true;
}

bool InstallStorageFallback() {
    if (!g_api->HookCode(
            g_mod,
            "friend_unit_gift.storage_init",
            reinterpret_cast<void*>(&StorageInitHook),
            reinterpret_cast<void**>(&g_storage_init)) ||
        !g_api->HookCode(
            g_mod,
            "friend_unit_gift.storage_ui_event",
            reinterpret_cast<void*>(&StorageEventHook),
            reinterpret_cast<void**>(&g_storage_event)) ||
        !g_api->HookCode(
            g_mod,
            "friend_unit_gift.storage_packet",
            reinterpret_cast<void*>(&StoragePacketHook),
            reinterpret_cast<void**>(&g_storage_packet))) {
        return false;
    }
    g_storage_fallback = true;
    return true;
}

template <class T>
bool Bind(T& output, const char* name) {
    output = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return output != nullptr;
}

} // namespace

static int GetSnapshot(
    SdgoFriendUnitGiftClientSnapshotV1* output) {
    if (!output ||
        output->size < sizeof(SdgoFriendUnitGiftClientSnapshotV1) ||
        (output->version &&
         output->version !=
             SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION)) {
        return 0;
    }
    const auto runHash = SyncObserver();
    if (!runHash || !g_game_thread_ready ||
        GetCurrentThreadId() != g_ui_thread) {
        return 0;
    }
    SdgoFriendUnitGiftClientSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version =
        SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
    snapshot.run_hash = runHash;
    snapshot.process_id = GetCurrentProcessId();
    snapshot.ui_thread_id = g_ui_thread;
    snapshot.session = CurrentSessionKey();
    snapshot.transport_state =
        static_cast<std::uint32_t>(g_transport.State());
    snapshot.capabilities = g_transport.Capabilities();
    snapshot.server_epoch = g_transport.ServerEpoch();
    snapshot.last_status =
        static_cast<std::int32_t>(g_model.last_status());
    if (const auto& pending = g_model.pending_request(); pending) {
        snapshot.pending_command =
            static_cast<std::uint32_t>(pending->command);
        std::memcpy(
            snapshot.pending_request_id,
            pending->request_id.bytes.data(),
            pending->request_id.bytes.size());
    }
    snapshot.ready = g_transport.Ready() ? 1u : 0u;
    snapshot.mutation_uncertain =
        g_transport.MutationUncertain() ? 1u : 0u;
    snapshot.storage_broker = g_storage_broker ? 1u : 0u;
    snapshot.storage_fallback = g_storage_fallback ? 1u : 0u;
    snapshot.friend_context_x = g_friend_context_x;
    snapshot.friend_context_y = g_friend_context_y;
    if (g_friend_main.service() ||
        g_friend_three_d.service() || g_friend_community.service()) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_FRIEND_CONTEXT;
    }
    if ((g_friend_main.ready() &&
         ControlReady(g_friend_main.button())) ||
        (g_friend_three_d.ready() &&
         ControlReady(g_friend_three_d.button())) ||
        (g_friend_community.ready() && ControlReady(g_friend_community.button()))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_GIFT_BUTTON;
    }
    if (g_composer) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMPOSER;
        snapshot.composer_phase =
            static_cast<std::uint32_t>(g_composer_phase) + 1u;
        auto* root = Read<void*>(g_composer, 0x6C);
        auto* note = g_ui.FindNamed(root, L"DescEdit", 8);
        auto* primary = g_composer_primary;
        auto* cancel = g_composer_cancel;
        if (ControlReady(note)) {
            snapshot.control_flags |=
                SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_NOTE;
        }
        if (ControlReady(primary)) {
            snapshot.control_flags |=
                SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_PRIMARY;
        }
        if (ControlReady(cancel)) {
            snapshot.control_flags |=
                SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_CANCEL;
        }
    }
    if (g_storage_view.attached()) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_PAGE;
    }
    if (g_storage_view.ready() &&
        ControlReady(g_storage_view.button())) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_BUTTON;
    }
    const auto* visibleGift=g_model.row(static_cast<std::size_t>(g_storage_local_page)*5u);
    if (g_storage_view.custom_view() && visibleGift &&
        ControlReady(g_storage_view.ControlFor(g_storage_view.RowActionForGift(visibleGift->gift_id)))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ROWS;
    }
    if (SelectedGift() && !g_model.pending() &&
        !g_transport.MutationUncertain() && !g_inventory_sync.Busy() &&
        g_inventory_sync.error()==NativeInventorySync::Error::none &&
        ControlReady(g_storage_view.ControlFor(ui::StorageView::Action::provenance))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ACTION;
    }
    if ((SelectedGift() && g_model.CanReadProvenance()) ||
        (g_unit_detail_view.ready() &&
         ControlReady(g_unit_detail_view.button()))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_PROVENANCE;
    }
    if (g_picker_selecting) {
        auto* grid=ResolveService(L"UnitIndexingCubbyhole",g_inventory_service_vtable);
        auto* root=grid ? Read<void*>(grid,0x44):nullptr;
        if (root && Read<std::uint8_t>(root,0x162)) {
            snapshot.control_flags|=SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMBO;
            snapshot.composer_phase=SDGO_FRIEND_UNIT_GIFT_CLIENT_COMPOSER_SELECTING;
        }
    }
    snapshot.action_sequence = g_observer.action_sequence;
    snapshot.reply_sequence = g_observer.reply_sequence;
    snapshot.last_action = g_observer.last_action;
    snapshot.last_accepted_operation =
        g_observer.last_accepted_operation;
    snapshot.last_reply_command =
        g_observer.last_reply_command;
    snapshot.last_reply_status =
        static_cast<std::int32_t>(
            g_observer.last_reply_status);
    std::memcpy(
        snapshot.last_reply_request_id,
        g_observer.last_reply_request_id.bytes.data(),
        g_observer.last_reply_request_id.bytes.size());
    snapshot.last_reply_gift_id =
        g_observer.last_reply_gift_id;
    snapshot.last_reply_unit_uid =
        g_observer.last_reply_unit_uid;
    snapshot.selected_unit_number =
        g_selected_unit.unit_number;
    snapshot.selected_unit_slot = g_selected_unit.slot;
    snapshot.selected_unit_level = g_selected_unit.level;
    snapshot.selected_unit_uid = g_selected_unit.uid;
    snapshot.selected_gift_id = g_selected_gift_id;
    snapshot.page = g_model.page();
    snapshot.total_count = g_model.total_count();
    snapshot.row_count = std::min<std::uint32_t>(
        g_model.row_count(), kMaximumRows);
    for (std::size_t index = 0;
         index < snapshot.row_count;
         ++index) {
        const auto* row = g_model.row(index);
        if (!row) break;
        auto& target = snapshot.rows[index];
        target.gift_id = row->gift_id;
        target.unit_uid = row->unit_uid;
        target.offered_unix_millis =
            row->offered_unix_millis;
        target.peer_account = row->peer_account;
        target.unit_number = row->unit_number;
        target.level = row->level;
        target.state = static_cast<std::uint32_t>(row->state);
        target.direction =
            static_cast<std::uint32_t>(row->direction);
        target.peer_name_code_units =
            row->peer_name_code_units;
        for (std::size_t nameIndex = 0;
             nameIndex < row->peer_name_code_units &&
             nameIndex < std::size(target.peer_name);
             ++nameIndex) {
            target.peer_name[nameIndex] =
                static_cast<std::uint16_t>(
                    row->peer_name[nameIndex]);
        }
    }
    snapshot.inventory_sync_busy=g_inventory_sync.Busy()?1u:0u;
    snapshot.inventory_sync_error=static_cast<std::uint32_t>(g_inventory_sync.error());
    snapshot.inventory_last_added_uid=g_inventory_sync.last_added();
    snapshot.inventory_last_removed_uid=g_inventory_sync.last_removed();
    const auto& provenance = g_composer_received ? g_received_detail : g_model.provenance();
    std::wstring text;
    bool receivedDisplayed=false;
    if (provenance.present && g_composer && g_composer_received && g_composer_controls &&
        g_composer_card_uid==provenance.unit_uid) {
        auto* root=Read<void*>(g_composer,0x6C);
        std::wstring sender,dates;
        std::array<char16_t,kMaximumNoteCodeUnits> note{};
        std::uint16_t units=0;
        if (root && Read<std::uint8_t>(root,0x162) && g_composer_note &&
            Read<std::uint8_t>(g_composer_note,0x162) &&
            g_ui.ReadText(g_ui.FindNamed(root,L"TitleStatic",3),&sender) &&
            g_ui.ReadText(g_composer_notice,&dates) && ReadNote(g_composer,&note,&units)) {
            text=sender+L"\n"+dates+L"\n";
            for (std::size_t i=0;i<units;++i) text.push_back(static_cast<wchar_t>(note[i]));
            receivedDisplayed=true;
        }
    }
    const bool displayed = provenance.present && (receivedDisplayed ||
        ((g_unit_detail_view.unit_uid()==provenance.unit_uid && g_unit_detail_view.detail_visible() &&
          g_ui.ReadText(g_unit_detail_view.detail(),&text)) ||
         (g_storage_view.custom_view() && g_ui.ReadText(g_storage_view.status_control(),&text) &&
          text==ProvenanceText(provenance))));
    if (displayed) {
        snapshot.provenance_present = 1;
        const auto length = std::min<std::size_t>(
            text.size(),
            std::size(snapshot.provenance_text));
        snapshot.provenance_text_code_units =
            static_cast<std::uint16_t>(length);
        for (std::size_t index = 0; index < length; ++index) {
            snapshot.provenance_text[index] =
                static_cast<std::uint16_t>(text[index]);
        }
    }
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return 1;
}

extern "C" __declspec(dllexport)
int __cdecl SdgoFriendUnitGiftClientGetSnapshotV1(
    SdgoFriendUnitGiftClientSnapshotV1* output) {
    __try {
        return GetSnapshot(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport)
int __cdecl SdgoFriendUnitGiftClientActionV1(
    const SdgoFriendUnitGiftClientActionRequestV1* request) {
    if (!request ||
        request->size <
            sizeof(SdgoFriendUnitGiftClientActionRequestV1) ||
        request->version !=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION) {
        return 0;
    }
    __try {
        return ExecuteObserverAction(*request) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int HomeFriendsSnapshot(SdgoHomeFriendsSnapshotV1* output) {
        const auto hash=SyncObserver();if(!hash)return 0;
        SdgoHomeFriendsSnapshotV1 result{};result.size=sizeof(result);result.version=1;
        result.run_hash=hash;result.process_id=GetCurrentProcessId();
        auto* service=ResolveService(L"ServiceCommunity",g_home_community_vtable);
        if (service && Read<std::uint32_t>(service,0x44)==3) {
            result.open=1;result.mode=Read<std::uint32_t>(service,0x50);
            auto* first=Read<std::uint8_t*>(service,0x78);auto* last=Read<std::uint8_t*>(service,0x7C);
            if (result.mode==2 && first && last && last>=first && last-first<=8*0x38 && (last-first)%0x38==0) {
                result.row_count=static_cast<std::uint32_t>((last-first)/0x38);
                for (unsigned i=0;i<result.row_count;++i) {
                    auto& row=result.rows[i];const auto* data=first+i*0x38;
                    row.account=Read<std::uint32_t>(data);row.level=Read<std::uint16_t>(data,4);
                    row.step=Read<std::uint32_t>(data,6);row.presence=Read<std::uint32_t>(data,0x1C);
                    auto* icon=Read<void*>(service,0x130+i*4);
                    if (icon) {
                        row.rank_visible=Read<std::uint8_t>(icon,0x162);row.rank_draw=Read<std::uint8_t>(icon,0x163);
                        auto** begin=Read<void**>(icon,0x1A4);auto** end=Read<void**>(icon,0x1A8);
                        if (begin && end>begin && end-begin<16 && *begin) {
                            row.skin_first_frame=Read<std::uint32_t>(*begin,0x3C);
                            row.skin_last_frame=Read<std::uint32_t>(*begin,0x40);
                            auto* sprite=Read<void*>(*begin);if(sprite)row.sprite_frame=Read<std::uint16_t>(sprite,0x120);
                        }
                    }
                }
            }
        }
        if (g_friend_community.service()) {
            auto* context=g_friend_community.service();result.context_target=Read<std::uint32_t>(context,0x84);
            const wchar_t* expected[]={L"私聊",L"玩家资料",L"机体资料",L"发送信件",L"删除好友",L"屏蔽"};
            result.captions_chinese=1;
            for(unsigned i=0;i<6;++i){std::wstring text;g_ui.ReadText(Read<void*>(context,0x4C+i*4),&text);
                if(text!=expected[i] && !(i==4 && text==L"添加好友") && !(i==5 && text==L"取消屏蔽"))result.captions_chinese=0;}
        }
        *output=result;return 1;
}
extern "C" __declspec(dllexport) int __cdecl SdgoHomeFriendsGetSnapshotV1(SdgoHomeFriendsSnapshotV1* output) {
    if (!output || output->size!=sizeof(*output) || output->version!=1) return 0;
    __try {return HomeFriendsSnapshot(output);}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}

extern "C" __declspec(dllexport) int __cdecl SdgoGiftPickerGetSnapshotV1(SdgoGiftPickerSnapshotV1* output) {
    if (!output || output->size!=sizeof(*output) || output->version!=1) return 0;
    __try {
        SdgoGiftPickerSnapshotV1 result{};result.size=sizeof(result);result.version=1;
        result.run_hash=SyncObserver();result.process_id=GetCurrentProcessId();
        if (!result.run_hash) return 0;
        result.selecting=g_picker_selecting ? 1u:0u;
        auto* grid=ResolveService(L"UnitIndexingCubbyhole",g_inventory_service_vtable);
        auto* root=grid ? Read<void*>(grid,0x44):nullptr;
        auto* pager=ResolveService(L"UnitIndexingPage",g_management_pager_vtable);
        if (root && Read<std::uint8_t>(root,0x162) && pager) {
            result.open=1;result.grid_page=Read<std::uint32_t>(grid,0x838);
            result.pager_page=Read<std::uint32_t>(pager,0xDC);result.page_count=Read<std::uint32_t>(pager,0xD4);
            for (unsigned i=0;i<12;++i) {
                auto* button=Read<void*>(pager,i<10 ? 0x50+i*4 : (i==10?0x48:0x4C));
                if (PickerControlReady(button)) {
                    result.page_button_mask|=1u<<i;
                    const auto rect=Read<RECT>(button,0x138);
                    auto* parent=Read<void*>(button,0x15C);
                    result.page_x[i]=rect.left+(rect.right-rect.left)/2+(parent?static_cast<int>(Read<float>(parent,0x148)):0);
                    result.page_y[i]=rect.top+(rect.bottom-rect.top)/2+(parent?static_cast<int>(Read<float>(parent,0x14C)):0);
                }
            }
            for (unsigned i=0;i<36;++i) {
                result.slots[i]=Read<std::uint16_t>(grid,0x7AC+i*2);
                auto* icon=Read<void*>(grid,0xE0+i*4);
                if (icon) {
                    const auto rect=Read<RECT>(icon,0x138);auto* parent=Read<void*>(icon,0x15C);
                    result.unit_x[i]=rect.left+(rect.right-rect.left)/2+(parent?static_cast<int>(Read<float>(parent,0x148)):0);
                    result.unit_y[i]=rect.top+(rect.bottom-rect.top)/2+(parent?static_cast<int>(Read<float>(parent,0x14C)):0);
                }
                if (result.slots[i] && g_unit_info && *g_unit_info) {
                    std::uint8_t brief[25]{};g_brief(*g_unit_info,brief,result.slots[i]);result.unit_uids[i]=Read<std::uint64_t>(brief,4);
                }
            }
        }
        *output=result;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    g_ui.unit_name=&UnitDisplayName;
    g_ui.unit_icon=&UnitIconPath;
    g_ui.restore_storage=&RestoreNativeStorage;
    if (!api || !mod || !api->Symbol || !api->HookCode ||
        api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite) || !api->HookSite ||
        !api->Log) {
        return 0;
    }
    if (!g_transport.Bind(api, mod) ||
        !Bind(g_session, "storage.session") ||
        !g_ui.Bind(api, mod) ||
        !Bind(g_alloc, "friend_unit_gift.native_alloc") ||
        !Bind(g_free, "friend_unit_gift.native_free") ||
        !Bind(g_enqueue, "friend_unit_gift.enqueue_ui") ||
        !Bind(g_close, "friend_unit_gift.close_ui") ||
        !Bind(g_step, "friend_unit_gift.composer_step") ||
        !Bind(g_combo_clear, "friend_unit_gift.combo_clear") ||
        !Bind(g_item_ctor, "friend_unit_gift.combo_item_ctor") ||
        !Bind(g_combo_add, "friend_unit_gift.combo_add") ||
        !Bind(g_edit_get, "friend_unit_gift.edit_get_text") ||
        !Bind(g_edit_set, "friend_unit_gift.edit_set_text") ||
        !Bind(g_brief, "hangar.brief") ||
        !Bind(g_full, "hangar.full") ||
        !Bind(g_downmenu_callback,"flow.hangar_downmenu_callback") ||
        !Bind(g_operator_callback,"flow.operator_menu_callback") ||
        !Bind(g_downmenu_vtable,"flow.hangar_downmenu_vtable") ||
        !Bind(g_operator_vtable,"flow.operator_menu_vtable") ||
        !Bind(g_management_pager_vtable,"friend_unit_gift.management_pager_vtable") ||
        !Bind(g_management_page_change,"friend_unit_gift.management_page_change") ||
        !Bind(g_add_combo,"friend_unit_gift.ui_add_combo") ||
        !Bind(g_add_edit,"friend_unit_gift.ui_add_edit") ||
        !Bind(g_set_font,"friend_unit_gift.ui_set_font") ||
        !Bind(g_ui.clear_skins,"friend_unit_gift.ui_clear_skins") ||
        !Bind(g_ui.set_number,"friend_unit_gift.ui_set_number") ||
        !Bind(g_storage_labels,"friend_unit_gift.storage_labels") ||
        !Bind(g_storage_render,"friend_unit_gift.storage_render") ||
        !Bind(g_unit_card_ctor,"friend_unit_gift.native_card_ctor") ||
        !Bind(g_unit_card_dtor,"friend_unit_gift.native_card_dtor") ||
        !Bind(g_sync_full, "hangar.sync_full") ||
        !Bind(g_inventory_delete, "friend_unit_gift.inventory_delete") ||
        !Bind(g_inventory_view, "hangar.view_list") ||
        !Bind(g_inventory_redraw, "hangar.redraw") ||
        !Bind(g_inventory_page, "hangar.page_frame") ||
        !Bind(g_inventory_service_vtable, "flow.unit_management_vtable") ||
        !Bind(g_grid_data, "grid.data") ||
        !Bind(g_grid_find, "grid.find") ||
        !Bind(g_unit_info, "hangar.info_global") ||
        !Bind(g_profile, "hangar.profile_global") ||
        !Bind(g_service_manager, "flow.service_managed_pointer") ||
        !Bind(g_find_service, "flow.service_find_by_name") ||
        !Bind(g_wstring_ctor, "flow.native_wstring_ctor") ||
        !Bind(g_wstring_dtor, "flow.native_wstring_dtor") ||
        !Bind(g_player_data, "friends.player_data_pointer") ||
        !Bind(g_messenger_vtable, "friends.messenger_vtable") ||
        !Bind(
            g_messenger_service_vtable,
            "friends.messenger_service_vtable") ||
        !Bind(g_open_messenger, "friends.open_messenger") ||
        !Bind(
            g_open_friend_context,
            "friend_unit_gift.open_friend_context") ||
        !Bind(g_tree_find, "friends.tree_find_by_data") ||
        !Bind(g_tree_data, "friends.tree_item_data") ||
        !Bind(g_tree_select, "friends.tree_select") ||
        !Bind(
            g_unit_detail_service_vtable,
            "friend_unit_gift.unit_detail_service_vtable")) {
        return 0;
    }
    if (!g_inventory_sync.Bind({g_unit_info, g_brief, g_full,
            g_inventory_delete, &RequestNativeDetail, &NativeInventoryChanged, nullptr}))
        return 0;
    g_transport.SetSink(&OnTransport, nullptr);
    if(!api->HookSite(mod,"friend_unit_gift.receive_decoded_site",
            reinterpret_cast<void*>(&ReceiveDecodedSite),&g_receive_decoded_original))return 0;

    if (!api->HookCode(
            mod,
            "friend_unit_gift.game_app",
            reinterpret_cast<void*>(&GameAppHook),
            reinterpret_cast<void**>(&g_app)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.friend_main_init",
            reinterpret_cast<void*>(&FriendMainInit),
            reinterpret_cast<void**>(&g_friend_main_init)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.friend_three_d_init",
            reinterpret_cast<void*>(&FriendThreeDInit),
            reinterpret_cast<void**>(&g_friend_three_d_init)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.friend_callback",
            reinterpret_cast<void*>(&FriendEvent),
            reinterpret_cast<void**>(&g_friend_callback)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.unit_detail_init",
            reinterpret_cast<void*>(&UnitDetailInitHook),
            reinterpret_cast<void**>(&g_unit_detail_init)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.unit_detail_event",
            reinterpret_cast<void*>(&UnitDetailEventHook),
            reinterpret_cast<void**>(&g_unit_detail_event)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.unit_detail_refresh",
            reinterpret_cast<void*>(&UnitDetailRefreshHook),
            reinterpret_cast<void**>(&g_unit_detail_refresh)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.native_card_update",
            reinterpret_cast<void*>(&ComposerUpdateHook),
            reinterpret_cast<void**>(&g_composer_update)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.native_card_callback",
            reinterpret_cast<void*>(&ComposerEvent),
            reinterpret_cast<void**>(&g_composer_callback))) {
        return 0;
    }

    if (!api->HookCode(mod,"friend_unit_gift.native_card_dtor",
            reinterpret_cast<void*>(&ComposerDtorHook),
            reinterpret_cast<void**>(&g_unit_card_dtor))) return 0;

    if (!api->HookCode(mod,"friend_unit_gift.management_callback",
            reinterpret_cast<void*>(&ManagementEventHook),
            reinterpret_cast<void**>(&g_management_callback))) return 0;
    if (!api->HookCode(mod,"friend_unit_gift.management_pager_callback",
            reinterpret_cast<void*>(&ManagementPagerEventHook),
            reinterpret_cast<void**>(&g_management_pager_callback))) return 0;

    if (!api->HookCode(mod,"friend_unit_gift.community_init",reinterpret_cast<void*>(&CommunityFriendInit),
            reinterpret_cast<void**>(&g_friend_community_init)) ||
        !api->HookCode(mod,"friend_unit_gift.community_dtor",reinterpret_cast<void*>(&CommunityFriendDtor),
            reinterpret_cast<void**>(&g_friend_community_dtor)) ||
        !api->HookCode(mod,"friend_unit_gift.community_event",reinterpret_cast<void*>(&CommunityFriendEvent),
            reinterpret_cast<void**>(&g_friend_community_callback))) return 0;

    if (!Bind(g_home_upmenu_callback,"friend_unit_gift.home_upmenu_callback") ||
        !Bind(g_home_upmenu_vtable,"friend_unit_gift.home_upmenu_vtable") ||
        !Bind(g_home_community_vtable,"friend_unit_gift.home_community_vtable")) return 0;

    if (!RegisterStorageBroker() && !InstallStorageFallback()) {
        return 0;
    }
    g_native_ready = true;

    api->Log(
        mod,
        SDGO_LOG_INFO,
        "ready: Mod-only friend unit gift companion; capability probe "
        "starts before Storage opens; storage hook owner=%s",
        g_storage_broker ? "storage_claim broker" : "gift fallback");
    return 1;
}
