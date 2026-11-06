// Over-custom (OC) six-stat enhancement, client side.
//
// Adds an OC allocation panel to the native factory upgrade page. The page
// works on the active unit (the profile record the native page copies to
// +0x6C). Requests ride the friend-gift carrier through the exports of
// friend_unit_gift_client.dll; the server answers with the full profile.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "native_ui.h"
#include "sdgo/oc_custom_client_observer.h"
#include "sdgo/oc_custom_protocol.h"
#include "sdgo/sdgo_api.h"

namespace {

using namespace sdgo::oc_custom;
using namespace sdgo::oc_custom::client;
using sdgo::friend_unit_gift::ClientExtensionSendFn;
using sdgo::friend_unit_gift::ClientExtensionStateFn;
using sdgo::friend_unit_gift::WireStatus;

constexpr char kHost[] = "GOnline-54BE1568-00C97000";
// CServiceFactoryRoomUnitUpGrade embeds the upgrade page component at +0xA8;
// its Init receives that component (dialog at +0x28, active record at +0x6C).
// The service's own OnUIEvent (vtable slot 14) receives the service pointer.
constexpr std::size_t kPageComponent = 0xA8;
constexpr std::size_t kPageDialog = 0x28;
constexpr std::size_t kPageRecord = 0x6C;
constexpr std::uint32_t kButtonClickEvent = 0x101;
constexpr std::uint32_t kIconClickEvent = 0x801;

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(pointer) + offset, sizeof(value));
    return value;
}

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
ui::Bindings g_bindings;
ui::Panel g_panel;
ClientExtensionSendFn g_send = nullptr;
ClientExtensionStateFn g_state = nullptr;
// The page component's Init is __thiscall with one unused stack argument
// (it returns with RET 4); the hook must pop it the same way.
using PageInit = void(__thiscall*)(void*, void*);
using PageCallback = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
PageInit g_init_original = nullptr;
PageCallback g_callback_original = nullptr;
using PageUpdate = void(__thiscall*)(void*);
PageUpdate g_update_original = nullptr;
// Native session and sender (same bindings the friend-gift transport uses) for
// re-requesting the active unit after a saved allocation, so every native copy
// of the owned record (profile, room pilot) carries the new OC payload.
using NativeSession = std::uint8_t*(__cdecl*)();
using NativeSend = int(__thiscall*)(void*, int, const std::uint8_t*, int, int);
NativeSession g_session = nullptr;
NativeSend g_native_send = nullptr;
std::uint8_t** g_profile = nullptr;
constexpr std::uint16_t kUserInfoRequestOpcode = 0x06B3;
constexpr std::size_t kProfileAccount = 0x138;
constexpr std::size_t kProfileActiveRecord = 0x1A9;
int g_panel_left = 560;
int g_panel_top = 96;
DWORD g_ui_thread = 0;
UINT_PTR g_timer = 0;
// Page component handed over by the loader-thread Init hook; the service's
// per-frame update (game thread) builds the panel for that same page.
void* volatile g_pending_page = nullptr;
void* g_upgrade_vtable = nullptr;
bool g_tick_active = false;

enum class Pending : std::uint8_t { none, mode, profile, set_normal, set_over, reset_cost, reset, challenge };

struct State {
    void* service = nullptr;
    std::uint64_t unit_uid = 0;
    std::uint32_t unit_number = 0;
    std::uint8_t record_level = 0;
    bool carrier_present = false;
    Carrier carrier{};
    bool mode_known = false;
    bool server_oc_mode = false;
    bool profile_present = false;
    Profile profile{};
    std::array<std::uint8_t, stat_count> pending_normal{};
    std::array<std::uint8_t, stat_count> pending_over{};
    bool over_mode = false;
    Pending outstanding = Pending::none;
    Pending queued = Pending::none;
    std::uint8_t queued_work = 0;
    std::uint64_t reset_armed_until = 0;
    std::int32_t reset_gp = 0;
    std::uint8_t reset_work = 0;
    std::wstring status;
    std::uint32_t request_count = 0;
    std::uint32_t reply_count = 0;
    std::int32_t last_status = 0;
    std::int32_t last_ret_val = 0;
    std::uint32_t last_command = 0;
    std::uint32_t last_upgrade_success = 0;
};
State g_state_data;

void Log(int level, const char* message) {
    if (g_api && g_api->Log) {
        const DWORD error = GetLastError();
        g_api->Log(g_mod, level, "%s", message);
        SetLastError(error);
    }
}

bool TransportIdle() {
    return g_send && g_state && g_state() == 1;
}

bool TransportReady() {
    return g_send && g_state && g_state() != 0;
}

int SumOf(const std::array<std::uint8_t, stat_count>& values) {
    int sum = 0;
    for (const auto value : values) sum += value;
    return sum;
}

int PendingLeft(const State& state) {
    if (!state.profile_present) return 0;
    if (state.over_mode)
        return static_cast<int>(state.profile.over_budget) - SumOf(state.pending_over);
    return static_cast<int>(state.profile.normal_budget) - SumOf(state.pending_normal);
}

bool Dirty(const State& state) {
    return state.profile_present &&
           (state.pending_normal != state.profile.normal || state.pending_over != state.profile.over);
}

std::wstring UnitCaption(const State& state) {
    if (!state.unit_uid) return L"";
    return L"机体 " + std::to_wstring(state.unit_number) + L" #" + std::to_wstring(state.unit_uid);
}

void RenderPanel() {
    if (!g_panel.Alive()) return;
    const auto& s = g_state_data;
    ui::View view;
    view.server_oc_mode = s.mode_known && s.server_oc_mode;
    view.profile_present = s.profile_present;
    view.busy = s.outstanding != Pending::none || s.queued != Pending::none;
    view.over_mode = s.over_mode;
    view.status = s.status;
    view.unit_caption = UnitCaption(s);
    view.oc_level = s.profile.oc_level;
    view.native_level = s.profile.native_level ? s.profile.native_level : s.record_level;
    view.normal_points_left = static_cast<std::uint8_t>(std::max(0, static_cast<int>(s.profile.normal_budget) - SumOf(s.profile.normal)));
    view.over_points_left = static_cast<std::uint8_t>(std::max(0, static_cast<int>(s.profile.over_budget) - SumOf(s.profile.over)));
    view.base = s.profile.base;
    view.normal_gain = s.profile.normal_stat;
    view.over_gain = s.profile.over_stat;
    view.normal = s.profile.normal;
    view.over = s.profile.over;
    view.pending_normal = s.pending_normal;
    view.pending_over = s.pending_over;
    view.pending_left = std::max(0, PendingLeft(s));
    view.dirty = Dirty(s);
    view.challenge_available = s.profile_present && s.profile.oc_level >= s.profile.normal_max_lv + 4 &&
                               s.profile.oc_level < 15;
    if (s.profile_present) {
        view.challenge_caption = L"OC 挑战 → Lv" + std::to_wstring(s.profile.next_level);
        view.reset_caption = s.over_mode ? L"洗 Over 点" : L"洗普通点";
        if (s.reset_armed_until && GetTickCount64() < s.reset_armed_until)
            view.reset_caption = L"确认洗点 " + std::to_wstring(s.reset_gp) + L" GP";
    }
    g_panel.Render(view);
}

void ApplyProfile(const Profile& profile) {
    auto& s = g_state_data;
    s.profile = profile;
    s.profile_present = true;
    s.pending_normal = profile.normal;
    s.pending_over = profile.over;
}

// After a saved change, ask for the user info again through the client's own
// request (0x6B3: opcode, account). Its 0x5C1 reply rebuilds the profile's
// active-unit record with the new payload before the next room is created.
// Re-selecting the already active unit (0x04CF) is answered without a record.
bool RefreshActiveUnitUnsafe() {
    const auto& s = g_state_data;
    if (!g_session || !g_native_send || !g_profile || !*g_profile || !s.unit_uid) return false;
    auto* profile = *g_profile;
    const auto account = Read<std::uint32_t>(profile, kProfileAccount);
    const auto active = Read<std::uint64_t>(profile, kProfileActiveRecord + 8);
    if (!account || active != s.unit_uid) return false;
    auto* session = g_session();
    if (!session) return false;
    const auto connection = Read<int>(session, 0x14);
    if (connection < 0) return false;
    std::uint8_t packet[6]{};
    sdgo::friend_unit_gift::wire::Write16(packet, kUserInfoRequestOpcode);
    sdgo::friend_unit_gift::wire::Write32(packet + 2, account);
    auto packets = Read<std::uint32_t>(session, 0x58);
    auto bytes = Read<std::uint32_t>(session, 0x5C);
    ++packets;
    bytes += sizeof(packet);
    std::memcpy(session + 0x58, &packets, sizeof(packets));
    std::memcpy(session + 0x5C, &bytes, sizeof(bytes));
    return g_native_send(session, connection, packet, sizeof(packet), 0) != 0;
}

void RefreshActiveUnit() {
    bool sent = false;
    __try {
        sent = RefreshActiveUnitUnsafe();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sent = false;
    }
    Log(SDGO_LOG_INFO, sent ? "oc allocation saved; active unit record re-requested"
                            : "oc allocation saved; the unit is not the active unit, no record refresh");
}

const wchar_t* SetFailureText(bool over, std::int32_t code) {
    switch (code) {
    case 1: return L"未找到该机体的 OC 档";
    case 2: return L"分配的点数超过可用点数";
    case 3: return L"某项属性超过上限";
    case 4: return over ? L"Over 加点需要 OC Lv9 以上且普通点已用完" : L"当前没有可用的普通点";
    case 5: return L"数据已被更新，请刷新后重试";
    default: return L"加点失败";
    }
}

const wchar_t* ResetFailureText(std::int32_t code) {
    switch (code) {
    case 1: return L"未找到该机体的 OC 档";
    case 2: return L"Over 点尚未全部获得，不能洗普通点";
    case 3: return L"没有可洗的点数";
    case 4: return L"该机体尚未达到 Over 等级";
    case 100: return L"GP 不足";
    default: return L"洗点失败";
    }
}

const wchar_t* UpgradeFailureText(std::int32_t code) {
    switch (code) {
    case 1: return L"未找到该机体的 OC 档";
    case 2: return L"尚未满足挑战条件（普通点等级已满且经验条已满）";
    case 3: case 4: case 5: return L"挑战道具不可用";
    case 6: return L"该等级没有挑战数据（对战次数不足）";
    case 7: return L"道具扣除失败";
    default: return L"OC 挑战失败";
    }
}

void __cdecl OnReply(void*, std::uint16_t command, std::int32_t status, const std::uint8_t* payload,
                     std::uint32_t bytes) {
    auto& s = g_state_data;
    const auto finished = s.outstanding;
    s.outstanding = Pending::none;
    ++s.reply_count;
    s.last_status = status;
    s.last_command = command;
    const auto wire = static_cast<WireStatus>(status);
    if (wire != WireStatus::success || !payload) {
        s.status = wire == WireStatus::disabled ? L"服务器未启用 OC 六维强化" : L"服务器暂时无法处理 OC 请求";
        if (finished == Pending::mode) s.mode_known = true, s.server_oc_mode = false;
        RenderPanel();
        return;
    }
    const auto wrapped = static_cast<Command>(command);
    std::int32_t ret_val = 0;
    Profile profile;
    switch (wrapped) {
    case Command::mode: {
        ModeReply mode;
        if (ReadModeReply(payload, bytes, &ret_val, &mode)) {
            s.mode_known = true;
            s.server_oc_mode = mode.enabled && mode.oc_mode;
            s.status = s.server_oc_mode ? L"正在读取 OC 档…" : L"服务器当前使用现行改造部件体系";
            if (s.server_oc_mode && s.unit_uid) s.queued = Pending::profile;
        }
        break;
    }
    case Command::get_profile:
        if (ReadProfileReply(payload, bytes, &ret_val, &profile)) {
            s.last_ret_val = ret_val;
            if (ret_val == 0) {
                ApplyProfile(profile);
                s.status = L"OC 档已读取";
            } else {
                s.status = ret_val == 1 ? L"该机体不属于当前账号" : L"该机体没有 OC 成长数据";
            }
        }
        break;
    case Command::set_normal:
    case Command::set_over: {
        SetReply reply;
        if (ReadSetReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            ApplyProfile(reply.profile);
            s.status = reply.ret_val == 0 ? L"加点已保存，下一场战斗生效"
                                          : SetFailureText(wrapped == Command::set_over, reply.ret_val);
            if (reply.ret_val == 0) RefreshActiveUnit();
        }
        break;
    }
    case Command::reset_cost: {
        ResetCostReply reply;
        if (ReadResetCostReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            if (reply.ret_val == 0) {
                s.reset_gp = reply.reset_gp;
                s.reset_armed_until = GetTickCount64() + 15000;
                s.status = L"洗点需要 " + std::to_wstring(reply.reset_gp) + L" GP，再次点击确认";
            } else {
                s.status = ResetFailureText(reply.ret_val);
            }
        }
        break;
    }
    case Command::reset: {
        ResetReply reply;
        if (ReadResetReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            s.reset_armed_until = 0;
            ApplyProfile(reply.profile);
            s.status = reply.ret_val == 0 ? L"已洗点，扣除 " + std::to_wstring(reply.charged_gp) + L" GP"
                                          : ResetFailureText(reply.ret_val);
            if (reply.ret_val == 0) RefreshActiveUnit();
        }
        break;
    }
    case Command::upgrade: {
        UpgradeReply reply;
        if (ReadUpgradeReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            s.last_upgrade_success = reply.success;
            ApplyProfile(reply.profile);
            if (reply.ret_val != 0)
                s.status = UpgradeFailureText(reply.ret_val);
            else if (reply.success)
                s.status = L"OC 挑战成功！OC 等级 " + std::to_wstring(reply.oc_level) + L"，Over 点 " +
                           std::to_wstring(reply.over_points);
            else
                s.status = reply.lv_prevention ? L"OC 挑战失败，等级已保护" : L"OC 挑战失败，OC 等级回到 Lv" +
                                                                                 std::to_wstring(reply.oc_level);
            if (reply.ret_val == 0) RefreshActiveUnit();
        }
        break;
    }
    default:
        break;
    }
    RenderPanel();
}

bool SendCommand(Command command, const std::uint8_t* payload, std::uint32_t bytes, Pending pending) {
    auto& s = g_state_data;
    if (!TransportIdle() || s.outstanding != Pending::none) return false;
    if (!g_send(static_cast<std::uint16_t>(command), payload, bytes, &OnReply, nullptr)) return false;
    s.outstanding = pending;
    ++s.request_count;
    return true;
}

void PumpQueue() {
    auto& s = g_state_data;
    if (s.queued == Pending::none || s.outstanding != Pending::none || !TransportIdle()) return;
    const auto next = s.queued;
    s.queued = Pending::none;
    std::array<std::uint8_t, 64> payload{};
    switch (next) {
    case Pending::mode:
        SendCommand(Command::mode, nullptr, 0, Pending::mode);
        break;
    case Pending::profile: {
        UnitRequest request;
        request.unit_uid = s.unit_uid;
        WriteUnitRequest(payload.data(), request);
        SendCommand(Command::get_profile, payload.data(), kUnitRequestBytes, Pending::profile);
        break;
    }
    case Pending::set_normal:
    case Pending::set_over: {
        SetRequest request;
        request.unit_uid = s.unit_uid;
        request.values = next == Pending::set_over ? s.pending_over : s.pending_normal;
        request.revision = s.profile.revision;
        WriteSetRequest(payload.data(), request);
        SendCommand(next == Pending::set_over ? Command::set_over : Command::set_normal, payload.data(),
                    kSetRequestBytes, next);
        break;
    }
    case Pending::reset_cost:
    case Pending::reset: {
        WorkRequest request;
        request.unit_uid = s.unit_uid;
        request.work = s.queued_work;
        WriteWorkRequest(payload.data(), request);
        SendCommand(next == Pending::reset ? Command::reset : Command::reset_cost, payload.data(),
                    kWorkRequestBytes, next);
        break;
    }
    case Pending::challenge: {
        UpgradeRequest request;
        request.unit_uid = s.unit_uid;
        WriteUpgradeRequest(payload.data(), request);
        SendCommand(Command::upgrade, payload.data(), kUpgradeRequestBytes, Pending::challenge);
        break;
    }
    default:
        break;
    }
}

void Queue(Pending pending, std::uint8_t work = 0) {
    auto& s = g_state_data;
    s.queued = pending;
    s.queued_work = work;
    PumpQueue();
    RenderPanel();
}

void HandleAction(ui::Action action, std::size_t stat_override, bool from_observer) {
    auto& s = g_state_data;
    if (action == ui::Action::panel_toggle) {
        g_panel.Toggle();
        if (g_panel.open() && s.unit_uid && !s.profile_present && s.outstanding == Pending::none &&
            s.queued == Pending::none)
            Queue(s.mode_known ? Pending::profile : Pending::mode);
        RenderPanel();
        return;
    }
    if (!s.unit_uid) return;
    const std::size_t stat = from_observer ? stat_override : ui::StatOf(action);
    if (stat >= stat_count) return;
    const bool interactive = s.mode_known && s.server_oc_mode && s.profile_present && s.outstanding == Pending::none &&
                             s.queued == Pending::none;
    switch (action) {
    case ui::Action::refresh:
        s.reset_armed_until = 0;
        if (!s.mode_known) Queue(Pending::mode);
        else Queue(Pending::profile);
        return;
    default:
        break;
    }
    if (!interactive) return;
    auto& vector = s.over_mode ? s.pending_over : s.pending_normal;
    if (ui::IsPlus(action)) {
        if (PendingLeft(s) > 0 && vector[stat] < 255) ++vector[stat];
        s.status = L"";
    } else if (ui::IsMinus(action)) {
        if (vector[stat] > 0) --vector[stat];
        s.status = L"";
    } else if (action == ui::Action::over_toggle) {
        s.over_mode = !s.over_mode;
        s.reset_armed_until = 0;
    } else if (action == ui::Action::apply) {
        if (!Dirty(s)) return;
        if (s.pending_normal != s.profile.normal) {
            s.status = L"正在保存普通点…";
            Queue(Pending::set_normal);
            return;
        }
        s.status = L"正在保存 Over 点…";
        Queue(Pending::set_over);
        return;
    } else if (action == ui::Action::reset) {
        const std::uint8_t work = s.over_mode ? 2 : 1;
        if (s.reset_armed_until && GetTickCount64() < s.reset_armed_until && s.reset_work == work) {
            s.reset_armed_until = 0;
            s.status = L"正在洗点…";
            Queue(Pending::reset, work);
            return;
        }
        s.reset_work = work;
        s.status = L"正在查询洗点费用…";
        Queue(Pending::reset_cost, work);
        return;
    } else if (action == ui::Action::challenge) {
        s.reset_armed_until = 0;
        s.status = L"正在进行 OC 挑战…";
        Queue(Pending::challenge);
        return;
    }
    RenderPanel();
}

void* FindSkinSource(void* dialog) {
    // Any native button with three skins on the page or its parent frame.
    __try {
        for (void* owner = dialog; owner; owner = Read<void*>(owner, 0x15C)) {
            auto* head = Read<std::uint8_t*>(owner, 0x23C);
            if (!head) continue;
            auto* node = Read<std::uint8_t*>(head);
            std::size_t guard = 0;
            while (node && node != head && ++guard < 2048) {
                auto* child = Read<void*>(node, 8);
                if (child && Read<void*>(child) == g_bindings.button_vtable && Read<void*>(child, 0x15C) == owner) {
                    auto** first = Read<std::uint8_t**>(child, 0x1A4);
                    auto** last = Read<std::uint8_t**>(child, 0x1A8);
                    if (first && last && last - first >= 3 && last - first <= 16 && first[0] && first[1] && first[2] &&
                        Read<void*>(first[0]) && Read<void*>(first[1]) && Read<void*>(first[2]))
                        return child;
                }
                node = Read<std::uint8_t*>(node);
            }
            if (owner == dialog) {
                // The page dialog's parent is the factory frame dialog.
                continue;
            }
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void AttachUnsafe(void* service);

void TraceActiveRecord() {
    static std::uint64_t last_uid = 0;
    static std::uint32_t last_revision = ~0u;
    if (!g_profile || !*g_profile) return;
    std::uint8_t record[kUnitRecordBytes];
    std::memcpy(record, *g_profile + kProfileActiveRecord, sizeof(record));
    Carrier carrier{};
    const bool present = ReadCarrier(record, &carrier);
    const auto uid = Read<std::uint64_t>(record, 8);
    const auto revision = present ? carrier.revision : 0u;
    if (uid == last_uid && revision == last_revision) return;
    last_uid = uid;
    last_revision = revision;
    char line[160];
    std::snprintf(line, sizeof(line), "oc active record uid=%llu carrier=%u revision=%u hp=%d",
                  static_cast<unsigned long long>(uid), present ? 1u : 0u, revision, present ? carrier.delta[1] : 0);
    Log(SDGO_LOG_INFO, line);
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (g_tick_active || GetCurrentThreadId() != g_ui_thread) return;
    g_tick_active = true;
    __try {
        TraceActiveRecord();
        if (g_panel.Ready() && !g_panel.Alive()) {
            g_panel.Detach();
            g_state_data.service = nullptr;
        }
        // Requests continue without the page (observer-driven selection).
        PumpQueue();
        if (g_state_data.reset_armed_until && GetTickCount64() >= g_state_data.reset_armed_until) {
            g_state_data.reset_armed_until = 0;
            RenderPanel();
        }
    } __finally {
        g_tick_active = false;
    }
}

void ArmTimer() {
    if (g_timer) return;
    g_ui_thread = GetCurrentThreadId();
    g_timer = SetTimer(nullptr, 0, 250, &Tick);
}

void AttachUnsafe(void* service) {
    auto* dialog = Read<void*>(service, kPageDialog);
    if (!dialog) return;
    std::uint8_t record[kUnitRecordBytes];
    std::memcpy(record, static_cast<const std::uint8_t*>(service) + kPageRecord, sizeof(record));
    auto& s = g_state_data;
    const auto uid = Read<std::uint64_t>(record, 8);
    if (uid != s.unit_uid) {
        s.profile_present = false;
        s.pending_normal = {};
        s.pending_over = {};
        s.status.clear();
    }
    s.service = service;
    s.unit_uid = uid;
    s.unit_number = Read<std::uint32_t>(record, 0);
    s.record_level = record[16];
    s.carrier_present = ReadCarrier(record, &s.carrier);
    if (s.carrier_present && (s.carrier.flags & carrier_oc_mode) && !s.mode_known) {
        s.mode_known = true;
        s.server_oc_mode = true;
    }
    s.outstanding = Pending::none; // a reply for a previous page is handled by the gift transport failing it
    void* source = FindSkinSource(dialog);
    char trace[240];
    std::snprintf(trace, sizeof(trace), "oc panel page component=%p dialog=%p size=%dx%d visible=%u skin=%p uid=%llu carrier=%u",
                  service, dialog, Read<int>(dialog, 0x154), Read<int>(dialog, 0x158), Read<std::uint8_t>(dialog, 0x162), source,
                  static_cast<unsigned long long>(uid), s.carrier_present ? 1u : 0u);
    Log(SDGO_LOG_INFO, trace);
    if (!g_panel.Attach(service, dialog, source, g_bindings, g_panel_left, g_panel_top)) {
        std::snprintf(trace, sizeof(trace), "oc panel could not attach to the factory upgrade page: %s",
                      g_panel.last_failure() ? g_panel.last_failure() : "unknown");
        Log(SDGO_LOG_WARN, trace);
        return;
    }
    Log(SDGO_LOG_INFO, "oc panel attached to the factory upgrade page");
    if (!source) Log(SDGO_LOG_INFO, "oc panel uses the client's standard button sprites");
    ArmTimer();
    if (!TransportReady()) {
        s.status = L"等待服务器能力握手…";
    } else if (!s.mode_known) {
        s.queued = Pending::mode;
    } else if (s.server_oc_mode && s.unit_uid && !s.profile_present) {
        s.queued = Pending::profile;
    }
    PumpQueue();
    RenderPanel();
}

void __fastcall PageInitHook(void* service, void*, void* argument) {
    g_init_original(service, argument);
    // Init runs on the game's resource loader thread. Native controls are
    // created on the game thread by the service's per-frame update.
    InterlockedExchangePointer(&g_pending_page, service);
}

// CServiceFactoryRoomUnitUpGrade per-frame update (vtable slot 12), game thread.
void __fastcall PageUpdateHook(void* owner, void*) {
    g_update_original(owner);
    const DWORD error = GetLastError();
    __try {
        if (!g_timer) ArmTimer();
        void* component = static_cast<std::uint8_t*>(owner) + kPageComponent;
        if (g_pending_page == component &&
            InterlockedCompareExchangePointer(&g_pending_page, nullptr, component) == component &&
            Read<void*>(owner) == g_upgrade_vtable)
            AttachUnsafe(component);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_panel.Detach();
    }
    SetLastError(error);
}

void __stdcall PageCallbackHook(void* service, std::uint32_t event, std::uintptr_t second, void* control) {
    if ((event == kButtonClickEvent || event == kIconClickEvent) && control && service && g_panel.Alive() &&
        g_panel.service() == static_cast<std::uint8_t*>(service) + kPageComponent) {
        const auto action = g_panel.Click(control);
        if (action != ui::Action::none) {
            const DWORD error = GetLastError();
            __try {
                HandleAction(action, 0, false);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            SetLastError(error);
            return;
        }
    }
    g_callback_original(service, event, second, control);
}

int Snapshot(SdgoOcCustomClientSnapshotV1* output) {
    if (!output || output->size < sizeof(SdgoOcCustomClientSnapshotV1) ||
        output->version != SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION)
        return 0;
    const auto& s = g_state_data;
    SdgoOcCustomClientSnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    snapshot.panel_attached = g_panel.Alive() ? 1u : 0u;
    snapshot.transport_ready = TransportReady() ? 1u : 0u;
    snapshot.server_oc_mode = s.mode_known && s.server_oc_mode ? 1u : 0u;
    snapshot.profile_present = s.profile_present ? 1u : 0u;
    snapshot.busy = s.outstanding != Pending::none || s.queued != Pending::none ? 1u : 0u;
    snapshot.request_count = s.request_count;
    snapshot.reply_count = s.reply_count;
    snapshot.last_status = s.last_status;
    snapshot.last_ret_val = s.last_ret_val;
    snapshot.last_command = s.last_command;
    snapshot.unit_uid = s.unit_uid;
    snapshot.unit_number = s.unit_number;
    snapshot.oc_level = s.profile.oc_level;
    snapshot.native_level = s.profile.native_level ? s.profile.native_level : s.record_level;
    snapshot.revision = s.profile.revision;
    snapshot.normal_points_left = static_cast<std::uint32_t>(std::max(0, static_cast<int>(s.profile.normal_budget) - SumOf(s.profile.normal)));
    snapshot.over_points_left = static_cast<std::uint32_t>(std::max(0, static_cast<int>(s.profile.over_budget) - SumOf(s.profile.over)));
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        snapshot.normal[stat] = s.profile.normal[stat];
        snapshot.over[stat] = s.profile.over[stat];
        snapshot.pending_normal[stat] = s.pending_normal[stat];
        snapshot.pending_over[stat] = s.pending_over[stat];
        snapshot.base[stat] = s.profile.base[stat];
        snapshot.normal_gain[stat] = s.profile.normal_stat[stat];
        snapshot.over_gain[stat] = s.profile.over_stat[stat];
        snapshot.carrier_delta[stat] = s.carrier.delta[stat];
    }
    snapshot.over_mode = s.over_mode ? 1u : 0u;
    snapshot.carrier_present = s.carrier_present ? 1u : 0u;
    snapshot.last_upgrade_success = s.last_upgrade_success;
    snapshot.last_reset_gp = s.reset_gp;
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return 1;
}

int Action(const SdgoOcCustomClientActionV1& request) {
    if (GetCurrentThreadId() != g_ui_thread && g_ui_thread) return 0;
    switch (request.action) {
    case 1: HandleAction(ui::Action::refresh, 0, true); return 1;
    case 2: HandleAction(static_cast<ui::Action>(static_cast<int>(ui::Action::plus0) + static_cast<int>(request.stat)), request.stat, true); return 1;
    case 3: HandleAction(static_cast<ui::Action>(static_cast<int>(ui::Action::minus0) + static_cast<int>(request.stat)), request.stat, true); return 1;
    case 4: HandleAction(ui::Action::apply, 0, true); return 1;
    case 5: HandleAction(ui::Action::reset, 0, true); return 1;
    case 6: {
        auto& s = g_state_data;
        s.reset_work = s.over_mode ? 2 : 1;
        s.reset_armed_until = GetTickCount64() + 15000;
        HandleAction(ui::Action::reset, 0, true);
        return 1;
    }
    case 7: HandleAction(ui::Action::challenge, 0, true); return 1;
    case 8: HandleAction(ui::Action::over_toggle, 0, true); return 1;
    case 9: {
        // Observer-only: work on a unit without the native page (GameTest).
        auto& s = g_state_data;
        if (!request.argument64) return 0;
        if (s.unit_uid != request.argument64) {
            s.profile_present = false;
            s.pending_normal = {};
            s.pending_over = {};
        }
        s.unit_uid = request.argument64;
        ArmTimer();
        if (!s.mode_known) Queue(Pending::mode);
        else Queue(Pending::profile);
        return 1;
    }
    default:
        return 0;
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientGetSnapshotV1(SdgoOcCustomClientSnapshotV1* output) {
    __try {
        return Snapshot(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientActionV1(const SdgoOcCustomClientActionV1* request) {
    if (!request || request->size < sizeof(*request) ||
        request->version != SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION)
        return 0;
    __try {
        return Action(*request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

SDGO_MOD_ENTRY {
    if (!api || !mod || api->version != SDGO_API_VERSION || !api->host_key || !api->Symbol || !api->HookCode ||
        !api->Log || std::strcmp(api->host_key, kHost) != 0)
        return 0;
    g_api = api;
    g_mod = mod;
    if (!api->SettingInt(mod, "Enabled", 1)) return 0;
    g_panel_left = api->SettingInt(mod, "PanelLeft", 4);
    g_panel_top = api->SettingInt(mod, "PanelTop", 172);
    if (!g_bindings.Bind(api, mod)) return 0;
    g_session = reinterpret_cast<NativeSession>(api->Symbol(mod, "storage.session"));
    g_native_send = reinterpret_cast<NativeSend>(api->Symbol(mod, "net.send_packet"));
    g_profile = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "hangar.profile_global"));
    if (!g_session || !g_native_send || !g_profile) return 0;
    HMODULE gift = GetModuleHandleW(L"friend_unit_gift_client.dll");
    if (gift) {
        g_send = reinterpret_cast<ClientExtensionSendFn>(
            GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));
        g_state = reinterpret_cast<ClientExtensionStateFn>(
            GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));
    }
    if (!g_send || !g_state) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "friend_unit_gift_client is required for OC lobby requests; over-custom panel inactive");
        return 0;
    }
    g_upgrade_vtable = api->Symbol(mod, "oc_custom.client_upgrade_vtable");
    if (!g_upgrade_vtable) {
        Log(SDGO_LOG_WARN, "oc panel: the factory upgrade service vtable symbol is missing");
        return 0;
    }
    if (!api->HookCode(mod, "oc_custom.client_upgrade_init", reinterpret_cast<void*>(&PageInitHook),
                       reinterpret_cast<void**>(&g_init_original)) ||
        !api->HookCode(mod, "oc_custom.client_upgrade_callback", reinterpret_cast<void*>(&PageCallbackHook),
                       reinterpret_cast<void**>(&g_callback_original)) ||
        !api->HookCode(mod, "oc_custom.client_upgrade_update", reinterpret_cast<void*>(&PageUpdateHook),
                       reinterpret_cast<void**>(&g_update_original)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: OC allocation panel on the factory upgrade page");
    return 1;
}
