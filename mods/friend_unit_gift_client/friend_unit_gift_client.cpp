#include <windows.h>

#include "client_model.h"
#include "client_runtime_policy.h"
#include "native_ui.h"
#include "transport.h"

#include "sdgo/friend_unit_gift_client_observer.h"
#include "sdgo/game_test_page_observer.h"
#include "sdgo/storage_extension.h"

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
using ComposerCtor = void*(__thiscall*)(
    void*, const void*, std::uint8_t);
using ComposerDtor = void(__thiscall*)(void*);
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
ui::Bindings g_ui;
ui::FriendEntry g_friend_main;
ui::FriendEntry g_friend_three_d;
ui::StorageView g_storage_view;
ui::UnitDetailView g_unit_detail_view;
NativeSession g_session = nullptr;
NativeApp g_app = nullptr;
FriendInit g_friend_main_init = nullptr;
FriendInit g_friend_three_d_init = nullptr;
FriendCallback g_friend_callback = nullptr;
UnitDetailInit g_unit_detail_init = nullptr;
UnitDetailEvent g_unit_detail_event = nullptr;
UnitDetailRefresh g_unit_detail_refresh = nullptr;
void* g_unit_detail_service_vtable = nullptr;
StorageInit g_storage_init = nullptr;
StorageEvent g_storage_event = nullptr;
StoragePacket g_storage_packet = nullptr;
ComposerCtor g_composer_ctor = nullptr;
ComposerDtor g_composer_dtor = nullptr;
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
void RefreshUnitDetailView();
bool ArmGameThread();

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
        !g_transport.MutationUncertain();
    const bool resolve =
        g_native_ready && g_model.CanResolveExisting();
    g_friend_main.SetAvailable(offer);
    g_friend_three_d.SetAvailable(offer);
    g_storage_view.SetAvailable(resolve);
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
            nativeSlot == active || brief[15] || brief[16] ||
            brief[17] || brief[18]) {
            continue;
        }
        UnitSnapshot unit;
        unit.uid = uid;
        unit.unit_number = unitNumber;
        unit.slot = nativeSlot;
        g_full(info, unit.full_record.data(), nativeSlot);
        if (Read<std::uint32_t>(unit.full_record.data()) != unitNumber ||
            Read<std::uint16_t>(unit.full_record.data(), 4) != nativeSlot ||
            Read<std::uint64_t>(unit.full_record.data(), 8) != uid ||
            unit.full_record[6] != 0 ||
            unit.full_record[137] != 0) {
            continue;
        }
        output->push_back(unit);
    }
    return true;
}

bool SnapshotUnits(std::vector<UnitSnapshot>* output) {
    __try {
        return SnapshotUnitsUnsafe(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (output) output->clear();
        return false;
    }
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

bool ReadNote(
    void* service,
    std::array<char16_t, kMaximumNoteCodeUnits>* note,
    std::uint16_t* codeUnits) {
    if (!service || !note || !codeUnits || !g_edit_get) return false;
    auto* root = Read<void*>(service, 0x4C);
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
    auto* root = Read<void*>(service, 0x4C);
    auto* edit = g_ui.FindNamed(root, L"DescEdit", 8);
    if (!edit) return false;
    std::wstring text;
    text.reserve(codeUnits);
    for (std::uint16_t index = 0; index < codeUnits; ++index) {
        text.push_back(static_cast<wchar_t>(note[index]));
    }
    const auto native = NativeView(text);
    g_edit_set(edit, &native, 0);
    return true;
}

void SetComposerPrimary(
    const std::wstring& caption,
    bool enabled) {
    if (!g_composer) return;
    auto* primary = Read<void*>(g_composer, 0xA4);
    g_ui.SetState(primary, true, enabled);
    g_ui.SetText(primary, caption);
}

void SetComposerNotice(const std::wstring& message) {
    if (!g_composer) return;
    auto* root = Read<void*>(g_composer, 0x4C);
    if (auto* notice = g_ui.FindNamed(root, L"NoticeStatic", 3)) {
        g_ui.SetText(notice, message, 1);
    }
}

bool SelectComposerUnit(std::uint64_t uid) {
    if (!g_composer || g_units.empty()) return false;
    auto* combo = Read<void*>(g_composer, 0xAC);
    if (!combo) return false;
    std::size_t selected = g_units.size();
    if (uid) {
        for (std::size_t index = 0; index < g_units.size(); ++index) {
            if (g_units[index].uid == uid) {
                selected = index;
                break;
            }
        }
    } else {
        const auto nativeIndex = Read<std::int32_t>(combo, 0x21C);
        if (nativeIndex >= 0 &&
            static_cast<std::size_t>(nativeIndex) < g_units.size()) {
            selected = static_cast<std::size_t>(nativeIndex);
        }
    }
    if (selected >= g_units.size()) return false;
    Put<std::int32_t>(
        combo, 0x21C, static_cast<std::int32_t>(selected));
    g_selected_unit = g_units[selected];
    g_composer_phase = ComposerPhase::selecting;
    g_model.CancelOffer();
    SetComposerPrimary(L"服务器预检", true);
    SetComposerNotice(
        L"已选择 #" + std::to_wstring(g_selected_unit.unit_number) +
        L"（UID " + std::to_wstring(g_selected_unit.uid) +
        L"）。点击“服务器预检”不会转移机体。");
    return true;
}

bool AddComboItem(
    void* combo,
    std::size_t index,
    const UnitSnapshot& unit) {
    if (!combo || !g_alloc || !g_free || !g_item_ctor ||
        !g_combo_add || !g_ui.skin) {
        return false;
    }
    auto* memory = g_alloc(0x220);
    if (!memory) return false;
    auto* item = g_item_ctor(
        memory, Read<void*>(combo, 0x15C));
    if (!item) {
        g_free(memory);
        return false;
    }
    Put<int>(item, 0x154, 192);
    Put<int>(item, 0x158, 18);
    for (const auto* path : {
             L"sprs\\17520.spr",
             L"sprs\\17521.spr",
             L"sprs\\17520.spr"}) {
        const std::wstring owned(path);
        const auto skin = NativeView(owned);
        g_ui.skin(item, &skin, 0, 0, 0);
    }
    g_ui.SetText(
        item,
        L"#" + std::to_wstring(unit.unit_number) +
            L"  UID " + std::to_wstring(unit.uid),
        1);
    const auto keyText = std::to_wstring(index);
    const auto key = NativeView(keyText);
    return g_combo_add(combo, &key, item) != 0;
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
        !g_model.pending() && !g_transport.MutationUncertain(),
        g_has_uncertain_operation &&
            g_transport.Ready() &&
            !g_model.pending());
}

bool BeginStorageList(Command command, std::uint32_t page) {
    if (command != Command::list_inbox &&
        command != Command::list_returned) {
        return false;
    }
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
        g_transport.MutationUncertain()) {
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
        command == Command::provenance
            ? L"正在读取机体来源…"
            : L"正在提交一次性处理请求；不会自动重发…",
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
            if (SyncObserver()) {
                ++g_observer.reply_sequence;
                g_observer.last_reply_command =
                    static_cast<std::uint32_t>(event->command);
                g_observer.last_reply_status = event->status;
                g_observer.last_reply_request_id = event->request_id;
                g_observer.last_reply_gift_id = event->reply->gift_id;
                g_observer.last_reply_unit_uid = event->reply->unit_uid;
            }
            if (event->command == Command::preview_offer) {
                if (event->status == WireStatus::success &&
                    g_model.HasOfferConfirmation()) {
                    g_composer_phase = ComposerPhase::confirming;
                    SetComposerPrimary(L"确认赠送", true);
                    SetComposerNotice(
                        L"服务器预检通过：向 " + g_receiver_name +
                        L" 赠送 #" +
                        std::to_wstring(g_selected_unit.unit_number) +
                        L"（UID " +
                        std::to_wstring(g_selected_unit.uid) +
                        L"）。再次点击才会提交。");
                } else {
                    g_composer_phase = ComposerPhase::selecting;
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
                    L"自定义机体礼物：" +
                        std::to_wstring(g_model.total_count()) +
                        L" 项。原生礼物仍保持独立。",
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
                        g_unit_detail_view.SetDetail(
                            ProvenanceDetailText(provenance),
                            true);
                    }
                    RefreshUnitDetailView();
                } else {
                    const auto& provenance =
                        g_model.provenance();
                    g_storage_view.ShowStatus(
                        provenance.present
                            ? ProvenanceText(provenance)
                            : StatusText(event->status),
                        true);
                    RenderStorageRows();
                }
                g_provenance_origin =
                    ProvenanceOrigin::none;
                g_provenance_request_uid = 0;
            }
        }
    } else if (event->kind == EventKind::request_failed) {
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

bool OpenComposer() {
    if (!g_native_ready || !g_model.CanOffer() ||
        g_composer || !g_alloc || !g_free ||
        !g_composer_ctor || !g_composer_dtor ||
        !g_enqueue) {
        return false;
    }
    struct EmptyVector {
        void* begin = nullptr;
        void* end = nullptr;
        void* capacity = nullptr;
    } empty;
    auto* object = g_alloc(0xC8);
    if (!object) return false;
    g_composer_ctor(object, &empty, 0);
    auto* live = g_enqueue(object, 1);
    g_composer_dtor(object);
    g_free(object);
    if (!live) return false;
    g_composer = live;
    g_composer_phase = ComposerPhase::selecting;
    g_units.clear();
    g_selected_unit = {};
    return true;
}

void PopulateComposer(void* service) {
    auto* root = Read<void*>(service, 0x4C);
    if (!root) return;
    if (auto* title =
            g_ui.FindNamed(root, L"TitleStatic", 3)) {
        g_ui.SetText(title, L"向 " + g_receiver_name + L" 赠送机体");
    }
    if (auto* notice =
            g_ui.FindNamed(root, L"NoticeStatic", 3)) {
        g_ui.SetText(
            notice,
            L"请选择自己拥有的具体机体并填写留言；"
            L"服务器预检后还需再次确认。",
            1);
    }
    auto* combo = Read<void*>(service, 0xAC);
    const bool unitsReady =
        combo && SnapshotUnits(&g_units) && !g_units.empty();
    if (combo && g_combo_clear) {
        g_combo_clear(combo);
        for (std::size_t i = 0; unitsReady && i < g_units.size(); ++i) {
            if (!AddComboItem(combo, i, g_units[i])) {
                g_units.clear();
                g_selected_unit = {};
                break;
            }
        }
        if (!g_units.empty()) {
            SelectComposerUnit(0);
        }
    }
    if (auto* label =
            g_ui.FindNamed(root, L"GiftNameStatic", 3)) {
        g_ui.SetText(
            label,
            g_units.empty()
                ? L"没有当前可预检的机体"
                : L"已读取 " + std::to_wstring(g_units.size()) +
                      L" 台可选机体");
    }
    for (const auto offset : {0x98u, 0x9Cu, 0xA0u}) {
        g_ui.SetState(Read<void*>(service, offset), false, false);
    }
    g_ui.SetState(
        Read<void*>(service, 0xA4), true, !g_units.empty());
    g_ui.SetText(
        Read<void*>(service, 0xA4),
        g_units.empty() ? L"没有可选机体" : L"服务器预检");
    g_ui.SetState(Read<void*>(service, 0xA8), true, true);
    g_ui.SetText(
        Read<void*>(service, 0xA8), L"取消");
}

void CloseComposer() {
    auto* service = g_composer;
    g_composer = nullptr;
    g_units.clear();
    g_selected_unit = {};
    g_model.CancelOffer();
    if (service && g_close) g_close(service, 1);
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

void CaptureFriendAndOpen(void* service) {
    auto* record = Read<std::uint8_t*>(service, 0x50);
    if (!record) return;
    const auto receiver = Read<std::uint32_t>(record);
    if (!receiver) return;
    char nickname[17]{};
    std::memcpy(nickname, record + 0x0A, 16);
    wchar_t wide[33]{};
    const int count = MultiByteToWideChar(
        CP_ACP, 0, nickname, -1, wide,
        static_cast<int>(std::size(wide)));
    if (count <= 0) return;
    g_receiver_id = receiver;
    g_receiver_name.assign(wide);
    OpenComposer();
}

void __fastcall FriendMainInit(
    void* service,
    void*,
    std::uint32_t argument) {
    g_friend_main_init(service, argument);
    __try {
        ArmGameThread();
        g_friend_main.Attach(service, g_ui);
        RefreshAvailability();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
        g_friend_three_d.Attach(service, g_ui);
        RefreshAvailability();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
            CaptureFriendAndOpen(service);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    g_friend_callback(service, event, second, control);
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
    void* eventData) {
    auto* control = Read<void*>(eventData, 0x10);
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
        UnitDetailEventUnsafe(service, eventData);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall ComposerUpdateHook(void* service, void*) {
    if (service == g_composer && Read<std::uint32_t>(service, 0x44) == 3) {
        __try {
            PopulateComposer(service);
            Put<std::uint32_t>(service, 0x44, 4);
            g_step(service);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            CloseComposer();
        }
        return;
    }
    g_composer_update(service);
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
    if (event == 0x201 &&
        control == Read<void*>(service, 0xAC)) {
        SelectComposerUnit(0);
        return;
    }
    if (event == 0x101 && control == Read<void*>(service, 0xA8)) {
        CloseComposer();
        return;
    }
    if (event == 0x101 && control == Read<void*>(service, 0xA4)) {
        if (g_composer_phase == ComposerPhase::selecting) {
            std::array<char16_t, kMaximumNoteCodeUnits> note{};
            std::uint16_t noteCodeUnits = 0;
            if (!SelectComposerUnit(0) ||
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
    // The stock 1261 business callback assumes a shop offer vector. Our
    // tracked clone deliberately has none, so all its business events remain
    // inside this companion Mod.
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
        g_storage_view.EnforceCustomView();
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
    if (event != 0x101) {
        return 0;
    }
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
    if (action == ui::StorageView::Action::open_returned) {
        if (!BeginStorageList(Command::list_returned, 1)) {
            g_storage_view.ShowStatus(
                StatusText(g_model.last_status()), true);
        }
        return 1;
    }

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
        BeginGiftAction(Command::provenance);
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

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_game_thread_ready ||
        GetCurrentThreadId() != g_ui_thread ||
        g_tick_active) {
        return;
    }
    g_tick_active = true;
    __try {
        g_transport.Tick();
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
    return StorageUiEvent(
               nullptr,
               service,
               0x101,
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
    if (!unitUid || !g_grid_data || !g_grid_find) return false;
    if (g_storage_view.custom_view() &&
        !InvokeStorageAction(ui::StorageView::Action::toggle)) {
        return false;
    }

    UnitSnapshot unit;
    if (!FindOwnedUnit(unitUid, &unit)) return false;
    auto* service = ResolveService(
        L"UnitIndexingInfo", g_unit_detail_service_vtable);
    if (!service ||
        service != g_unit_detail_view.service()) {
        return false;
    }
    auto* root = Read<void*>(service, 0x44);
    if (!root || Read<std::uint8_t>(root, 0x162) == 0) {
        return false;
    }
    auto* grid = g_grid_data();
    const std::uint8_t* record = nullptr;
    if (!grid ||
        !g_grid_find(
            grid,
            &record,
            unit.unit_number,
            2,
            1) ||
        !record) {
        return false;
    }

    std::memcpy(
        static_cast<std::uint8_t*>(service) + 0x94,
        unit.full_record.data(),
        unit.full_record.size());
    Put<const std::uint8_t*>(service, 0x90, record);
    UnitDetailRefreshHook(service, nullptr);

    auto* button = g_unit_detail_view.button();
    if (g_unit_detail_view.unit_uid() != unitUid ||
        !ControlReady(button)) {
        return false;
    }
    std::array<std::uint8_t, 0x14> eventData{};
    Put<void*>(eventData.data(), 0x10, button);
    UnitDetailEventHook(
        service, 0x101, eventData.data(), 0);
    const auto& pending = g_model.pending_request();
    return pending &&
           pending->command == Command::provenance &&
           pending->unit_uid == unitUid;
}

bool ExecuteObserverAction(
    const SdgoFriendUnitGiftClientActionRequestV1& request) {
    const auto runHash = SyncObserver();
    if (!runHash || request.run_hash != runHash ||
        request.action <=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_NONE ||
        request.action >
            SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_QUERY_RECEIPT) {
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
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_FRIEND_CONTEXT:
        accepted =
            request.receiver_id != 0 &&
            OpenFriendContextFor(request.receiver_id);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CLICK_GIFT: {
        ui::FriendEntry* entry = nullptr;
        if (g_friend_main.ready()) entry = &g_friend_main;
        else if (g_friend_three_d.ready()) entry = &g_friend_three_d;
        if (entry && ControlReady(entry->button())) {
            FriendEvent(
                entry->service(), 0x101, 0, entry->button());
            accepted = g_composer != nullptr;
        }
        break;
    }
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SELECT_UNIT: {
        if (!g_composer || !request.unit_uid) break;
        auto* combo = Read<void*>(g_composer, 0xAC);
        if (!combo || !ControlReady(combo)) break;
        for (std::size_t index = 0; index < g_units.size(); ++index) {
            if (g_units[index].uid != request.unit_uid) continue;
            Put<std::int32_t>(
                combo, 0x21C, static_cast<std::int32_t>(index));
            ComposerEvent(g_composer, 0x201, 0, combo);
            accepted = g_selected_unit.uid == request.unit_uid;
            break;
        }
        break;
    }
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_SET_NOTE:
        accepted =
            g_composer &&
            request.note_code_units <= kMaximumNoteCodeUnits &&
            SetNote(
                g_composer,
                request.note,
                request.note_code_units);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_PREVIEW:
        if (g_composer_phase == ComposerPhase::selecting) {
            accepted = InvokeComposerButton(
                Read<void*>(g_composer, 0xA4));
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CONFIRM:
        if (g_composer_phase == ComposerPhase::confirming) {
            accepted = InvokeComposerButton(
                Read<void*>(g_composer, 0xA4));
        }
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_CANCEL:
        accepted = InvokeComposerButton(
            g_composer ? Read<void*>(g_composer, 0xA8) : nullptr);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_OPEN_STORAGE_VIEW:
        if (!g_storage_view.custom_view()) {
            accepted = InvokeStorageAction(
                ui::StorageView::Action::toggle);
        }
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
        accepted = InvokeStorageAction(
            ui::StorageView::Action::accept);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_RETURN_TO_SENDER:
        accepted = InvokeStorageAction(
            ui::StorageView::Action::return_to_sender);
        break;
    case SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_TAKEBACK:
        accepted = InvokeStorageAction(
            ui::StorageView::Action::takeback);
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
                Read<void*>(g_composer, 0xA4));
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
    if (g_friend_main.service() ||
        g_friend_three_d.service()) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_FRIEND_CONTEXT;
    }
    if ((g_friend_main.ready() &&
         ControlReady(g_friend_main.button())) ||
        (g_friend_three_d.ready() &&
         ControlReady(g_friend_three_d.button()))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_GIFT_BUTTON;
    }
    if (g_composer) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMPOSER;
        snapshot.composer_phase =
            static_cast<std::uint32_t>(g_composer_phase) + 1u;
        auto* combo = Read<void*>(g_composer, 0xAC);
        auto* root = Read<void*>(g_composer, 0x4C);
        auto* note = g_ui.FindNamed(root, L"DescEdit", 8);
        auto* primary = Read<void*>(g_composer, 0xA4);
        auto* cancel = Read<void*>(g_composer, 0xA8);
        if (ControlReady(combo)) {
            snapshot.control_flags |=
                SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_COMBO;
        }
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
    if (g_storage_view.custom_view() &&
        g_model.row_count()) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ROWS;
    }
    if (SelectedGift() && !g_model.pending() &&
        !g_transport.MutationUncertain()) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_STORAGE_ACTION;
    }
    if ((SelectedGift() && g_model.CanReadProvenance()) ||
        (g_unit_detail_view.ready() &&
         ControlReady(g_unit_detail_view.button()))) {
        snapshot.control_flags |=
            SDGO_FRIEND_UNIT_GIFT_CLIENT_CONTROL_PROVENANCE;
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
    const auto& provenance = g_model.provenance();
    if (provenance.present) {
        snapshot.provenance_present = 1;
        std::wstring text = L"发送方：";
        for (std::size_t index = 0;
             index < provenance.peer_name_code_units;
             ++index) {
            text.push_back(static_cast<wchar_t>(
                provenance.peer_name[index]));
        }
        text += L"；发送时间：";
        text += std::to_wstring(
            provenance.offered_unix_millis);
        text += L"；接收时间：";
        text += std::to_wstring(
            provenance.accepted_unix_millis);
        text += L"；留言：";
        for (std::size_t index = 0;
             index < provenance.note_code_units;
             ++index) {
            text.push_back(static_cast<wchar_t>(
                provenance.note[index]));
        }
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

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    if (!api || !mod || !api->Symbol || !api->HookCode ||
        !api->Log) {
        return 0;
    }
    if (!g_transport.Bind(api, mod) ||
        !Bind(g_session, "storage.session") ||
        !g_ui.Bind(api, mod) ||
        !Bind(g_composer_ctor, "friend_unit_gift.composer_ctor") ||
        !Bind(g_composer_dtor, "friend_unit_gift.composer_dtor") ||
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
    g_transport.SetSink(&OnTransport, nullptr);

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
            "friend_unit_gift.composer_update",
            reinterpret_cast<void*>(&ComposerUpdateHook),
            reinterpret_cast<void**>(&g_composer_update)) ||
        !api->HookCode(
            mod,
            "friend_unit_gift.composer_callback",
            reinterpret_cast<void*>(&ComposerEvent),
            reinterpret_cast<void**>(&g_composer_callback))) {
        return 0;
    }

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
