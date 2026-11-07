// Over-custom (OC) six-stat enhancement, client side.
//
// Lobby: the original OC windows of the client package (uirs\0794.uir
// allocation, uirs\0813.uir challenge, uirs\0819.uir confirmation) and the
// OVER CUSTOM entry of the OC-era upgrade page run on the native factory
// upgrade page for the active unit (see original_ui.h). Their texts are the
// client's own OC strings (text table ids 126262..127192). Requests ride the
// friend-gift carrier through the exports of friend_unit_gift_client.dll; the
// server answers with the full profile.
//
// Battle: the OC speed and agility gains are client-side mobility. The stat
// setup of a battle unit (0x676C70) copies DashLevel and MotivePower from the
// unit data to unit +0xB64/+0xB6A, adds the current system's bonus there and
// then derives the movement values (0x675850). Right before that derive call
// the Mod adds the active unit's carried gains to the local unit's levels, the
// same place the native bonus lands; remote units move on their own clients.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "original_ui.h"
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
constexpr std::size_t kPageRecord = 0x6C;
constexpr std::size_t kServiceActive = 0xAC;
constexpr std::uint32_t kButtonClickEvent = 0x101;
constexpr std::uint32_t kCheckClickEvent = 0x401;

template <class T>
T Read(const void* pointer, std::size_t offset = 0) {
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(pointer) + offset, sizeof(value));
    return value;
}

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
ui::Bindings g_bindings;
ui::OriginalUi g_ui;
// Windows loaded by the loader-thread Init, adopted by the game thread.
SRWLOCK g_loaded_lock = SRWLOCK_INIT;
ui::OriginalUi g_loaded;
bool g_loaded_ready = false;
ClientExtensionSendFn g_send = nullptr;
ClientExtensionStateFn g_state = nullptr;
// The page component's Init is __thiscall with one unused stack argument
// (it returns with RET 4); the hook must pop it the same way.
using PageInit = void(__thiscall*)(void*, void*);
using PageCallback = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
using PageUpdate = void(__thiscall*)(void*);
PageInit g_init_original = nullptr;
PageCallback g_callback_original = nullptr;
PageUpdate g_update_original = nullptr;
// Native session and sender (same bindings the friend-gift transport uses) for
// re-requesting the active unit after a saved allocation, so every native copy
// of the owned record (profile, room pilot) carries the new OC payload.
using NativeSession = std::uint8_t*(__cdecl*)();
using NativeSend = int(__thiscall*)(void*, int, const std::uint8_t*, int, int);
NativeSession g_session = nullptr;
NativeSend g_native_send = nullptr;
std::uint8_t** g_profile = nullptr;
std::uint8_t** g_grid = nullptr;         // grid data manager (id -> typed record map)
std::uint8_t** g_unit_manager = nullptr; // battle unit manager
void* g_derive_original = nullptr;       // site trampoline of the derive call
constexpr std::uint16_t kUserInfoRequestOpcode = 0x06B3;
constexpr std::size_t kProfileAccount = 0x138;
constexpr std::size_t kProfileActiveRecord = 0x1A9;
constexpr std::size_t kUnitManagerLocal = 0x88;
DWORD g_ui_thread = 0;
UINT_PTR g_timer = 0;
void* g_upgrade_vtable = nullptr;
bool g_tick_active = false;

// --- Client data ------------------------------------------------------------

// The grid data manager keeps every client table record in one std::map keyed
// by id: node {left, parent, right, key, value, color, isnil(+0x15)}; value ->
// {type, record}. Type 0x3B is the UI text table (record: id, flag, UTF-16
// text at +5); type 2 is the unit table (name at +5, DashLevel +0x21E,
// MotivePower +0x221, UnitNumber_Transform +0x13C).
const std::uint8_t* GridRecord(std::int32_t id, std::int32_t type) noexcept {
    __try {
        auto* manager = g_grid ? *g_grid : nullptr;
        if (!manager) return nullptr;
        auto* head = Read<std::uint8_t*>(manager, 4);
        auto* node = Read<std::uint8_t*>(head, 4);
        auto* best = head;
        for (int guard = 0; node && !Read<std::uint8_t>(node, 0x15) && guard < 64; ++guard) {
            if (Read<std::int32_t>(node, 0xC) < id) {
                node = Read<std::uint8_t*>(node, 8);
            } else {
                best = node;
                node = Read<std::uint8_t*>(node, 0);
            }
        }
        if (best == head || Read<std::int32_t>(best, 0xC) != id) return nullptr;
        auto* value = Read<std::int32_t*>(best, 0x10);
        if (!value || value[0] != type) return nullptr;
        return reinterpret_cast<const std::uint8_t*>(value[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::size_t CopyChars(const std::uint8_t* record, std::size_t offset, wchar_t* out, std::size_t max_chars) noexcept {
    std::size_t count = 0;
    __try {
        const auto* chars = reinterpret_cast<const wchar_t*>(record + offset);
        while (count < max_chars && chars[count]) {
            out[count] = chars[count];
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        count = 0;
    }
    return count;
}

std::wstring RecordText(const std::uint8_t* record, std::size_t offset, std::size_t max_chars) {
    if (!record) return {};
    wchar_t buffer[512];
    const auto count = CopyChars(record, offset, buffer, std::min<std::size_t>(max_chars, 512));
    return std::wstring(buffer, count);
}

// The client's own OC strings. The fallback is the same Chinese text, used only
// when the table lookup fails.
std::wstring Text(std::uint32_t id, const wchar_t* fallback) {
    auto text = RecordText(GridRecord(static_cast<std::int32_t>(id), 0x3B), 5, 512);
    return text.empty() ? std::wstring(fallback) : text;
}

// Formats a table string that carries exactly the listed conversions.
std::wstring Format(const std::wstring& format, const wchar_t* expected, const std::wstring& a = L"",
                    const std::wstring& b = L"", const std::wstring& c = L"") {
    std::wstring conversions;
    for (std::size_t i = 0; i + 1 < format.size(); ++i)
        if (format[i] == L'%') {
            if (format[i + 1] == L'%') { ++i; continue; }
            conversions.push_back(format[i + 1]);
        }
    if (conversions != expected) return format;
    std::wstring result;
    const std::wstring args[3] = {a, b, c};
    std::size_t next = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] == L'%' && i + 1 < format.size()) {
            if (format[i + 1] == L'%') { result.push_back(L'%'); ++i; continue; }
            if (next < 3) result += args[next++];
            ++i;
            continue;
        }
        result.push_back(format[i]);
    }
    return result;
}

std::pair<std::wstring, std::wstring> Lines(const std::wstring& text) {
    const auto at = text.find(L'\n');
    if (at == std::wstring::npos) return {text, L""};
    return {text.substr(0, at), text.substr(at + 1)};
}

std::wstring LevelName(std::uint8_t level) {
    if (level >= 9) {
        const int over = level - 8;
        return over >= 7 ? std::wstring(L"OVER CUSTOM EX") : L"OVER CUSTOM" + std::to_wstring(over);
    }
    return L"Lv" + std::to_wstring(level);
}

std::wstring UnitName(std::uint32_t unit_number) {
    return RecordText(GridRecord(static_cast<std::int32_t>(unit_number), 2), 5, 50);
}

std::wstring ItemName(std::int32_t item) {
    // Item records carry their name at +5 like the other client tables.
    for (std::int32_t type : {3, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}) {
        auto name = RecordText(GridRecord(item, type), 5, 50);
        if (!name.empty()) return name;
    }
    return std::to_wstring(item);
}

// --- State ----------------------------------------------------------------

enum class Pending : std::uint8_t { none, mode, profile, set_normal, set_over, reset_cost, reset, challenge, items };
enum class Confirm : std::uint8_t { none, message, allocate, reset, challenge };

struct State {
    std::uint64_t unit_uid = 0;
    std::uint32_t unit_number = 0;
    std::uint8_t record_level = 0;
    bool carrier_present = false;
    Carrier carrier{};
    bool mode_known = false;
    bool server_oc_mode = false;
    bool profile_present = false;
    Profile profile{};
    Pending outstanding = Pending::none;
    Pending queued = Pending::none;
    std::uint8_t queued_work = 0;
    // The vector the next set request carries (the profile plus one point).
    std::array<std::uint8_t, stat_count> request_values{};
    bool request_over = false;
    std::int32_t reset_gp = 0;
    std::uint8_t reset_work = 0;
    bool challenge_open = false;
    ChallengeItemsReply items{};
    std::array<bool, kChallengeItemMax> item_selected{};
    Confirm confirm = Confirm::none;
    std::uint32_t confirm_text_id = 0;
    std::wstring confirm_line0, confirm_line1;
    std::uint32_t request_count = 0;
    std::uint32_t reply_count = 0;
    std::int32_t last_status = 0;
    std::int32_t last_ret_val = 0;
    std::uint32_t last_command = 0;
    std::uint32_t last_upgrade_success = 0;
};
State g_state_data;

struct Mobility {
    std::uint32_t applied = 0;
    std::uint32_t unit_number = 0;
    std::int32_t dash_base = 0, dash_before = 0, dash = 0;
    std::int32_t motive_base = 0, motive_before = 0, motive = 0;
    std::int32_t speed = 0, agility = 0;
};
Mobility g_mobility;

void Log(int level, const char* message) {
    if (g_api && g_api->Log) {
        const DWORD error = GetLastError();
        g_api->Log(g_mod, level, "%s", message);
        SetLastError(error);
    }
}

bool TransportIdle() { return g_send && g_state && g_state() == 1; }
bool TransportReady() { return g_send && g_state && g_state() != 0; }

int SumOf(const std::array<std::uint8_t, stat_count>& values) {
    int sum = 0;
    for (const auto value : values) sum += value;
    return sum;
}

int NormalLeft(const State& s) {
    return s.profile_present ? std::max(0, static_cast<int>(s.profile.normal_budget) - SumOf(s.profile.normal)) : 0;
}
int OverLeft(const State& s) {
    return s.profile_present ? std::max(0, static_cast<int>(s.profile.over_budget) - SumOf(s.profile.over)) : 0;
}
bool ChallengeAvailable(const State& s) {
    return s.profile_present && s.profile.oc_level >= s.profile.normal_max_lv + 4 && s.profile.oc_level < 15;
}
bool Busy(const State& s) { return s.outstanding != Pending::none || s.queued != Pending::none; }
std::int32_t StatValue(const State& s, std::size_t stat, int extra_normal, int extra_over) {
    const auto& p = s.profile;
    return p.base[stat] + (p.normal[stat] + extra_normal) * p.normal_stat[stat] +
           (p.over[stat] + extra_over) * p.over_stat[stat];
}

void ShowMessage(std::uint32_t id, const std::wstring& text) {
    auto& s = g_state_data;
    const auto lines = Lines(text);
    s.confirm = Confirm::message;
    s.confirm_text_id = id;
    s.confirm_line0 = lines.first;
    s.confirm_line1 = lines.second;
}

void ShowConfirm(Confirm kind, std::uint32_t id, const std::wstring& text) {
    ShowMessage(id, text);
    g_state_data.confirm = kind;
}

// --- Rendering --------------------------------------------------------------

bool PageActive() {
    __try {
        auto* component = g_ui.component();
        return component && Read<std::uint8_t>(static_cast<std::uint8_t*>(component) - kPageComponent, kServiceActive);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RenderWindows(bool active, const ui::EntryView& entry, const ui::OverView& over,
                   const ui::ChallengeView& challenge, const ui::ConfirmView& confirm) noexcept {
    __try {
        if (active) g_ui.Render(entry, over, challenge, confirm);
        else g_ui.HideAll();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Render() {
    if (!g_ui.Alive()) return;
    const auto& s = g_state_data;
    const bool active = PageActive();
    const bool oc = active && s.mode_known && s.server_oc_mode && s.profile_present;
    const bool idle = !Busy(s) && s.confirm == Confirm::none;
    ui::EntryView entry;
    entry.visible = oc;
    entry.lit = oc && ChallengeAvailable(s) && !s.challenge_open;
    ui::OverView over;
    // The allocation window (OC tab) and the challenge window never showed together.
    over.visible = oc && !s.challenge_open;
    if (oc) {
        static const std::uint32_t kLabels[stat_count] = {126638, 126639, 126640, 126641, 126642, 126643};
        static const wchar_t* kFallback[stat_count] = {L"推进力", L"耐久力", L"攻击力", L"防御力", L"机动力", L"必杀技"};
        over.notice = Text(126644, L"强化点数");
        over.normal_left = std::to_wstring(NormalLeft(s));
        over.over_left = std::to_wstring(OverLeft(s));
        const bool can_normal = NormalLeft(s) > 0;
        const bool can_over = !can_normal && s.profile.oc_level >= 9 && OverLeft(s) > 0;
        for (std::size_t stat = 0; stat < stat_count; ++stat) {
            over.label[stat] = Text(kLabels[stat], kFallback[stat]);
            over.normal[stat] = std::to_wstring(s.profile.normal[stat]);
            over.over[stat] = std::to_wstring(s.profile.over[stat]);
            over.up[stat] = idle && !s.challenge_open && (can_normal || can_over);
        }
        const bool over_reset = s.profile.oc_level >= 9;
        over.reset = over_reset ? Text(126647, L"终极强化还原") : Text(126646, L"基本强化点数还原");
        over.reset_enabled = idle && !s.challenge_open &&
                             (over_reset ? SumOf(s.profile.over) > 0 : SumOf(s.profile.normal) > 0);
        over.ex_enabled = false;
    }
    ui::ChallengeView challenge;
    challenge.visible = oc && s.challenge_open;
    if (challenge.visible) {
        challenge.title = Text(126631, L"终极强化");
        challenge.unit = UnitName(s.unit_number);
        challenge.state = Format(Text(126720, L"对战次数:%d回"), L"d", std::to_wstring(s.profile.battles));
        challenge.target_title = Text(127182, L"等级");
        challenge.target = LevelName(s.profile.next_level ? s.profile.next_level : s.profile.oc_level + 1);
        challenge.message0 = Format(Text(126721, L"条件:对战次数%d回以上"), L"d", std::to_wstring(s.profile.next_battle_cnt));
        challenge.over_title = Text(126631, L"终极强化");
        challenge.message1 = Text(126682, L"终极强化失败时.\n机体等级会降低到最高基本强化等级.");
        challenge.start_enabled = idle && ChallengeAvailable(s);
        for (std::size_t slot = 0; slot < s.items.count && slot < ui::kItemSlots; ++slot) {
            const auto& item = s.items.items[slot];
            challenge.item_visible[slot] = true;
            challenge.item_checked[slot] = s.item_selected[slot];
            challenge.item_text[slot] = ItemName(item.item) + L" x" + std::to_wstring(item.count);
        }
    }
    ui::ConfirmView confirm;
    confirm.visible = active && s.confirm != Confirm::none;
    confirm.line0 = s.confirm_line0;
    confirm.line1 = s.confirm_line1;
    confirm.cancel = s.confirm != Confirm::message;
    RenderWindows(active, entry, over, challenge, confirm);
}

// --- Requests ---------------------------------------------------------------

void ApplyProfile(const Profile& profile) {
    auto& s = g_state_data;
    s.profile = profile;
    s.profile_present = true;
}

// After a saved change, ask for the user info again through the client's own
// request (0x6B3: opcode, account). Its 0x5C1 reply rebuilds the profile's
// active-unit record with the new payload before the next room is created.
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
    Log(SDGO_LOG_INFO, sent ? "oc change saved; active unit record re-requested"
                            : "oc change saved; the unit is not the active unit, no record refresh");
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
        if (finished == Pending::mode) s.mode_known = true, s.server_oc_mode = false;
        if (finished != Pending::mode && finished != Pending::profile)
            ShowMessage(126270, Text(126270, L"无法使用强化点数."));
        Render();
        return;
    }
    std::int32_t ret_val = 0;
    Profile profile;
    switch (static_cast<Command>(command)) {
    case Command::mode: {
        ModeReply mode;
        if (ReadModeReply(payload, bytes, &ret_val, &mode)) {
            s.mode_known = true;
            s.server_oc_mode = mode.enabled && mode.oc_mode;
            if (s.server_oc_mode && s.unit_uid) s.queued = Pending::profile;
        }
        break;
    }
    case Command::get_profile:
        if (ReadProfileReply(payload, bytes, &ret_val, &profile)) {
            s.last_ret_val = ret_val;
            if (ret_val == 0) ApplyProfile(profile);
        }
        break;
    case Command::set_normal:
    case Command::set_over: {
        SetReply reply;
        if (ReadSetReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            ApplyProfile(reply.profile);
            if (reply.ret_val == 0) {
                RefreshActiveUnit();
            } else if (reply.ret_val == 3) {
                ShowMessage(126673, Text(126673, L"不能再使用.\n超过了最大能力值."));
            } else {
                ShowMessage(126270, Text(126270, L"无法使用强化点数."));
            }
        }
        break;
    }
    case Command::reset_cost: {
        ResetCostReply reply;
        if (ReadResetCostReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            if (reply.ret_val == 0) {
                s.reset_gp = reply.reset_gp;
                const auto title = s.reset_work == 2 ? Text(126647, L"终极强化还原") : Text(126646, L"基本强化点数还原");
                ShowConfirm(Confirm::reset, s.reset_work == 2 ? 126647 : 126646,
                            title + L"\n" + Text(126649, L"G币:") + L" " + std::to_wstring(reply.reset_gp));
            } else {
                ShowMessage(126271, Text(126271, L"不能初始化."));
            }
        }
        break;
    }
    case Command::reset: {
        ResetReply reply;
        if (ReadResetReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            ApplyProfile(reply.profile);
            if (reply.ret_val == 0) {
                RefreshActiveUnit();
            } else if (reply.ret_val == reset_insufficient_gp) {
                ShowMessage(126272, Text(126272, L"初始化费用不足."));
            } else {
                ShowMessage(126271, Text(126271, L"不能初始化."));
            }
        }
        break;
    }
    case Command::challenge_items: {
        ChallengeItemsReply reply;
        if (ReadChallengeItemsReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            s.items = reply;
            s.item_selected = {};
        }
        break;
    }
    case Command::upgrade: {
        UpgradeReply reply;
        if (ReadUpgradeReply(payload, bytes, &reply)) {
            s.last_ret_val = reply.ret_val;
            s.last_upgrade_success = reply.success;
            ApplyProfile(reply.profile);
            s.challenge_open = false;
            if (reply.ret_val == 2) {
                ShowMessage(126680, Text(126680, L"该机体经验值不足,所以不能进行终极强化."));
            } else if (reply.ret_val == 6) {
                ShowMessage(126719, Text(126719, L"该机体不能进行终极强化.\n对战次数不足."));
            } else if (reply.ret_val != 0) {
                ShowMessage(126700, Text(126700, L"该机体等级不能进行终极强化."));
            } else if (reply.success) {
                ShowMessage(126704, Format(Text(126704, L"[ %s ]强化成功!"), L"s", LevelName(reply.oc_level)));
            } else {
                ShowMessage(126691, Format(Text(126691, L"终极强化失败.\n机体等級下降为%s."), L"s",
                                           LevelName(reply.oc_level)));
            }
            if (reply.ret_val == 0) RefreshActiveUnit();
        }
        break;
    }
    default:
        break;
    }
    Render();
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
    case Pending::profile:
    case Pending::items: {
        UnitRequest request;
        request.unit_uid = s.unit_uid;
        WriteUnitRequest(payload.data(), request);
        SendCommand(next == Pending::items ? Command::challenge_items : Command::get_profile, payload.data(),
                    kUnitRequestBytes, next);
        break;
    }
    case Pending::set_normal:
    case Pending::set_over: {
        SetRequest request;
        request.unit_uid = s.unit_uid;
        request.values = s.request_values;
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
        SendCommand(next == Pending::reset ? Command::reset : Command::reset_cost, payload.data(), kWorkRequestBytes,
                    next);
        break;
    }
    case Pending::challenge: {
        UpgradeRequest request;
        request.unit_uid = s.unit_uid;
        std::size_t used = 0;
        for (std::size_t slot = 0; slot < s.items.count && used < 3; ++slot)
            if (s.item_selected[slot]) {
                request.item[used] = s.items.items[slot].item;
                request.count[used] = 1;
                ++used;
            }
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
    Render();
}

// --- Actions -----------------------------------------------------------------

std::wstring SelectedItemNames(std::size_t* count) {
    const auto& s = g_state_data;
    std::wstring names[3];
    std::size_t used = 0;
    for (std::size_t slot = 0; slot < s.items.count && used < 3; ++slot)
        if (s.item_selected[slot]) names[used++] = ItemName(s.items.items[slot].item);
    *count = used;
    if (used == 1) return Format(Text(126683, L"进行终极强化.\n使用道具 : %s"), L"s", names[0]);
    if (used == 2) return Format(Text(126684, L"进行终极强化.\n使用道具 : %s , %s"), L"ss", names[0], names[1]);
    if (used == 3)
        return Format(Text(126685, L"进行终极强化.\n使用道具 : %s , %s , %s"), L"sss", names[0], names[1], names[2]);
    return Lines(Text(126683, L"进行终极强化.\n使用道具 : %s")).first;
}

void HandleAction(ui::Action action) {
    auto& s = g_state_data;
    if (!s.unit_uid || !s.profile_present) return;
    if (action == ui::Action::confirm_ok || action == ui::Action::confirm_cancel) {
        const auto kind = s.confirm;
        s.confirm = Confirm::none;
        s.confirm_text_id = 0;
        if (action == ui::Action::confirm_ok) {
            if (kind == Confirm::allocate) Queue(s.request_over ? Pending::set_over : Pending::set_normal);
            else if (kind == Confirm::reset) Queue(Pending::reset, s.reset_work);
            else if (kind == Confirm::challenge) Queue(Pending::challenge);
        }
        Render();
        return;
    }
    if (s.confirm != Confirm::none || Busy(s)) return;
    if (ui::IsUp(action)) {
        const auto stat = ui::UpStat(action);
        const bool normal = NormalLeft(s) > 0;
        const bool over = !normal && s.profile.oc_level >= 9 && OverLeft(s) > 0;
        if (!normal && !over) {
            ShowMessage(126270, Text(126270, L"无法使用强化点数."));
        } else if (StatValue(s, stat, normal ? 1 : 0, over ? 1 : 0) > kStatCaps[stat]) {
            ShowMessage(126673, Text(126673, L"不能再使用.\n超过了最大能力值."));
        } else {
            s.request_over = over;
            s.request_values = over ? s.profile.over : s.profile.normal;
            ++s.request_values[stat];
            ShowConfirm(Confirm::allocate, 126262,
                        Text(126262, L"在使用强化点数之前再仔细考虑下如何？确定要强化这项性能吗?"));
            // The original sentence is one line; split it at the question mark.
            const auto at = s.confirm_line0.find(L"？");
            if (s.confirm_line1.empty() && at != std::wstring::npos && at + 1 < s.confirm_line0.size()) {
                s.confirm_line1 = s.confirm_line0.substr(at + 1);
                s.confirm_line0 = s.confirm_line0.substr(0, at + 1);
            }
        }
    } else if (action == ui::Action::reset) {
        s.reset_work = s.profile.oc_level >= 9 ? 2 : 1;
        Queue(Pending::reset_cost, s.reset_work);
        return;
    } else if (action == ui::Action::entry) {
        if (!ChallengeAvailable(s)) {
            ShowMessage(126700, Text(126700, L"该机体等级不能进行终极强化."));
        } else {
            s.challenge_open = true;
            s.items = {};
            s.item_selected = {};
            Queue(Pending::items);
            return;
        }
    } else if (action == ui::Action::challenge_close) {
        s.challenge_open = false;
    } else if (ui::IsItem(action)) {
        const auto slot = ui::ItemIndex(action);
        if (slot < s.items.count) {
            std::size_t selected = 0;
            for (std::size_t index = 0; index < s.items.count; ++index) selected += s.item_selected[index] ? 1 : 0;
            if (!s.item_selected[slot] && selected >= 3) {
                ShowMessage(126692, Text(126692, L"同样的道具不能重复使用2个."));
            } else {
                s.item_selected[slot] = !s.item_selected[slot];
            }
        }
    } else if (action == ui::Action::challenge_start) {
        if (ChallengeAvailable(s)) {
            std::size_t count = 0;
            const auto text = SelectedItemNames(&count);
            ShowConfirm(Confirm::challenge, count == 0 ? 126683 : 126682 + static_cast<std::uint32_t>(count), text);
        }
    }
    Render();
}

// --- Page lifecycle ------------------------------------------------------------

void AdoptRecord(void* component) {
    std::uint8_t record[kUnitRecordBytes];
    std::memcpy(record, static_cast<const std::uint8_t*>(component) + kPageRecord, sizeof(record));
    auto& s = g_state_data;
    const auto uid = Read<std::uint64_t>(record, 8);
    if (uid != s.unit_uid) {
        s.profile_present = false;
        s.challenge_open = false;
        s.confirm = Confirm::none;
    }
    s.unit_uid = uid;
    s.unit_number = Read<std::uint32_t>(record, 0);
    s.record_level = record[16];
    s.carrier_present = ReadCarrier(record, &s.carrier);
    if (s.carrier_present && (s.carrier.flags & carrier_oc_mode) && !s.mode_known) {
        s.mode_known = true;
        s.server_oc_mode = true;
    }
}

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
    char line[200];
    std::snprintf(line, sizeof(line), "oc active record uid=%llu carrier=%u revision=%u speed=%d hp=%d agility=%d",
                  static_cast<unsigned long long>(uid), present ? 1u : 0u, revision, present ? carrier.delta[0] : 0,
                  present ? carrier.delta[1] : 0, present ? carrier.delta[4] : 0);
    Log(SDGO_LOG_INFO, line);
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (g_tick_active || GetCurrentThreadId() != g_ui_thread) return;
    g_tick_active = true;
    __try {
        TraceActiveRecord();
        if (g_ui.Loaded() && !g_ui.Alive()) g_ui.Detach();
        PumpQueue();
        Render();
    } __finally {
        g_tick_active = false;
    }
}

void ArmTimer() {
    if (g_timer) return;
    g_ui_thread = GetCurrentThreadId();
    g_timer = SetTimer(nullptr, 0, 250, &Tick);
}

void AttachUnsafe() {
    // Adopt the windows the loader thread prepared for this page.
    AcquireSRWLockExclusive(&g_loaded_lock);
    if (g_loaded_ready) {
        g_ui = g_loaded;
        g_loaded.Detach();
        g_loaded_ready = false;
    }
    ReleaseSRWLockExclusive(&g_loaded_lock);
    if (!g_ui.Loaded() || g_ui.entry_ready()) return;
    if (!g_ui.AttachEntry()) {
        char trace[320];
        std::snprintf(trace, sizeof(trace), "oc entry could not attach: %s",
                      g_ui.last_failure() ? g_ui.last_failure() : "unknown");
        Log(SDGO_LOG_WARN, trace);
        g_ui.Detach();
        return;
    }
    AdoptRecord(g_ui.component());
    auto& s = g_state_data;
    char trace[220];
    std::snprintf(trace, sizeof(trace),
                  "oc original windows attached: 0794/0813/0819 bound, OVER CUSTOM entry restored; uid=%llu "
                  "carrier=%u text126631=%s",
                  static_cast<unsigned long long>(s.unit_uid), s.carrier_present ? 1u : 0u,
                  GridRecord(126631, 0x3B) ? "client" : "fallback");
    Log(SDGO_LOG_INFO, trace);
    s.outstanding = Pending::none;
    if (!TransportReady()) {
    } else if (!s.mode_known) {
        s.queued = Pending::mode;
    } else if (s.server_oc_mode && s.unit_uid && !s.profile_present) {
        s.queued = Pending::profile;
    }
    PumpQueue();
    Render();
}

bool LoadWindows(ui::OriginalUi* target, void* component) noexcept {
    __try {
        return target->Load(component, g_bindings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall PageInitHook(void* component, void*, void* argument) {
    g_init_original(component, argument);
    // Init runs on the resource loader thread, where the page loads its own
    // uirs\0799.uir; the OC layouts load the same way right after it.
    ui::OriginalUi loaded;
    const bool ok = LoadWindows(&loaded, component);
    if (!ok) {
        char trace[320];
        std::snprintf(trace, sizeof(trace), "oc original windows not loaded: %s",
                      loaded.last_failure() ? loaded.last_failure() : "fault");
        Log(SDGO_LOG_WARN, trace);
        return;
    }
    AcquireSRWLockExclusive(&g_loaded_lock);
    g_loaded = loaded;
    g_loaded_ready = true;
    ReleaseSRWLockExclusive(&g_loaded_lock);
}

// CServiceFactoryRoomUnitUpGrade per-frame update (vtable slot 12), game thread.
void __fastcall PageUpdateHook(void* owner, void*) {
    g_update_original(owner);
    const DWORD error = GetLastError();
    __try {
        if (!g_timer) ArmTimer();
        if (Read<void*>(owner) == g_upgrade_vtable) {
            bool pending = false;
            AcquireSRWLockShared(&g_loaded_lock);
            pending = g_loaded_ready && g_loaded.component() == static_cast<std::uint8_t*>(owner) + kPageComponent;
            ReleaseSRWLockShared(&g_loaded_lock);
            if (pending || (g_ui.Loaded() && !g_ui.entry_ready() &&
                            g_ui.component() == static_cast<std::uint8_t*>(owner) + kPageComponent))
                AttachUnsafe();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ui.Detach();
    }
    SetLastError(error);
}

bool DispatchClick(void* service, std::uint32_t event, void* control) {
    if (!control || !service || !g_ui.Alive() || g_ui.component() != static_cast<std::uint8_t*>(service) + kPageComponent)
        return false;
    const auto action = g_ui.Click(control);
    if (action == ui::Action::none) return false;
    if (ui::IsItem(action) ? (event != kCheckClickEvent && event != kButtonClickEvent) : event != kButtonClickEvent)
        return true; // ours, but not a click (hover, leave): swallow
    const DWORD error = GetLastError();
    __try {
        HandleAction(action);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    SetLastError(error);
    return true;
}

void __stdcall PageCallbackHook(void* service, std::uint32_t event, std::uintptr_t second, void* control) {
    if (DispatchClick(service, event, control)) return;
    g_callback_original(service, event, second, control);
}

// --- Battle mobility -------------------------------------------------------------

bool SameUnitFamily(std::uint32_t active, std::uint32_t current) {
    if (active == current) return true;
    const auto* record = GridRecord(static_cast<std::int32_t>(active), 2);
    return record && Read<std::uint32_t>(record, 0x13C) == current;
}

// The active unit's carried speed/agility gains for a battle unit built from
// it (either form of a transformable unit); false when nothing applies.
bool GainFor(const std::uint8_t* unit, std::int16_t* speed, std::int16_t* agility, std::uint32_t* revision) {
    if (!g_profile || !*g_profile) return false;
    std::uint8_t record[kUnitRecordBytes];
    std::memcpy(record, *g_profile + kProfileActiveRecord, sizeof(record));
    Carrier carrier{};
    if (!ReadCarrier(record, &carrier) || !(carrier.flags & carrier_oc_mode) || !(carrier.flags & carrier_profile))
        return false;
    *speed = carrier.delta[stat_speed];
    *agility = carrier.delta[stat_agility];
    *revision = carrier.revision;
    if (!*speed && !*agility) return false;
    auto* spec = Read<std::uint8_t*>(unit, 0x9C4);
    return spec && SameUnitFamily(Read<std::uint32_t>(record), Read<std::uint32_t>(spec));
}

void AddGain(std::uint8_t* unit, std::int16_t speed, std::int16_t agility, std::uint32_t revision, const char* when) {
    auto add = [unit](std::size_t offset, std::int16_t value) {
        std::int16_t level = Read<std::int16_t>(unit, offset);
        level = static_cast<std::int16_t>(level + value);
        std::memcpy(unit + offset, &level, sizeof(level));
    };
    auto* spec = Read<std::uint8_t*>(unit, 0x9C4);
    g_mobility.dash_before = Read<std::int16_t>(unit, 0xB64);
    g_mobility.motive_before = Read<std::int16_t>(unit, 0xB6A);
    // unit +0xB64 DashLevel, +0xB6A MotivePower (with the native bonus) and
    // their restore copies +0xD5C/+0xD5E, written by the stat setup.
    add(0xB64, speed);
    add(0xD5C, speed);
    add(0xB6A, agility);
    add(0xD5E, agility);
    g_mobility.applied++;
    g_mobility.speed = speed;
    g_mobility.agility = agility;
    g_mobility.unit_number = Read<std::uint32_t>(spec);
    g_mobility.dash_base = spec[0x21E];
    g_mobility.motive_base = spec[0x221];
    g_mobility.dash = Read<std::int16_t>(unit, 0xB64);
    g_mobility.motive = Read<std::int16_t>(unit, 0xB6A);
    char line[220];
    std::snprintf(line, sizeof(line), "oc mobility applied (%s) unit=%u revision=%u dash %d->%d (+%d) motive %d->%d (+%d)",
                  when, g_mobility.unit_number, revision, g_mobility.dash_before, g_mobility.dash, speed,
                  g_mobility.motive_before, g_mobility.motive, agility);
    Log(SDGO_LOG_INFO, line);
}

// Stat setups seen for the active unit's family while the unit was not (yet)
// the local unit: the battle start builds units before AddPlayer marks the
// local one, so the gain is added when AddPlayer does.
struct SetupMark {
    std::uint8_t* unit = nullptr;
    bool applied = false;
};
std::array<SetupMark, 16> g_setups{};

SetupMark* MarkFor(std::uint8_t* unit) {
    for (auto& mark : g_setups)
        if (mark.unit == unit) return &mark;
    for (auto& mark : g_setups)
        if (!mark.unit) return &(mark = SetupMark{unit, false});
    for (std::size_t index = 1; index < g_setups.size(); ++index) g_setups[index - 1] = g_setups[index];
    return &(g_setups.back() = SetupMark{unit, false});
}

extern "C" void __cdecl ApplyMobility(std::uint8_t* unit) {
    __try {
        auto* manager = g_unit_manager ? *g_unit_manager : nullptr;
        std::int16_t speed = 0, agility = 0;
        std::uint32_t revision = 0;
        if (!unit || !manager || !GainFor(unit, &speed, &agility, &revision)) return;
        auto* mark = MarkFor(unit);
        if (Read<std::uint8_t*>(manager, kUnitManagerLocal) != unit) {
            mark->applied = false; // fresh native levels; AddPlayer may mark it local later
            return;
        }
        AddGain(unit, speed, agility, revision, "stat setup");
        mark->applied = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using AddPlayerFn = void(__thiscall*)(void*, void*, int, int);
using DeriveFn = void(__thiscall*)(void*);
AddPlayerFn g_add_player_original = nullptr;
DeriveFn g_derive = nullptr;

void ApplyAfterAddPlayer(std::uint8_t* unit) noexcept {
    __try {
        std::int16_t speed = 0, agility = 0;
        std::uint32_t revision = 0;
        for (auto& mark : g_setups) {
            if (mark.unit != unit || mark.applied) continue;
            if (!GainFor(unit, &speed, &agility, &revision)) return;
            AddGain(unit, speed, agility, revision, "local player");
            mark.applied = true;
            if (g_derive) g_derive(unit); // movement values from the raised levels
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall AddPlayerHook(void* manager, void*, void* unit, int local, int player) {
    g_add_player_original(manager, unit, local, player);
    if ((local & 0xFF) && unit) ApplyAfterAddPlayer(static_cast<std::uint8_t*>(unit));
}

// Site detour on `call 0x675850` inside the stat setup (ECX = unit). Every
// register and the flags are preserved; the trampoline performs the call.
__declspec(naked) void DeriveCallDetour() {
    __asm {
        pushfd
        pushad
        push ecx
        call ApplyMobility
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_derive_original]
    }
}

// --- Observer ---------------------------------------------------------------------

int Snapshot(SdgoOcCustomClientSnapshotV2* output) {
    if (!output || output->size < sizeof(SdgoOcCustomClientSnapshotV2) ||
        output->version != SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION)
        return 0;
    const auto& s = g_state_data;
    SdgoOcCustomClientSnapshotV2 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    snapshot.windows_loaded = g_ui.Alive() && g_ui.entry_ready() ? 1u : 0u;
    snapshot.transport_ready = TransportReady() ? 1u : 0u;
    snapshot.server_oc_mode = s.mode_known && s.server_oc_mode ? 1u : 0u;
    snapshot.profile_present = s.profile_present ? 1u : 0u;
    snapshot.busy = Busy(s) ? 1u : 0u;
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
    snapshot.normal_points_left = static_cast<std::uint32_t>(NormalLeft(s));
    snapshot.over_points_left = static_cast<std::uint32_t>(OverLeft(s));
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        snapshot.normal[stat] = s.profile.normal[stat];
        snapshot.over[stat] = s.profile.over[stat];
        snapshot.base[stat] = s.profile.base[stat];
        snapshot.normal_gain[stat] = s.profile.normal_stat[stat];
        snapshot.over_gain[stat] = s.profile.over_stat[stat];
        snapshot.carrier_delta[stat] = s.carrier.delta[stat];
    }
    snapshot.carrier_present = s.carrier_present ? 1u : 0u;
    snapshot.last_upgrade_success = s.last_upgrade_success;
    snapshot.last_reset_gp = static_cast<std::uint32_t>(s.reset_gp);
    if (g_ui.Visible("entry")) snapshot.windows |= SDGO_OC_WINDOW_ENTRY;
    if (g_ui.Visible("entry") && ChallengeAvailable(s) && !s.challenge_open) snapshot.windows |= SDGO_OC_WINDOW_ENTRY_LIT;
    if (g_ui.Visible("over")) snapshot.windows |= SDGO_OC_WINDOW_OVER;
    if (g_ui.Visible("challenge")) snapshot.windows |= SDGO_OC_WINDOW_CHALLENGE;
    if (g_ui.Visible("confirm")) snapshot.windows |= SDGO_OC_WINDOW_CONFIRM;
    snapshot.confirm_text_id = s.confirm != Confirm::none ? s.confirm_text_id : 0;
    snapshot.challenge_items = s.items.count;
    for (std::size_t slot = 0; slot < s.items.count && slot < 10; ++slot) {
        snapshot.challenge_item_ids[slot] = s.items.items[slot].item;
        if (s.item_selected[slot]) snapshot.challenge_selected |= 1u << slot;
    }
    snapshot.mobility_applied = g_mobility.applied;
    snapshot.mobility_unit_number = g_mobility.unit_number;
    snapshot.mobility_dash_base = g_mobility.dash_base;
    snapshot.mobility_dash_before = g_mobility.dash_before;
    snapshot.mobility_dash = g_mobility.dash;
    snapshot.mobility_motive_base = g_mobility.motive_base;
    snapshot.mobility_motive_before = g_mobility.motive_before;
    snapshot.mobility_motive = g_mobility.motive;
    snapshot.mobility_speed_gain = g_mobility.speed;
    snapshot.mobility_agility_gain = g_mobility.agility;
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return 1;
}

int Action(const SdgoOcCustomClientActionV2& request) {
    if (GetCurrentThreadId() != g_ui_thread && g_ui_thread) return 0;
    auto& s = g_state_data;
    switch (request.action) {
    case 1:
        if (!s.mode_known) Queue(Pending::mode);
        else Queue(Pending::profile);
        return 1;
    case 2:
        // GameTest preparation: save one more normal point on a stat directly.
        if (request.stat >= stat_count || !s.profile_present || Busy(s) || NormalLeft(s) <= 0) return 0;
        s.request_over = false;
        s.request_values = s.profile.normal;
        ++s.request_values[request.stat];
        Queue(Pending::set_normal);
        return 1;
    case 9:
        // Observer-only: work on a unit without the native page (GameTest).
        if (!request.argument64) return 0;
        if (s.unit_uid != request.argument64) s.profile_present = false;
        s.unit_uid = request.argument64;
        ArmTimer();
        if (!s.mode_known) Queue(Pending::mode);
        else Queue(Pending::profile);
        return 1;
    default:
        return 0;
    }
}

int Click(const char* window, const wchar_t* name) {
    if (GetCurrentThreadId() != g_ui_thread && g_ui_thread) return 0;
    void* control = g_ui.Control(window, name);
    if (!control || !g_ui.Alive()) return 0;
    void* service = static_cast<std::uint8_t*>(g_ui.component()) - kPageComponent;
    const bool check = name && !std::wcsncmp(name, L"UseItemCheck", 12);
    PageCallbackHook(service, check ? kCheckClickEvent : kButtonClickEvent, 0, control);
    return 1;
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientGetSnapshotV2(SdgoOcCustomClientSnapshotV2* output) {
    __try {
        return Snapshot(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientActionV2(const SdgoOcCustomClientActionV2* request) {
    if (!request || request->size < sizeof(*request) || request->version != SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION)
        return 0;
    __try {
        return Action(*request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientClickV2(const char* window, const wchar_t* control) {
    __try {
        return Click(window, control);
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
    if (!g_bindings.Bind(api, mod)) return 0;
    g_session = reinterpret_cast<NativeSession>(api->Symbol(mod, "storage.session"));
    g_native_send = reinterpret_cast<NativeSend>(api->Symbol(mod, "net.send_packet"));
    g_profile = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "hangar.profile_global"));
    g_grid = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "oc_custom.grid_singleton"));
    g_unit_manager = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "game.unit_manager"));
    if (!g_session || !g_native_send || !g_profile || !g_grid || !g_unit_manager) return 0;
    HMODULE gift = GetModuleHandleW(L"friend_unit_gift_client.dll");
    if (gift) {
        g_send = reinterpret_cast<ClientExtensionSendFn>(
            GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));
        g_state = reinterpret_cast<ClientExtensionStateFn>(
            GetProcAddress(gift, SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_STATE_EXPORT));
    }
    if (!g_send || !g_state) {
        api->Log(mod, SDGO_LOG_ERROR, "friend_unit_gift_client is required for OC lobby requests; OC inactive");
        return 0;
    }
    g_upgrade_vtable = api->Symbol(mod, "oc_custom.client_upgrade_vtable");
    if (!g_upgrade_vtable) {
        Log(SDGO_LOG_WARN, "oc: the factory upgrade service vtable symbol is missing");
        return 0;
    }
    if (!api->HookCode(mod, "oc_custom.client_upgrade_init", reinterpret_cast<void*>(&PageInitHook),
                       reinterpret_cast<void**>(&g_init_original)) ||
        !api->HookCode(mod, "oc_custom.client_upgrade_callback", reinterpret_cast<void*>(&PageCallbackHook),
                       reinterpret_cast<void**>(&g_callback_original)) ||
        !api->HookCode(mod, "oc_custom.client_upgrade_update", reinterpret_cast<void*>(&PageUpdateHook),
                       reinterpret_cast<void**>(&g_update_original)))
        return 0;
    if (api->SettingInt(mod, "BattleMobility", 1)) {
        g_derive = reinterpret_cast<DeriveFn>(api->Symbol(mod, "oc_custom.client_unit_derive"));
        if (!g_derive || !api->HookSite ||
            !api->HookSite(mod, "oc_custom.client_unit_derive_call", reinterpret_cast<void*>(&DeriveCallDetour),
                           &g_derive_original) ||
            !api->HookCode(mod, "oc_custom.client_add_player", reinterpret_cast<void*>(&AddPlayerHook),
                           reinterpret_cast<void**>(&g_add_player_original))) {
            Log(SDGO_LOG_WARN, "oc: the battle unit setup hooks are unavailable; speed and agility gains inactive");
        }
    }
    api->Log(mod, SDGO_LOG_INFO,
             "ready: original OC windows on the factory upgrade page; battle speed/agility gains on the local unit");
    return 1;
}
