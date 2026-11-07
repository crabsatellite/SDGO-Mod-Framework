// Over-custom (OC) six-stat enhancement, client side.
//
// Lobby: a dual-system factory. The native tabs keep their pages untouched;
// the restored 终极强化 tab (after 特殊扩张) is a page of its own built from
// the client package's original OC layouts (see original_ui.h): 0800 (OVER
// CUSTOM level track, ITEM box and the OVER CUSTOM challenge entry) in the
// upgrade page's slot and 0794 with the unit's six dimensions drawn on its
// hexagon (radar.h), plus the 0813 challenge window and the 0819
// confirmation. Their texts are the
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
#include "radar.h"
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

template <class T>
void PutValue(void* pointer, std::size_t offset, T value) {
    std::memcpy(static_cast<std::uint8_t*>(pointer) + offset, &value, sizeof(value));
}
using RefreshGeometryFn = void(__thiscall*)(void*);
RefreshGeometryFn g_refresh_geometry = nullptr;

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
const std::uint8_t* GridEntry(std::int32_t id, std::int32_t* type) noexcept;

const std::uint8_t* GridRecord(std::int32_t id, std::int32_t type) noexcept {
    std::int32_t found = 0;
    const auto* record = GridEntry(id, &found);
    return record && found == type ? record : nullptr;
}

const std::uint8_t* GridEntry(std::int32_t id, std::int32_t* type) noexcept {
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
        if (!value) return nullptr;
        *type = value[0];
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
    if (text.empty()) text = fallback;
    // The client table keeps line breaks as the two characters backslash and n.
    for (std::size_t at = text.find(L"\\n"); at != std::wstring::npos; at = text.find(L"\\n", at))
        text.replace(at, 2, L"\n");
    return text;
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

void Log(int level, const char* message);

std::wstring ItemName(std::int32_t item) {
    // Item records carry their name at +5 like the other client tables.
    std::int32_t type = 0;
    auto name = RecordText(GridEntry(item, &type), 5, 50);
    static bool traced = false;
    if (!traced) {
        traced = true;
        char line[96];
        std::snprintf(line, sizeof(line), "oc challenge item %d is client table type %d (name %s)", item, type,
                      name.empty() ? "missing" : "found");
        Log(SDGO_LOG_INFO, line);
    }
    return name.empty() ? std::to_wstring(item) : name;
}

// The current client retired the OC challenge items: their table entries read
// 「不可用」. Such an item is described by its effect from the item data.
// The challenge adds ProbabilityPlus to a roll out of 10000: 100 is one percent.
std::wstring Percent(std::int32_t hundredths) {
    std::wstring text = std::to_wstring(hundredths / 100);
    if (const int part = hundredths % 100) {
        text += L"." + std::wstring(part < 10 ? L"0" : L"") + std::to_wstring(part);
        while (text.back() == L'0') text.pop_back();
    }
    return text + L"%";
}

std::wstring ItemLabel(const ChallengeItem& item, bool brief = false) {
    auto name = ItemName(item.item);
    if (!name.empty() && name != std::to_wstring(item.item) && name != L"不可用") return name;
    std::wstring label;
    if (item.probability_plus > 0) label = (brief ? L"+" : L"成功率+") + Percent(item.probability_plus);
    if (item.lv_prevention) label += label.empty() ? L"等级保护" : L" 等级保护";
    return label.empty() ? std::to_wstring(item.item) : label;
}

// --- State ----------------------------------------------------------------

enum class Pending : std::uint8_t { none, mode, profile, set_normal, set_over, reset_cost, reset, challenge, items, owned };
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
    // Every OC item the account holds, for the ITEM box of the OC tab page.
    ChallengeItemsReply owned{};
    bool owned_wanted = false;
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

void RenderWindows(bool active, const ui::EntryView& entry, const ui::OverView& over, const ui::InfoView& info,
                   const ui::ChallengeView& challenge, const ui::ConfirmView& confirm) noexcept {
    __try {
        if (active) g_ui.Render(entry, over, info, challenge, confirm);
        else g_ui.HideAll();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HideUnderWindows(bool oc_page) noexcept;
void RestoreHidden() noexcept;
void ForgetHidden() noexcept;
void SetTabLook(bool oc) noexcept;
bool OcModeActive(bool active);
bool UpgradeTabSelected() noexcept;
void ApplyOcTitle(bool oc_tab) noexcept;
bool g_oc_page_shown = false; // the OC tab page is on screen (factory actions hidden)

void Render() {
    if (!g_ui.Alive()) return;
    const auto& s = g_state_data;
    // Every factory page service stays active while the factory is open; the
    // tab menu decides which page is shown. The OC content only runs on the
    // 经验值转换 tab's slot (the OC tab selects it natively first).
    const bool active = PageActive() && UpgradeTabSelected();
    const bool oc = active && s.mode_known && s.server_oc_mode && s.profile_present;
    const bool idle = !Busy(s) && s.confirm == Confirm::none;
    // Two systems, separate pages: the native tabs stay purely native, and the
    // OC tab shows only OC content (0800 in the upgrade page's place with the
    // OVER CUSTOM entry, 0794 beside it), as in the dual-system factory.
    const bool oc_tab = oc && OcModeActive(active);
    ui::EntryView entry;
    entry.visible = oc_tab;
    entry.lit = entry.visible && ChallengeAvailable(s) && !s.challenge_open;
    ui::OverView over;
    // The allocation window is the OC tab's content; it never showed with the challenge window.
    over.visible = oc_tab && !s.challenge_open;
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
    // The OC tab's left page: OVER CUSTOM 1..6 and EX lit up to the unit's OC
    // level (OC level 9 is OVER CUSTOM1), and the OC items the account holds.
    ui::InfoView info;
    info.visible = oc_tab; // stays while the challenge window covers 0794
    radar::Shape shape;
    shape.visible = over.visible;
    if (oc_tab) {
        for (std::size_t level = 0; level < ui::kLevelIcons; ++level) info.level[level] = s.profile.oc_level >= 9 + level;
        for (std::size_t slot = 0; slot < ui::kInfoItems && slot < s.owned.count; ++slot) {
            const auto& item = s.owned.items[slot];
            info.item[slot] = true;
            info.item_text[slot] = ItemLabel(item, true) + L" x" + std::to_wstring(item.count);
        }
        // All six dimensions on the 0794 hexagon: the unit's native stats and
        // the stats with its OC points, each against the OC stat cap.
        for (std::size_t stat = 0; stat < stat_count; ++stat) {
            const float cap = static_cast<float>(kStatCaps[stat]);
            shape.base[stat] = cap > 0 ? static_cast<float>(s.profile.base[stat]) / cap : 0.0f;
            shape.total[stat] = cap > 0 ? static_cast<float>(StatValue(s, stat, 0, 0)) / cap : 0.0f;
        }
    }
    radar::Set(shape);
    ui::ChallengeView challenge;
    challenge.visible = oc && s.challenge_open;
    if (challenge.visible) {
        challenge.title = Text(126631, L"终极强化");
        challenge.start = Text(126708, L"开始");
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
            challenge.item_text[slot] = ItemLabel(item) + L" x" + std::to_wstring(item.count);
        }
    }
    ui::ConfirmView confirm;
    confirm.visible = active && s.confirm != Confirm::none;
    confirm.line0 = s.confirm_line0;
    confirm.line1 = s.confirm_line1;
    confirm.cancel = s.confirm != Confirm::message;
    RenderWindows(active, entry, over, info, challenge, confirm);
    if (active) {
        SetTabLook(oc_tab);
        HideUnderWindows(oc_tab);
    } else {
        RestoreHidden();
    }
    ApplyOcTitle(active && oc_tab);
    g_oc_page_shown = active && oc_tab;
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
    // The ITEM box refresh is background bookkeeping: it never replaces the
    // result of the player's last OC command.
    if (static_cast<Command>(command) == Command::owned_items) {
        ChallengeItemsReply reply;
        if (static_cast<WireStatus>(status) == WireStatus::success && payload && ReadChallengeItemsReply(payload, bytes, &reply))
            s.owned = reply;
        Render();
        return;
    }
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
            } else if (reply.lv_prevention) {
                // A level-protection item kept the level.
                ShowMessage(126690, Format(Text(126690, L"终极强化失败.\n当前机体等级的经验值下降为%s."), L"s",
                                           LevelName(reply.oc_level)));
            } else {
                ShowMessage(126691, Format(Text(126691, L"终极强化失败.\n机体等級下降为%s."), L"s",
                                           LevelName(reply.oc_level)));
            }
            if (reply.ret_val == 0) RefreshActiveUnit();
            s.owned_wanted = true; // the challenge may have used OC items
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
    if (s.queued == Pending::none && s.owned_wanted && s.mode_known && s.server_oc_mode && s.unit_uid) {
        s.queued = Pending::owned;
        s.owned_wanted = false;
    }
    if (s.queued == Pending::none || s.outstanding != Pending::none || !TransportIdle()) return;
    const auto next = s.queued;
    s.queued = Pending::none;
    std::array<std::uint8_t, 64> payload{};
    switch (next) {
    case Pending::mode:
        SendCommand(Command::mode, nullptr, 0, Pending::mode);
        break;
    case Pending::profile:
    case Pending::items:
    case Pending::owned: {
        UnitRequest request;
        request.unit_uid = s.unit_uid;
        WriteUnitRequest(payload.data(), request);
        const auto command = next == Pending::items   ? Command::challenge_items
                             : next == Pending::owned ? Command::owned_items
                                                      : Command::get_profile;
        SendCommand(command, payload.data(), kUnitRequestBytes, next);
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
        if (g_ui.Loaded() && !g_ui.Alive()) {
            ForgetHidden(); // the page and its siblings are gone
            radar::Forget();
            g_ui.Detach();
        }
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
    // The current page keeps its unit-view buttons along y 590..612 where the
    // OC-era layout put the reset bar; the allocation window sits 40 px higher.
    if (void* over = g_ui.window_dialog(ui::window_over)) {
        PutValue<float>(over, 0x14C, Read<float>(over, 0x14C) - 40.0f);
        if (g_refresh_geometry) g_refresh_geometry(over);
    }
    const char* chart_failure = nullptr;
    const bool chart = radar::Install(g_ui.window_dialog(ui::window_over), &chart_failure);
    auto& s = g_state_data;
    char trace[320];
    std::snprintf(trace, sizeof(trace),
                  "oc original windows attached: 0794/0800/0813/0819 bound, OVER CUSTOM entry restored; chart %s; uid=%llu "
                  "carrier=%u text126631=%s",
                  chart ? "installed" : (chart_failure ? chart_failure : "unavailable"),
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

// --- OC tab and overlap --------------------------------------------------------
//
// The OC-era factory had a 终极强化 tab (OverCustomRadio in uirs\0801/1028)
// whose page showed the allocation window where the current pages show the
// factory inventory. The tab is re-created in the current tab bar after
// 经验值转换 (the bar is reflowed to fit eight tabs); it opens the upgrade page
// in OC mode. While an OC window is open, native dialogs lying under it (the
// inventory) are hidden and restored when it closes.

using TabCallback = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
using TabUpdate = void(__thiscall*)(void*);
TabCallback g_tab_callback_original = nullptr;
TabUpdate g_tab_update_original = nullptr;
void* g_tab_vtable = nullptr;
constexpr std::uint32_t kRadioEvent = 0x301;
constexpr std::size_t kTabRadios = 0xD4;   // seven radio controls cached by the tab menu service
constexpr std::size_t kTabRadioCount = 7;

struct OcTab {
    void* service = nullptr;
    void* dialog = nullptr;
    void* button = nullptr;
    void* unit_up = nullptr;
    void* selected = nullptr; // the native tab radio the menu last selected
    float top = 0.0f;         // the tab row's y; a selected tab stands 16 px higher
    bool raised = false;
    bool mode = false;        // the upgrade page shows the OC tab content
    bool requested = false;   // entering OC mode once the upgrade page is active
};
OcTab g_tab;
std::uint32_t g_tab_clicks_accepted = 0;

bool IsTabRadio(void* control) noexcept {
    if (!control || !g_tab.service) return false;
    __try {
        for (std::size_t index = 0; index < kTabRadioCount; ++index)
            if (Read<void*>(g_tab.service, kTabRadios + index * sizeof(void*)) == control) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// The native page in the 经验值转换 slot is the one the menu selected. Without
// a recorded selection (no tab menu yet), the page dialog's own visibility,
// which the menu toggles, tells.
bool UpgradeTabSelected() noexcept {
    if (g_tab.service && g_tab.unit_up && g_tab.selected) return g_tab.selected == g_tab.unit_up;
    __try {
        return g_ui.page_hidden() || (g_ui.page_dialog() && Read<std::uint8_t>(g_ui.page_dialog(), 0x162));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct HiddenDialog {
    void* dialog = nullptr;
    std::uint8_t visible = 0;
};
std::array<HiddenDialog, 16> g_hidden{};
std::size_t g_hidden_count = 0;

struct Rect {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
};

Rect RectOf(void* dialog) {
    Rect rect;
    rect.left = Read<float>(dialog, 0x148);
    rect.top = Read<float>(dialog, 0x14C);
    rect.right = rect.left + static_cast<float>(Read<int>(dialog, 0x154));
    rect.bottom = rect.top + static_cast<float>(Read<int>(dialog, 0x158));
    return rect;
}

void ForgetHidden() noexcept { g_hidden_count = 0; }

void RestoreHidden() noexcept {
    __try {
        for (std::size_t index = 0; index < g_hidden_count; ++index)
            if (g_hidden[index].dialog) PutValue<std::uint8_t>(g_hidden[index].dialog, 0x162, g_hidden[index].visible);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_hidden_count = 0;
}

bool OwnDialog(void* dialog) {
    if (dialog == g_ui.page_dialog()) return true;
    for (int window = 0; window < ui::window_count; ++window)
        if (dialog == g_ui.window_dialog(static_cast<ui::Window>(window))) return true;
    return false;
}

// The factory inventory service (CServiceFactoryRoomInventory) draws the
// item grid where the OC-era OC tab showed the allocation window. Its dialogs
// sit in the service's dialog map (service +0x30, std::map<wstring, dialog*>,
// the map the service loader files dialogs in); all factory dialogs are
// top-level, so they are found there.
using InventoryUpdate = void(__thiscall*)(void*);
InventoryUpdate g_inventory_update_original = nullptr;
void* g_inventory_vtable = nullptr;
// Several pages host their own inventory instance; every instance updated in
// the last two seconds is checked (only dialogs under the OC windows are hidden).
struct InventorySeen {
    void* service = nullptr;
    ULONGLONG tick = 0;
};
std::array<InventorySeen, 4> g_inventories{};

void __fastcall InventoryUpdateHook(void* service, void*) {
    g_inventory_update_original(service);
    __try {
        if (Read<void*>(service) != g_inventory_vtable) return;
        const auto now = GetTickCount64();
        InventorySeen* slot = nullptr;
        for (auto& seen : g_inventories)
            if (seen.service == service) slot = &seen;
        if (!slot)
            for (auto& seen : g_inventories)
                if (!slot || seen.tick < slot->tick) slot = &seen;
        slot->service = service;
        slot->tick = now;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The factory header (uirs\0804.uir, "FACTORY" and the tab name). A native
// tab switch posts 0x2342 with the tab index; the header keeps it at +0xCC and
// writes that tab's title into TabNameStatic (+0xC8), then resets +0xCC to -1.
// On the OC tab the title is the client's own 126631 「终极强化」; leaving
// through a native tab posts that tab's title again.
using HeaderUpdate = void(__thiscall*)(void*);
HeaderUpdate g_header_update_original = nullptr;
void* g_header_vtable = nullptr;
void* g_header = nullptr;
ULONGLONG g_header_tick = 0;
bool g_title_applied = false;
constexpr std::size_t kHeaderTitle = 0xC8;
constexpr std::size_t kHeaderPendingTab = 0xCC;

void __fastcall HeaderUpdateHook(void* service, void*) {
    g_header_update_original(service);
    __try {
        if (Read<void*>(service) != g_header_vtable) return;
        if (g_header != service) g_title_applied = false;
        g_header = service;
        g_header_tick = GetTickCount64();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The factory main page actions under the unit (uirs\0803.uir SaveBtn,
// ReturnBtn) act on the native page of the current tab; on the OC tab that is
// the hidden 经验值转换 page, so they are hidden there after every native
// update and get their own visibility back when the OC tab is left.
using FactoryUpdate = void(__thiscall*)(void*);
FactoryUpdate g_factory_update_original = nullptr;
void* g_factory_vtable = nullptr;
struct HiddenAction {
    void* control = nullptr;
    std::uint8_t visible = 0;
    std::uint8_t enabled = 0;
};
std::array<HiddenAction, 2> g_hidden_actions{};
constexpr std::size_t kFactoryActions[2] = {0xA8 + 0x3C, 0xA8 + 0x40}; // SaveBtn, ReturnBtn

void RestoreFactoryActions() noexcept {
    __try {
        for (auto& action : g_hidden_actions) {
            if (!action.control) continue;
            PutValue<std::uint8_t>(action.control, 0x162, action.visible);
            PutValue<std::uint8_t>(action.control, 0x163, action.visible);
            PutValue<std::uint8_t>(action.control, 0x167, action.enabled);
            action.control = nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_hidden_actions = {};
    }
}

void __fastcall FactoryUpdateHook(void* service, void*) {
    g_factory_update_original(service);
    __try {
        if (Read<void*>(service) != g_factory_vtable) return;
        if (!g_oc_page_shown) {
            RestoreFactoryActions();
            return;
        }
        for (std::size_t index = 0; index < g_hidden_actions.size(); ++index) {
            void* control = Read<void*>(service, kFactoryActions[index]);
            if (!control) continue;
            auto& action = g_hidden_actions[index];
            if (action.control != control) {
                action.control = control;
                action.visible = Read<std::uint8_t>(control, 0x162);
                action.enabled = Read<std::uint8_t>(control, 0x167);
            }
            PutValue<std::uint8_t>(control, 0x162, 0);
            PutValue<std::uint8_t>(control, 0x163, 0);
            PutValue<std::uint8_t>(control, 0x167, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 0: no live header, 1: a native title is still pending, 2: ready (title set).
int HeaderTitle(void** title) noexcept {
    __try {
        if (!g_header || GetTickCount64() - g_header_tick > 2000 || Read<void*>(g_header) != g_header_vtable) return 0;
        *title = Read<void*>(g_header, kHeaderTitle);
        if (!*title) return 0;
        return Read<std::int32_t>(g_header, kHeaderPendingTab) != -1 ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool SetControlText(void* control, const ui::NativeString* text) noexcept {
    __try {
        auto** vtable = Read<void**>(control);
        reinterpret_cast<void(__thiscall*)(void*, const ui::NativeString*)>(vtable[0x88 / sizeof(void*)])(control, text);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyOcTitle(bool oc_tab) noexcept {
    void* title = nullptr;
    const int state = oc_tab ? HeaderTitle(&title) : 0;
    // A native title still pending (the switch into this slot) lands first.
    if (!oc_tab || state == 1) {
        g_title_applied = false;
        return;
    }
    if (state != 2 || g_title_applied) return;
    const auto text = Text(126631, L"终极强化");
    const auto native = ui::ViewOf(text);
    g_title_applied = SetControlText(title, &native);
}

// Map node: left, parent, right, key (std::wstring, 0x1C bytes) at +0x0C,
// dialog at +0x28, color +0x2C, isnil +0x2D.
bool Readable(const void* pointer, std::size_t bytes) noexcept {
    if (!pointer) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    const auto end = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
    return static_cast<const std::uint8_t*>(pointer) + bytes <= end;
}

std::size_t ServiceDialogs(void* service, void** out, std::size_t capacity) {
    std::size_t count = 0;
    auto* tree = static_cast<std::uint8_t*>(service) + 0x30;
    for (std::size_t slot = 0; slot < 3 && !count; ++slot) {
        auto* head = Read<std::uint8_t*>(tree, slot * 4);
        // Only one slot holds the head; the others are padding or the size.
        if (!Readable(head, 0x30) || Read<std::uint8_t>(head, 0x2D) != 1) continue;
        std::uint8_t* stack[32];
        std::size_t depth = 0;
        auto* root = Read<std::uint8_t*>(head, 4);
        if (root && root != head) stack[depth++] = root;
        std::size_t guard = 0;
        while (depth && count < capacity && ++guard < 64) {
            auto* node = stack[--depth];
            if (!node || node == head || !Readable(node, 0x30) || Read<std::uint8_t>(node, 0x2D)) continue;
            void* dialog = Read<void*>(node, 0x28);
            if (Readable(dialog, 0x170)) out[count++] = dialog;
            for (std::size_t child : {0u, 8u}) {
                auto* next = Read<std::uint8_t*>(node, child);
                if (next && next != head && depth < 32) stack[depth++] = next;
            }
        }
    }
    return count;
}

// Hide the inventory dialogs lying mostly under one of the open OC windows
// (the 0800 page on the left and the 0794 window on the right are judged
// separately, so the factory's middle column is never swallowed).
void HideUnderWindows(bool oc_page) noexcept {
    __try {
        std::array<Rect, ui::window_count> areas{};
        std::size_t area_count = 0;
        for (int window = 0; window < ui::window_count; ++window) {
            const auto id = static_cast<ui::Window>(window);
            // The OC tab owns both page areas even while the challenge window
            // stands in front of 0794, so native dialogs never surface there.
            const bool page_area = oc_page && (id == ui::window_over || id == ui::window_info);
            if ((!g_ui.window_shown(id) && !page_area) || !g_ui.window_dialog(id)) continue;
            areas[area_count++] = RectOf(g_ui.window_dialog(id));
        }
        if (!area_count) {
            RestoreHidden();
            return;
        }
        static bool traced = false;
        void* dialogs[16]{};
        std::size_t count = 0;
        const auto now = GetTickCount64();
        std::size_t live = 0;
        for (const auto& seen : g_inventories)
            if (seen.service && now - seen.tick < 2000) {
                ++live;
                count += ServiceDialogs(seen.service, dialogs + count, 16 - count);
            }
        static bool reported = false;
        if (!reported) {
            reported = true;
            char line[120];
            std::snprintf(line, sizeof(line), "oc windows open: %u live inventory instance(s), %u dialog(s)",
                          static_cast<unsigned>(live), static_cast<unsigned>(count));
            Log(SDGO_LOG_INFO, line);
        }
        for (std::size_t index = 0; index < count; ++index) {
            void* child = dialogs[index];
            if (!child || OwnDialog(child)) continue;
            const auto rect = RectOf(child);
            if (rect.empty()) continue;
            const float width = rect.right - rect.left, height = rect.bottom - rect.top;
            float covered = 0.0f;
            for (std::size_t area = 0; area < area_count; ++area) {
                const float overlap_w = std::min(rect.right, areas[area].right) - std::max(rect.left, areas[area].left);
                const float overlap_h = std::min(rect.bottom, areas[area].bottom) - std::max(rect.top, areas[area].top);
                if (overlap_w > 0 && overlap_h > 0) covered = std::max(covered, overlap_w * overlap_h);
            }
            if (!traced && Read<std::uint8_t>(child, 0x162)) {
                char line[200];
                std::snprintf(line, sizeof(line), "oc inventory dialog %p at %.0f,%.0f %.0fx%.0f visible=%u (of %u)", child,
                              rect.left, rect.top, width, height, Read<std::uint8_t>(child, 0x162), static_cast<unsigned>(count));
                Log(SDGO_LOG_INFO, line);
            }
            if (covered < 0.5f * width * height) continue;
            traced = true;
            bool known = false;
            for (std::size_t slot = 0; slot < g_hidden_count; ++slot) known = known || g_hidden[slot].dialog == child;
            const auto visible = Read<std::uint8_t>(child, 0x162);
            if (!known && visible && g_hidden_count < g_hidden.size()) g_hidden[g_hidden_count++] = {child, visible};
            if (visible) PutValue<std::uint8_t>(child, 0x162, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The OC tab looks selected like a native tab: the selected skin (sprs\8467,
// its pressed state) and 16 px above the row; 经验值转换 shows unselected.
void SetTabLook(bool oc) noexcept {
    __try {
        if (g_tab.button) {
            PutValue<std::uint16_t>(g_tab.button, 0x21C, oc ? 1 : 0);
            if (g_tab.raised != oc) {
                PutValue<float>(g_tab.button, 0x14C, oc ? g_tab.top - 16.0f : g_tab.top);
                if (g_refresh_geometry) g_refresh_geometry(g_tab.button);
                // Skin records (button +0x1A4..+0x1A8): normal, hover, selected.
                // The raised tab draws the selected skin as its normal state.
                auto** first = Read<void**>(g_tab.button, 0x1A4);
                auto** last = Read<void**>(g_tab.button, 0x1A8);
                if (first && last && last - first >= 3) std::swap(first[0], first[2]);
                g_tab.raised = oc;
            }
        }
        if (g_tab.unit_up && oc) PutValue<std::uint8_t>(g_tab.unit_up, 0x21C, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The OC page takes the slot once the menu has really shown the 经验值转换
// page there (its switch is a posted message), so the page's own visibility
// is what comes back when the OC tab is left.
bool OcModeActive(bool active) {
    if (g_tab.requested && active && !g_tab.mode) {
        bool shown = false;
        __try {
            shown = g_ui.page_hidden() || (g_ui.page_dialog() && Read<std::uint8_t>(g_ui.page_dialog(), 0x162));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (shown) g_tab.mode = true;
    }
    return g_tab.mode && active;
}

// Leaving the OC tab: the native page and the hidden native dialogs come back
// before the menu switches pages, so the menu's own hide/show applies to them.
void ExitOcMode() {
    if (!g_tab.mode && !g_tab.requested) return;
    g_tab.mode = g_tab.requested = false;
    auto& s = g_state_data;
    s.challenge_open = false;
    s.confirm = Confirm::none;
    s.confirm_text_id = 0;
    radar::Shape hidden;
    radar::Set(hidden);
    g_oc_page_shown = false;
    RestoreFactoryActions();
    SetTabLook(false);
    __try {
        g_ui.HideAll();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    RestoreHidden();
}

bool CreateTab(void* service) {
    void* component = static_cast<std::uint8_t*>(service) + kPageComponent;
    void* dialog = Read<void*>(component, 0x28);
    if (!dialog || !g_bindings.add || !g_bindings.find || !g_bindings.skin || !g_bindings.text) return false;
    std::array<void*, kTabRadioCount> radios{};
    for (std::size_t index = 0; index < kTabRadioCount; ++index) {
        radios[index] = Read<void*>(service, kTabRadios + index * sizeof(void*));
        if (!radios[index] || Read<void*>(radios[index], 0x15C) != dialog) return false;
    }
    const std::wstring unit_up_name = L"UnitUpRadio";
    const auto native_unit_up = ui::ViewOf(unit_up_name);
    void* unit_up = g_bindings.find(dialog, &native_unit_up, 12);
    if (!unit_up || Read<void*>(unit_up, 0x15C) != dialog) return false;
    // The dual-system tab bar puts 终极强化 right after the current system's
    // 特殊扩张 (ExpensionCustomSlotRadio); without that tab, after 经验值转换.
    const std::wstring extend_name = L"ExpensionCustomSlotRadio";
    const auto native_extend = ui::ViewOf(extend_name);
    void* anchor = g_bindings.find(dialog, &native_extend, 12);
    if (!anchor || Read<void*>(anchor, 0x15C) != dialog) anchor = unit_up;
    // Reflow: the native radios ordered by position, the OC tab after the anchor.
    std::array<void*, kTabRadioCount> ordered = radios;
    std::sort(ordered.begin(), ordered.end(),
              [](void* a, void* b) { return Read<float>(a, 0x148) < Read<float>(b, 0x148); });
    const int width = Read<int>(unit_up, 0x154);
    const int height = Read<int>(unit_up, 0x158);
    const float top = Read<float>(unit_up, 0x14C);
    const float step = 125.0f;
    float x = 12.0f, oc_x = 0.0f;
    for (void* radio : ordered) {
        PutValue<float>(radio, 0x148, x);
        if (g_refresh_geometry) g_refresh_geometry(radio);
        x += step;
        if (radio == anchor) {
            oc_x = x;
            x += step;
        }
    }
    const std::wstring name = L"OverCustomRadio";
    const auto native_name = ui::ViewOf(name);
    g_bindings.add(dialog, &native_name, static_cast<int>(oc_x), static_cast<int>(top), width, height, 0);
    void* button = g_bindings.find(dialog, &native_name, 4);
    if (!button || Read<void*>(button, 0x15C) != dialog) return false;
    PutValue<std::uint8_t>(button, 0x168, 0);
    // The native tab radios' skins: normal, hover and, as this button's pressed
    // state, the radios' selected skin (their fourth), so the open OC tab looks
    // selected; the caption style matches too.
    auto** first = Read<std::uint8_t**>(unit_up, 0x1A4);
    auto** last = Read<std::uint8_t**>(unit_up, 0x1A8);
    const auto count = first && last && last >= first ? static_cast<std::size_t>(last - first) : 0;
    if (count >= 4) {
        for (std::size_t index : {0u, 1u, 3u}) {
            if (!first[index] || !Read<void*>(first[index])) continue;
            g_bindings.skin(button, reinterpret_cast<const ui::NativeString*>(first[index] + 4), Read<std::uint8_t>(first[index], 0x44),
                            Read<std::uint32_t>(first[index], 0x3C), Read<std::uint32_t>(first[index], 0x40));
        }
    } else {
        for (const wchar_t* path : {L"sprs\\8464.spr", L"sprs\\8465.spr", L"sprs\\8467.spr"}) {
            const std::wstring sprite = path;
            const auto native_sprite = ui::ViewOf(sprite);
            g_bindings.skin(button, &native_sprite, 0, 0, 0);
        }
    }
    std::uint32_t font = 1, color = 0xFFFFFFFF, shadow = 0xFF000000;
    auto** items = Read<std::uint8_t**>(unit_up, 0x190);
    auto** items_end = Read<std::uint8_t**>(unit_up, 0x194);
    if (items && items_end && items_end > items && *items) {
        font = Read<std::uint32_t>(*items, 0x6C);
        color = Read<std::uint32_t>(*items, 0x58);
        shadow = Read<std::uint32_t>(*items, 0x5C);
    }
    const auto caption = Text(126631, L"终极强化");
    const auto native_caption = ui::ViewOf(caption);
    g_bindings.text(button, 0, &native_caption, 2, std::max(0, (height - 14) / 2), std::max(1, width - 4), 14, color, shadow,
                    0, font, 2);
    PutValue<std::uint8_t>(button, 0x162, 1);
    PutValue<std::uint8_t>(button, 0x163, 1);
    PutValue<std::uint8_t>(button, 0x167, 1);
    if (g_refresh_geometry) g_refresh_geometry(button);
    g_tab.service = service;
    g_tab.dialog = dialog;
    g_tab.button = button;
    g_tab.unit_up = unit_up;
    g_tab.top = top;
    g_tab.raised = false;
    // The radio the menu shows checked is the page on screen.
    for (void* radio : radios)
        if (Read<std::uint8_t>(radio, 0x21C)) g_tab.selected = radio;
    char line[200];
    std::snprintf(line, sizeof(line), "oc tab restored in the factory tab bar at x=%.0f (radio skins=%u, caption text %s)", oc_x,
                  static_cast<unsigned>(count), GridRecord(126631, 0x3B) ? "126631" : "fallback");
    Log(SDGO_LOG_INFO, line);
    return true;
}

void __fastcall TabUpdateHook(void* service, void*) {
    g_tab_update_original(service);
    const DWORD error = GetLastError();
    __try {
        if (Read<void*>(service) == g_tab_vtable && Read<std::uint8_t>(service, kServiceActive)) {
            void* component = static_cast<std::uint8_t*>(service) + kPageComponent;
            const bool alive = g_tab.button && g_tab.service == service && Read<void*>(component, 0x28) == g_tab.dialog &&
                               Read<void*>(g_tab.button, 0x15C) == g_tab.dialog;
            if (!alive) {
                g_tab = OcTab{};
                // The OC tab only appears while the server runs OC mode.
                const auto& s = g_state_data;
                if (s.mode_known && s.server_oc_mode) CreateTab(service);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tab = OcTab{};
    }
    SetLastError(error);
}

// The menu callback (FUN_008975C0) ignores tab events unless the menu is
// active, its load state (**(service+0x44+0x14)) is idle and its message queue
// (*(*(service+0x9C+4)+4)) is empty. The OC tab follows the same rule, so a
// click the menu would drop leaves the page exactly as it was.
bool MenuAccepts(void* service, const char* when) noexcept {
    __try {
        auto* base = static_cast<std::uint8_t*>(service);
        const auto active = Read<std::uint8_t>(service, kServiceActive);
        const auto load = Read<std::int32_t>(Read<void*>(base + 0x44, 0x14));
        const auto queued = Read<std::int32_t>(Read<void*>(base + 0x9C, 4), 4);
        const bool accepts = active && load == 0 && queued < 1;
        if (!accepts) {
            char line[160];
            std::snprintf(line, sizeof(line), "oc %s ignored like the menu does: active=%u load=%d queued=%d", when, active,
                          load, queued);
            Log(SDGO_LOG_INFO, line);
        }
        return accepts;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __stdcall TabCallbackHook(void* service, std::uint32_t event, std::uintptr_t second, void* control) {
    if (control && control == g_tab.button && service == g_tab.service) {
        if (event == kButtonClickEvent && MenuAccepts(service, "OC tab click")) {
            g_tab.requested = true;
            g_state_data.owned_wanted = true; // the ITEM box lists the current holdings
            if (g_ui.Alive() && PageActive() && UpgradeTabSelected()) {
                g_tab.mode = true;
                Render();
            } else if (g_tab.unit_up) {
                // Select the 经验值转换 slot natively first (the menu hides the
                // page that was shown), then the OC content takes it over.
                g_tab.selected = g_tab.unit_up;
                g_tab_callback_original(service, kRadioEvent, 0, g_tab.unit_up);
            }
        }
        return;
    }
    if (event == kRadioEvent && service == g_tab.service) {
        if (!MenuAccepts(service, "native tab click")) return;
        ExitOcMode();
        if (IsTabRadio(control)) g_tab.selected = control;
        ++g_tab_clicks_accepted;
    }
    g_tab_callback_original(service, event, second, control);
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
    snapshot.busy = Busy(s) || s.owned_wanted ? 1u : 0u;
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
    if (g_tab.button) snapshot.windows |= SDGO_OC_WINDOW_TAB;
    if (g_tab.mode) snapshot.windows |= SDGO_OC_WINDOW_TAB_ACTIVE;
    snapshot.confirm_text_id = s.confirm != Confirm::none ? s.confirm_text_id : 0;
    snapshot.challenge_items = s.items.count;
    for (std::size_t slot = 0; slot < s.items.count && slot < 10; ++slot) {
        snapshot.challenge_item_ids[slot] = s.items.items[slot].item;
        if (s.item_selected[slot]) snapshot.challenge_selected |= 1u << slot;
    }
    snapshot.hidden_dialogs = static_cast<std::uint32_t>(g_hidden_count);
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
    if (g_ui.Visible("info")) snapshot.windows |= SDGO_OC_WINDOW_INFO;
    snapshot.upgrade_tab_selected = UpgradeTabSelected() ? 1u : 0u;
    snapshot.oc_title = g_title_applied ? 1u : 0u;
    snapshot.tab_clicks_accepted = g_tab_clicks_accepted;
    for (const auto& action : g_hidden_actions)
        if (action.control && !Read<std::uint8_t>(action.control, 0x162)) ++snapshot.actions_hidden;
    snapshot.page_hidden = g_ui.page_hidden() ? 1u : 0u;
    for (std::size_t level = 0; level < ui::kLevelIcons; ++level)
        if (s.profile_present && s.profile.oc_level >= 9 + level) ++snapshot.info_levels;
    snapshot.owned_items = s.owned.count;
    for (std::size_t slot = 0; slot < 3 && slot < s.owned.count; ++slot) snapshot.owned_item_ids[slot] = s.owned.items[slot].item;
    snapshot.radar_frames = radar::Frames();
    const auto& shape = radar::Current();
    for (std::size_t stat = 0; stat < stat_count; ++stat) {
        snapshot.radar_base[stat] = shape.base[stat];
        snapshot.radar_total[stat] = shape.total[stat];
    }
    if (g_profile && *g_profile) {
        std::uint8_t record[kUnitRecordBytes];
        std::memcpy(record, *g_profile + kProfileActiveRecord, sizeof(record));
        snapshot.record_slots_known = Read<std::uint64_t>(record, 8) ? 1u : 0u;
        for (std::size_t slot = 0; slot < kCustomSkillCount; ++slot)
            snapshot.record_custom[slot] = Read<std::uint16_t>(record, kCustomSkillOffset + slot * 2);
    }
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
    if (window && !std::strcmp(window, "tab")) {
        if (!g_tab.button || !g_tab.service || !g_tab.dialog || !name) return 0;
        if (!std::wcscmp(name, L"OverCustomRadio")) {
            TabCallbackHook(g_tab.service, kButtonClickEvent, 0, g_tab.button);
            return 1;
        }
        // A native factory tab (radio, type 12): the tab menu's own radio event.
        const std::wstring radio_name = name;
        const auto native = ui::ViewOf(radio_name);
        void* radio = g_bindings.find(g_tab.dialog, &native, 12);
        if (!radio || Read<void*>(radio, 0x15C) != g_tab.dialog) return 0;
        TabCallbackHook(g_tab.service, kRadioEvent, 0, radio);
        return 1;
    }
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
    radar::SetRenderer(reinterpret_cast<void* const*>(api->Symbol(mod, "oc_custom.client_renderer")));
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
    g_inventory_vtable = api->Symbol(mod, "oc_custom.client_inventory_vtable");
    if (!g_inventory_vtable ||
        !api->HookCode(mod, "oc_custom.client_inventory_update", reinterpret_cast<void*>(&InventoryUpdateHook),
                       reinterpret_cast<void**>(&g_inventory_update_original)))
        Log(SDGO_LOG_WARN, "oc: the factory inventory hook is unavailable; OC windows draw over the inventory");
    g_factory_vtable = api->Symbol(mod, "oc_custom.client_factory_vtable");
    if (!g_factory_vtable ||
        !api->HookCode(mod, "oc_custom.client_factory_update", reinterpret_cast<void*>(&FactoryUpdateHook),
                       reinterpret_cast<void**>(&g_factory_update_original)))
        Log(SDGO_LOG_WARN, "oc: the factory main hook is unavailable; its page actions stay on the OC tab");
    g_header_vtable = api->Symbol(mod, "oc_custom.client_header_vtable");
    if (!g_header_vtable ||
        !api->HookCode(mod, "oc_custom.client_header_update", reinterpret_cast<void*>(&HeaderUpdateHook),
                       reinterpret_cast<void**>(&g_header_update_original)))
        Log(SDGO_LOG_WARN, "oc: the factory header hook is unavailable; the OC tab keeps the page title");
    g_tab_vtable = api->Symbol(mod, "oc_custom.client_tab_menu_vtable");
    g_refresh_geometry = reinterpret_cast<RefreshGeometryFn>(api->Symbol(mod, "friend_unit_gift.ui_refresh_geometry"));
    if (!g_tab_vtable ||
        !api->HookCode(mod, "oc_custom.client_tab_menu_update", reinterpret_cast<void*>(&TabUpdateHook),
                       reinterpret_cast<void**>(&g_tab_update_original)) ||
        !api->HookCode(mod, "oc_custom.client_tab_menu_callback", reinterpret_cast<void*>(&TabCallbackHook),
                       reinterpret_cast<void**>(&g_tab_callback_original))) {
        Log(SDGO_LOG_WARN, "oc: the factory tab menu hooks are unavailable; the OC tab is inactive");
        return 0;
    }
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
