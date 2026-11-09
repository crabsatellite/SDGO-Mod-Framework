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
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "factory_l10n.h"
#include "g1_native.h"
#include "original_ui.h"
#include "radar.h"
#include "room_marks.h"
#include "sdgo/oc_custom_client_observer.h"
#include "sdgo/oc_custom_protocol.h"
#include "sdgo/oc_room_system.h"
#include "sdgo/room_titles_current.h"
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
void LogF(int level, const char* format, ...);
void ArmTimer();

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

// The item model an ITEM box slot shows: the item table's model path (UTF-16 at
// record +0x79, the field the factory pages load into their Item3DRender
// controls). The current client's item table (gdrs\0117) still holds the OC
// challenge items, but flags them disabled (byte +4 = 1, named 「不可用」) and
// does not load them into its runtime tables, so those items fall back to the
// model paths of their rows in that table.
struct ItemModel {
    std::int32_t item;
    const wchar_t* model;
};
constexpr ItemModel kOcItemModels[] = {
    {3240005, L"mdrs\\2977.mod"}, // success rate +100%
    {3240006, L"mdrs\\2977.mod"}, // success rate +0.01%
    {3240030, L"mdrs\\2952.mod"}, // level protection
};

std::wstring ItemModelPath(std::int32_t item) {
    std::int32_t type = 0;
    const auto* record = GridEntry(item, &type);
    if (record && type == 0x39) {
        auto path = RecordText(record, 0x79, 64);
        if (!path.empty()) return path;
    }
    for (const auto& entry : kOcItemModels)
        if (entry.item == item) return entry.model;
    return {};
}

// --- State ----------------------------------------------------------------

enum class Pending : std::uint8_t {
    none, mode, profile, set_normal, set_over, reset_cost, reset, challenge, items, owned,
    g1_rules, g1_categories, g1_get, g1_set, g1_reset,
};
enum class Confirm : std::uint8_t { none, message, allocate, reset, challenge, g1_reset };

struct State {
    std::uint64_t unit_uid = 0;
    std::uint32_t unit_number = 0;
    std::uint8_t record_level = 0;
    bool carrier_present = false;
    Carrier carrier{};
    bool mode_known = false;
    bool server_oc_mode = false;
    bool room_rule = false; // "room" mode: the room title decides OC or the custom skills
    bool mode_reply_seen = false; // the mode reply's flags (G改 1.0) were read, not only a record carrier
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

// G改 1.0: the server's rule set and the active unit's loadout. Requests share
// the OC command queue and wait while it is busy.
struct G1Client {
    bool available = false;
    std::vector<G1Rule> rules;
    std::uint16_t total = 0;
    std::uint16_t next_page = 0;
    bool rules_done = false;
    std::vector<std::wstring> categories;
    bool categories_done = false;
    bool loadout_known = false;
    G1Loadout loadout{};
    bool want_rules = false;
    bool want_categories = false;
    bool want_loadout = false;
    Pending action = Pending::none; // g1_set or g1_reset waiting to be sent
    std::array<std::uint16_t, 4> set_skills{};
    std::int32_t last_ret = 0;
    std::int32_t last_charged = 0;
    std::uint32_t replies = 0;
    std::uint32_t last_command = 0;
    bool mode_asked = false;
};
G1Client g_g1;

// The G改 1.0 page on the OC tab: the style switch, the category the player
// browses and the skills picked for empty slots before 设置保存.
struct G1Page {
    bool shown = false;              // the player switched the OC tab to G改 1.0
    std::uint8_t category = 0;
    std::array<std::uint16_t, 4> pending{};
    std::uint64_t pending_uid = 0;
    std::uint32_t pending_revision = 0xFFFFFFFF;
    std::uint32_t saves = 0;         // 设置保存 presses sent
    std::uint32_t resets = 0;        // 强化槽初始化 confirmations sent
};
G1Page g_g1_page;

// G改 1.0 on the native 强化部件 page (g1_native.h): the player's choice on
// the page's system switch, and the save or reset the native page asked for.
bool g_g1_want = false;
bool g_g1_native_rules_set = false;
Pending g_g1_native_pending = Pending::none;
std::uint32_t g_g1_switch_clicks = 0;

std::wstring G1ErrorText(std::int32_t ret);

struct Mobility {
    std::uint32_t applied = 0;
    std::uint32_t unit_number = 0;
    std::int32_t dash_base = 0, dash_before = 0, dash = 0;
    std::int32_t motive_base = 0, motive_before = 0, motive = 0;
    std::int32_t speed = 0, agility = 0;
};
Mobility g_mobility;

void LogF(int level, const char* format, ...) {
    char buffer[384];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
    va_end(args);
    Log(level, buffer);
}

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
bool G1Busy() {
    return g_g1.want_rules || g_g1.want_categories || g_g1.want_loadout || g_g1.action != Pending::none;
}
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
                   const ui::ChallengeView& challenge, const ui::ConfirmView& confirm, const ui::G1View& g1) noexcept {
    __try {
        if (active) g_ui.Render(entry, over, info, challenge, confirm, g1);
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

// A custom skill's name from the client's own skill table (gdrs\0234, name at +5).
std::wstring SkillName(std::uint16_t skill) {
    std::int32_t type = 0;
    auto name = RecordText(GridEntry(static_cast<std::int32_t>(skill), &type), 5, 50);
    return name.empty() ? L"#" + std::to_wstring(skill) : name;
}

// Skills of one category the unit and user levels allow, in rule order.
std::vector<std::uint16_t> G1Eligible(std::uint8_t category) {
    std::vector<std::uint16_t> skills;
    const auto& g = g_g1;
    for (const auto& rule : g.rules)
        if (rule.category == category && rule.unit_level <= g.loadout.unit_level && rule.user_level <= g.loadout.user_level)
            skills.push_back(rule.skill);
    return skills;
}

bool G1SlotOpen(std::size_t slot) {
    return slot < 2 || g_g1.loadout.normal_max >= slot + 1;
}

// Keeps the picks in step with the server's loadout: a filled slot shows its
// skill, a new loadout revision drops the picks.
void SyncG1Pending() {
    auto& page = g_g1_page;
    const auto& g = g_g1;
    if (!g.loadout_known || g.loadout.unit_uid != g_state_data.unit_uid) return;
    if (page.pending_uid != g.loadout.unit_uid || page.pending_revision != g.loadout.revision) {
        LogF(SDGO_LOG_INFO, "g1 page follows the loadout uid=%llu revision=%u (was %llu/%u)",
             static_cast<unsigned long long>(g.loadout.unit_uid), g.loadout.revision,
             static_cast<unsigned long long>(page.pending_uid), page.pending_revision);
        page.pending = g.loadout.skills;
        page.pending_uid = g.loadout.unit_uid;
        page.pending_revision = g.loadout.revision;
        if (g.loadout.category != 0xFF) page.category = g.loadout.category;
    }
}

// arrows: the page's own picks (no request); idle: no G改 1.0 request waits.
ui::G1View BuildG1View(bool oc_tab, bool arrows, bool idle) {
    ui::G1View view;
    const auto& g = g_g1;
    auto& page = g_g1_page;
    // G改 1.0 lives on the native 强化部件 page now (g1_native.h); the OC tab
    // shows OC only.
    view.offered = false;
    view.style_label = L"G改 1.0";
    view.back_label = Text(126631, L"终极强化");
    view.visible = false;
    (void)page;
    if (!view.visible) return view;
    SyncG1Pending();
    const bool ready = g.rules_done && !g.rules.empty() && g.categories_done && g.loadout_known &&
                       g.loadout.unit_uid == g_state_data.unit_uid;
    const bool filled = ready && (g.loadout.skills[0] || g.loadout.skills[1] || g.loadout.skills[2] || g.loadout.skills[3]);
    const bool picked = ready && page.pending != g.loadout.skills;
    view.header = ready ? L"机体等级 " + std::to_wstring(g.loadout.unit_level) + L" · 用户等级 " +
                              std::to_wstring(g.loadout.user_level)
                        : L"G改 1.0";
    const std::wstring category_name =
        page.category < g.categories.size() ? g.categories[page.category] : std::wstring(L"-");
    // The type line is the 类型 selector: the browsed type (fixed while skills are equipped).
    view.type = L"G改 1.0 · " + (ready ? category_name : std::wstring(L"读取中…"));
    view.category = ready ? category_name : L"读取中…";
    // One category for every slot: it is chosen while the loadout is empty.
    view.category_arrows = ready && arrows && !filled && !picked && g.categories.size() > 1;
    for (std::size_t slot = 0; slot < 4; ++slot) {
        view.slot_label[slot] = L""; // the frame numbers its slots
        if (!ready) continue;
        const auto skill = page.pending[slot];
        if (!G1SlotOpen(slot)) view.slot_value[slot] = L"（未开放）";
        else view.slot_value[slot] = skill ? SkillName(skill) : L"（空）";
        view.slot_arrows[slot] = arrows && G1SlotOpen(slot) && !g.loadout.skills[slot];
    }
    if (!ready) view.notice = L"正在读取 G改 1.0 数据…";
    else if (g.last_ret == g1_ok && filled) view.notice = L"初始化费用 " + std::to_wstring((std::max)(0, g.loadout.reset_cost)) + L" GP";
    else if (g.last_ret == g1_not_reset) view.notice = L"已装备的强化槽需初始化后才能更换";
    else if (g.last_ret == g1_wrong_skill) view.notice = L"机体或用户等级不足";
    else if (g.last_ret == g1_category_diff) view.notice = L"所有强化槽须为同一类型";
    else if (g.last_ret == g1_slot_locked) view.notice = L"该强化槽尚未开放";
    else if (g.last_ret == reset_insufficient_gp) view.notice = L"G币不足";
    else view.notice = L"选择类型和技能后点击设置保存";
    view.save_enabled = ready && idle && picked;
    view.revert_enabled = ready && idle && picked;
    view.reset_enabled = ready && idle && filled;
    return view;
}

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
    // The page's arrows only change local picks; saves and resets wait for the
    // G改 1.0 requests, not for the OC page's background refreshes.
    const ui::G1View g1 = BuildG1View(oc_tab, s.confirm == Confirm::none, s.confirm == Confirm::none && !G1Busy());
    // The G改 1.0 page takes the OC tab's place while it is selected.
    const bool oc_content = oc_tab && !g1.visible;
    ui::EntryView entry;
    entry.visible = oc_content;
    entry.lit = entry.visible && ChallengeAvailable(s) && !s.challenge_open;
    ui::OverView over;
    // The allocation window is the OC tab's content; it never showed with the challenge window.
    over.visible = oc_content && !s.challenge_open;
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
    info.visible = oc_content; // stays while the challenge window covers 0794
    radar::Shape shape;
    shape.visible = over.visible;
    if (oc_tab) {
        for (std::size_t level = 0; level < ui::kLevelIcons; ++level) info.level[level] = s.profile.oc_level >= 9 + level;
        for (std::size_t slot = 0; slot < ui::kInfoItems && slot < s.owned.count; ++slot) {
            const auto& item = s.owned.items[slot];
            info.item[slot] = true;
            info.item_text[slot] = ItemLabel(item, true) + L" x" + std::to_wstring(item.count);
            info.item_model[slot] = ItemModelPath(item.item);
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
    RenderWindows(active, entry, over, info, challenge, confirm, g1);
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

void HandleG1Reply(Command command, std::int32_t status, const std::uint8_t* payload, std::uint32_t bytes) {
    auto& g = g_g1;
    ++g.replies;
    g.last_command = static_cast<std::uint32_t>(command);
    if (static_cast<WireStatus>(status) != WireStatus::success || !payload) {
        g.last_ret = status;
        if (command == Command::g1_rules) g.rules_done = true; // stop paging
        if ((command == Command::g1_set && g_g1_native_pending == Pending::g1_set) ||
            (command == Command::g1_reset && g_g1_native_pending == Pending::g1_reset)) {
            g_g1_native_pending = Pending::none;
            if (command == Command::g1_set) g1_native::SaveResult(false, {});
            else g1_native::ResetResult(false, 0);
            g1_native::SetNotice(L"请求失败，请稍后再试");
        }
        LogF(SDGO_LOG_WARN, "g1 command 0x%X failed with wire status %d", static_cast<unsigned>(command), status);
        return;
    }
    switch (command) {
    case Command::g1_rules: {
        G1RulesReply reply;
        if (!ReadG1RulesReply(payload, bytes, &reply)) break;
        g.last_ret = reply.ret_val;
        if (reply.ret_val != 0 || reply.page != g.next_page) {
            g.rules_done = true;
            break;
        }
        g.total = reply.total;
        for (std::size_t index = 0; index < reply.count; ++index) g.rules.push_back(reply.rules[index]);
        ++g.next_page;
        if (g.rules.size() >= g.total || !reply.count) {
            g.rules_done = true;
            LogF(SDGO_LOG_INFO, "g1 rule set read: %u skill(s) in %u categor(ies)", static_cast<unsigned>(g.rules.size()),
                static_cast<unsigned>(reply.categories));
        } else {
            g.want_rules = true;
        }
        break;
    }
    case Command::g1_categories: {
        G1CategoriesReply reply;
        if (!ReadG1CategoriesReply(payload, bytes, &reply)) break;
        g.last_ret = reply.ret_val;
        g.categories.clear();
        for (std::size_t index = 0; index < reply.count; ++index) {
            const auto& name = reply.names[index];
            std::wstring text;
            for (auto c : name) {
                if (!c) break;
                text += static_cast<wchar_t>(c);
            }
            g.categories.push_back(text);
        }
        g.categories_done = true;
        break;
    }
    case Command::g1_get:
    case Command::g1_set:
    case Command::g1_reset: {
        G1Reply reply;
        if (!ReadG1Reply(payload, bytes, &reply)) break;
        g.last_ret = reply.ret_val;
        g.last_charged = reply.charged_gp;
        if (reply.loadout.unit_uid == g_state_data.unit_uid) {
            g.loadout = reply.loadout;
            g.loadout_known = true;
        }
        if (command != Command::g1_get) {
            LogF(SDGO_LOG_INFO, "g1 %s ret=%d skills=%u/%u/%u/%u charged=%d", command == Command::g1_set ? "set" : "reset",
                reply.ret_val, reply.loadout.skills[0], reply.loadout.skills[1], reply.loadout.skills[2],
                reply.loadout.skills[3], reply.charged_gp);
            // The records the World server sends carry the new loadout.
            if (reply.ret_val == 0) RefreshActiveUnit();
            const Pending native = command == Command::g1_set ? Pending::g1_set : Pending::g1_reset;
            if (g_g1_native_pending == native) {
                g_g1_native_pending = Pending::none;
                const bool ok = reply.ret_val == 0;
                if (command == Command::g1_set) g1_native::SaveResult(ok, reply.loadout.skills);
                else g1_native::ResetResult(ok, reply.charged_gp);
                g1_native::SetNotice(ok ? std::wstring() : G1ErrorText(reply.ret_val));
            }
        }
        break;
    }
    default:
        break;
    }
}

void __cdecl OnReply(void*, std::uint16_t command, std::int32_t status, const std::uint8_t* payload,
                     std::uint32_t bytes) {
    auto& s = g_state_data;
    const auto finished = s.outstanding;
    s.outstanding = Pending::none;
    if (command >= static_cast<std::uint16_t>(Command::g1_rules) &&
        command <= static_cast<std::uint16_t>(Command::g1_reset)) {
        HandleG1Reply(static_cast<Command>(command), status, payload, bytes);
        Render();
        return;
    }
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
            s.room_rule = mode.enabled && (mode.flags & kModeFlagRoomRule) != 0;
            g_g1.available = s.room_rule && (mode.flags & kModeFlagG1) != 0;
            s.mode_reply_seen = true;
            room_marks::SetG1Available(g_g1.available);
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
    // A mode learnt from a record carrier lacks the reply's flags: ask once.
    if (s.queued == Pending::none && s.mode_known && s.room_rule && !s.mode_reply_seen && !g_g1.mode_asked) {
        s.queued = Pending::mode;
        g_g1.mode_asked = true;
    }
    if (s.queued == Pending::none && g_g1.available) {
        auto& g = g_g1;
        if (g.action != Pending::none && s.unit_uid) {
            s.queued = g.action;
            g.action = Pending::none;
        } else if (g.want_categories) {
            s.queued = Pending::g1_categories;
            g.want_categories = false;
        } else if (g.want_rules) {
            s.queued = Pending::g1_rules;
            g.want_rules = false;
        } else if (g.want_loadout && s.unit_uid) {
            s.queued = Pending::g1_get;
            g.want_loadout = false;
        }
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
    case Pending::g1_rules: {
        G1RulesRequest request;
        request.page = g_g1.next_page;
        WriteG1RulesRequest(payload.data(), request);
        SendCommand(Command::g1_rules, payload.data(), kG1RulesRequestBytes, next);
        break;
    }
    case Pending::g1_categories:
        SendCommand(Command::g1_categories, nullptr, 0, next);
        break;
    case Pending::g1_get:
    case Pending::g1_reset: {
        UnitRequest request;
        request.unit_uid = s.unit_uid;
        WriteUnitRequest(payload.data(), request);
        SendCommand(next == Pending::g1_get ? Command::g1_get : Command::g1_reset, payload.data(), kUnitRequestBytes, next);
        break;
    }
    case Pending::g1_set: {
        G1SetRequest request;
        request.unit_uid = s.unit_uid;
        request.skills = g_g1.set_skills;
        request.revision = g_g1.loadout_known && g_g1.loadout.unit_uid == s.unit_uid ? g_g1.loadout.revision : 0;
        WriteG1SetRequest(payload.data(), request);
        SendCommand(Command::g1_set, payload.data(), kG1SetRequestBytes, next);
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

// The G改 1.0 page's actions: picks change only the page, saves and resets
// queue their own request. True when the action was the page's.
bool HandleG1Action(ui::Action action) {
    switch (action) {
    case ui::Action::g1_style: case ui::Action::oc_style: case ui::Action::g1_category_prev: case ui::Action::g1_category_next:
    case ui::Action::g1_slot_prev0: case ui::Action::g1_slot_prev1: case ui::Action::g1_slot_prev2: case ui::Action::g1_slot_prev3:
    case ui::Action::g1_slot_next0: case ui::Action::g1_slot_next1: case ui::Action::g1_slot_next2: case ui::Action::g1_slot_next3:
    case ui::Action::g1_save: case ui::Action::g1_revert: case ui::Action::g1_reset:
        break;
    default:
        return false;
    }
    if (action == ui::Action::g1_style || action == ui::Action::oc_style) {
        g_g1_page.shown = action == ui::Action::g1_style;
        if (g_g1_page.shown && g_g1.available) {
            // Read the rule set once, the loadout every time the page opens.
            if (!g_g1.rules_done || g_g1.rules.empty() || !g_g1.categories_done) {
                g_g1.rules.clear();
                g_g1.total = 0;
                g_g1.next_page = 0;
                g_g1.rules_done = false;
                g_g1.want_rules = true;
                g_g1.want_categories = true;
            }
            g_g1.want_loadout = true;
            ArmTimer();
            PumpQueue();
        }
        Log(SDGO_LOG_INFO, g_g1_page.shown ? "g1 page opened on the OC tab" : "OC page shown again on the OC tab");
        Render();
        return true;
    }
    if (action == ui::Action::g1_category_prev || action == ui::Action::g1_category_next) {
        const auto count = static_cast<int>(g_g1.categories.size());
        if (count > 0) {
            const int step = action == ui::Action::g1_category_next ? 1 : -1;
            g_g1_page.category = static_cast<std::uint8_t>((g_g1_page.category + step + count) % count);
            g_g1_page.pending = g_g1.loadout.skills;
        }
        Render();
        return true;
    }
    if (ui::IsG1SlotPrev(action) || ui::IsG1SlotNext(action)) {
        const auto slot = ui::G1Slot(action);
        auto& page = g_g1_page;
        if (slot < 4 && G1SlotOpen(slot) && !g_g1.loadout.skills[slot]) {
            // The slot's choices: empty, then every skill of the category that
            // no other slot holds.
            std::vector<std::uint16_t> choices{0};
            for (const auto skill : G1Eligible(page.category)) {
                bool taken = false;
                for (std::size_t other = 0; other < 4; ++other) taken = taken || (other != slot && page.pending[other] == skill);
                if (!taken) choices.push_back(skill);
            }
            std::size_t at = 0;
            for (std::size_t index = 0; index < choices.size(); ++index)
                if (choices[index] == page.pending[slot]) at = index;
            const std::size_t count = choices.size();
            at = ui::IsG1SlotNext(action) ? (at + 1) % count : (at + count - 1) % count;
            page.pending[slot] = choices[at];
            LogF(SDGO_LOG_INFO, "g1 page slot %u pick %u of %u choices: skill %u", static_cast<unsigned>(slot + 1),
                 static_cast<unsigned>(at), static_cast<unsigned>(count), page.pending[slot]);
        } else {
            LogF(SDGO_LOG_INFO, "g1 page slot %u is not open for picks (open=%u filled=%u)", static_cast<unsigned>(slot + 1),
                 G1SlotOpen(slot) ? 1u : 0u, slot < 4 ? g_g1.loadout.skills[slot] : 0u);
        }
        Render();
        return true;
    }
    if (action == ui::Action::g1_revert) {
        g_g1_page.pending = g_g1.loadout.skills;
        Render();
        return true;
    }
    if (action == ui::Action::g1_save) {
        if (g_g1.action == Pending::none && g_g1_page.pending != g_g1.loadout.skills) {
            g_g1.set_skills = g_g1_page.pending;
            g_g1.action = Pending::g1_set;
            ++g_g1_page.saves;
            PumpQueue();
        }
        Render();
        return true;
    }
    if (action == ui::Action::g1_reset) {
        ShowConfirm(Confirm::g1_reset, 0,
                    L"强化槽初始化\n" + Text(126649, L"G币:") + L" " + std::to_wstring((std::max)(0, g_g1.loadout.reset_cost)));
        Render();
        return true;
    }
    return true;
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
            else if (kind == Confirm::g1_reset && g_g1.action == Pending::none) {
                g_g1.action = Pending::g1_reset;
                ++g_g1_page.resets;
                PumpQueue();
            }
        }
        Render();
        return;
    }
    if (s.confirm == Confirm::none && HandleG1Action(action)) return;
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
    if (s.carrier_present && (s.carrier.flags & (carrier_oc_mode | carrier_room_rule)) && !s.mode_known) {
        s.mode_known = true;
        s.server_oc_mode = true;
        s.room_rule = (s.carrier.flags & carrier_room_rule) != 0;
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

// --- Room systems -------------------------------------------------------------
//
// The account profile (CAccountData) points at the current room's data at
// +0xEC (title length +0x79, native title +0x7A). The long-title Mod, when
// present, provides the full confirmed title. The last room read stays known
// until the room is left, so the battle start (stat setup) still sees it.
constexpr std::size_t kProfileRoom = 0xEC;
constexpr std::size_t kRoomTitleLength = 0x79, kRoomTitle = 0x7A, kRoomTitleMax = 20;

struct RoomView {
    std::uint8_t* room = nullptr;
    bool known = false;
    RoomSystem system = RoomSystem::parts;
};
RoomView g_room_view;
std::uint32_t g_mobility_room_skips = 0;

std::uint8_t* CurrentRoomData() noexcept {
    __try {
        return g_profile && *g_profile ? Read<std::uint8_t*>(*g_profile, kProfileRoom) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::uint32_t NativeRoomTitle(const std::uint8_t* room, char* text) noexcept {
    __try {
        const std::uint32_t length = room[kRoomTitleLength];
        if (length > kRoomTitleMax) return 0;
        std::memcpy(text, room + kRoomTitle, length);
        text[length] = 0;
        return length;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint32_t FullRoomTitle(char* text, std::uint32_t capacity) {
    if (!g_api || !g_api->ModDirectory) return 0;
    const std::uint32_t length = sdgo::CurrentRoomTitle(g_api->ModDirectory(g_mod), text, capacity);
    return length && length < capacity && text[length] == 0 ? length : 0;
}

void RefreshRoomView() {
    std::uint8_t* room = CurrentRoomData();
    if (!room) {
        g_room_view = RoomView{};
        return;
    }
    char title[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
    std::uint32_t length = FullRoomTitle(title, sizeof(title));
    if (!length) length = NativeRoomTitle(room, title);
    if (!length && !title[0]) {
        if (g_room_view.room != room) g_room_view = RoomView{room, false, RoomSystem::parts};
        return;
    }
    const RoomSystem system = RoomSystemOfTitle(g_api, title, length);
    if (g_room_view.room != room || !g_room_view.known || g_room_view.system != system) {
        char line[200];
        std::snprintf(line, sizeof(line), "oc room view system=%s room=%p", RoomSystemName(system), room);
        Log(SDGO_LOG_INFO, line);
    }
    g_room_view = RoomView{room, true, system};
}

// In "room" mode the OC gains apply only in a room that plays OC.
bool RoomPlaysOc() {
    RefreshRoomView();
    return g_room_view.known && g_room_view.system == RoomSystem::oc;
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    if (g_tick_active || GetCurrentThreadId() != g_ui_thread) return;
    g_tick_active = true;
    __try {
        TraceActiveRecord();
        RefreshRoomView();
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
    if (!ReadCarrier(record, &carrier) || !(carrier.flags & carrier_profile)) return false;
    if (!(carrier.flags & carrier_oc_mode)) {
        if (!(carrier.flags & carrier_room_rule)) return false;
        if (!RoomPlaysOc()) {
            ++g_mobility_room_skips;
            return false;
        }
    }
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

// Diagnostic for room systems: does the local battle unit carry the record's
// own custom skills? The battle server copies them into the battle unit it
// sends (server +0x644) unless the room plays OC; the client unit is searched
// for the record's four UnitSkill values.
constexpr std::size_t kUnitScanBytes = 0x4000;
std::uint32_t g_battle_skills_seen = 0;
std::int32_t g_battle_skills_offset = -1;
std::array<std::uint16_t, kCustomSkillCount> g_battle_skills{};
std::uint8_t g_unit_scan[kUnitScanBytes];

std::size_t CopyUnitBytes(const std::uint8_t* unit) noexcept {
    std::size_t copied = 0;
    for (; copied < kUnitScanBytes; copied += 0x1000) {
        __try {
            std::memcpy(g_unit_scan + copied, unit + copied, 0x1000);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return copied;
}

void NoteBattleSkills(const std::uint8_t* unit) noexcept {
    if (!g_profile || !*g_profile) return;
    std::array<std::uint16_t, kCustomSkillCount> wanted{};
    std::memcpy(wanted.data(), *g_profile + kProfileActiveRecord + kCustomSkillOffset, sizeof(wanted));
    if (!wanted[0] && !wanted[1] && !wanted[2] && !wanted[3]) return;
    const std::size_t copied = CopyUnitBytes(unit);
    std::int32_t found = -1;
    for (std::size_t offset = 0; offset + sizeof(wanted) <= copied; offset += 2)
        if (std::memcmp(g_unit_scan + offset, wanted.data(), sizeof(wanted)) == 0) {
            found = static_cast<std::int32_t>(offset);
            break;
        }
    if (!g_battle_skills_seen || found != g_battle_skills_offset || wanted != g_battle_skills) {
        char line[200];
        std::snprintf(line, sizeof(line), "oc battle unit custom skills %u/%u/%u/%u %s offset=%d (searched %u bytes)",
                      wanted[0], wanted[1], wanted[2], wanted[3], found >= 0 ? "present" : "absent", found,
                      static_cast<unsigned>(copied));
        Log(SDGO_LOG_INFO, line);
    }
    g_battle_skills = wanted;
    g_battle_skills_offset = found;
    ++g_battle_skills_seen;
}

// --- Room-system marks (room list and waiting room) ------------------------------

using ServiceUpdateFn = void(__thiscall*)(void*);
ServiceUpdateFn g_room_list_update_original = nullptr;
ServiceUpdateFn g_titlebar_update_original = nullptr;

bool RoomRuleKnown() {
    const auto& s = g_state_data;
    return s.mode_known && s.room_rule;
}

void LogMark(const char* message) { Log(SDGO_LOG_INFO, message); }

void __fastcall RoomListUpdateHook(void* service, void*) {
    g_room_list_update_original(service);
    if (!RoomRuleKnown()) return;
    room_marks::MarkRoomList(service);
    room_marks::UpdateCreateWindow();
}

// UI events of the PvP create windows (vtable slot 14): the 体系 row arrows.
using CreateEventFn = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
CreateEventFn g_make_room_event_original = nullptr;
CreateEventFn g_make_room_ai_event_original = nullptr;

void __stdcall MakeRoomEventHook(void* service, std::uint32_t event, std::uintptr_t second, void* control) {
    if (RoomRuleKnown() && room_marks::HandleCreateEvent(service, event, control)) return;
    g_make_room_event_original(service, event, second, control);
}

void __stdcall MakeRoomAiEventHook(void* service, std::uint32_t event, std::uintptr_t second, void* control) {
    if (RoomRuleKnown() && room_marks::HandleCreateEvent(service, event, control)) return;
    g_make_room_ai_event_original(service, event, second, control);
}

void __fastcall TitleBarUpdateHook(void* title_bar, void*) {
    g_titlebar_update_original(title_bar);
    if (!RoomRuleKnown()) return;
    RefreshRoomView();
    if (g_room_view.known) room_marks::MarkTitleBar(title_bar, g_room_view.system);
}

extern "C" void __cdecl ApplyMobility(std::uint8_t* unit) {
    __try {
        auto* manager = g_unit_manager ? *g_unit_manager : nullptr;
        if (unit && manager && Read<std::uint8_t*>(manager, kUnitManagerLocal) == unit) NoteBattleSkills(unit);
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
        NoteBattleSkills(unit);
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
    void* status = nullptr;   // StatusRadio: the 强化部件 (custom-skill attribute) tab
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
        l10n::PatchTexts();
        l10n::Header(Read<void*>(service, kHeaderTitle));
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

void ExitButtonOf(void* service);
void** g_service_list = nullptr; // flow.service_managed_pointer

void __fastcall FactoryUpdateHook(void* service, void*) {
    g_factory_update_original(service);
    ExitButtonOf(service);
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
    {
        const std::wstring status_name = L"StatusRadio";
        const auto native_status = ui::ViewOf(status_name);
        void* status = g_bindings.find(dialog, &native_status, 12);
        g_tab.status = status && Read<void*>(status, 0x15C) == dialog ? status : nullptr;
    }
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

// The 强化部件 tab is on screen (its radio is the one the menu selected and the
// OC content is not shown in the 经验值转换 slot).
bool AttributeTabSelected() noexcept {
    return g_tab.service && g_tab.status && g_tab.selected == g_tab.status && !g_tab.mode;
}

std::wstring G1ErrorText(std::int32_t ret) {
    switch (ret) {
    case g1_not_reset: return L"已装备的强化槽需初始化后才能更换";
    case g1_wrong_skill: return L"机体或用户等级不足";
    case g1_category_diff: return L"所有强化槽须为同一类型";
    case g1_slot_locked: return L"该强化槽尚未开放";
    case reset_insufficient_gp: return L"G币不足";
    default: return L"G改 1.0 设置失败（" + std::to_wstring(ret) + L"）";
    }
}

std::wstring g_attr_title;      // the title the Mod set on the 强化部件 tab
std::uint64_t g_attr_title_tick = 0;

void ApplyAttributeTitle(bool shown) noexcept {
    void* title = nullptr;
    const int state = shown ? HeaderTitle(&title) : 0;
    if (!shown || state == 1) {
        g_attr_title.clear();
        return;
    }
    if (state != 2) return;
    const std::wstring text = g1_native::Active() ? L"强化部件 · G改 1.0" : L"强化部件 · G改 2.0";
    // The tab switch can write the native title after the Mod's; set it again
    // now and then while the tab is on screen.
    const auto now = GetTickCount64();
    if (text == g_attr_title && now - g_attr_title_tick < 1000) return;
    const auto native = ui::ViewOf(text);
    if (SetControlText(title, &native)) {
        g_attr_title = text;
        g_attr_title_tick = now;
    }
}

// Game thread, every tab menu frame: G改 1.0 on the native 强化部件 page.
void UpdateAttributePage() noexcept {
    if (!g1_native::Bound()) return;
    auto& g = g_g1;
    const bool shown = AttributeTabSelected();
    void* page = shown ? g1_native::FindPage() : nullptr;
    if (page && g_g1_want && g.available) {
        bool ask = false;
        if ((!g.rules_done || g.rules.empty() || !g.categories_done) && !g.want_rules && g.next_page == 0) {
            g.rules.clear();
            g.total = 0;
            g.want_rules = true;
            g.want_categories = true;
            ask = true;
        }
        if ((!g.loadout_known || g.loadout.unit_uid != g_state_data.unit_uid) && !g.want_loadout) {
            g.want_loadout = true;
            ask = true;
        }
        if (ask) {
            ArmTimer();
            PumpQueue();
        }
    }
    if (g.rules_done && g.categories_done && !g.rules.empty() && !g_g1_native_rules_set) {
        g1_native::SetRules(g.rules, g.categories);
        g_g1_native_rules_set = true;
    }
    g1_native::SetResetCost(g.loadout.reset_cost);
    const bool current = g.loadout_known && g.loadout.unit_uid == g_state_data.unit_uid;
    g1_native::Update(page, g.available, g_g1_want, current ? &g.loadout : nullptr);
    if (page) l10n::CategoryBar(g1_native::CategoryBarService(), g1_native::CategoryNewMask());
    std::array<std::uint16_t, 4> skills{};
    if (g1_native::TakeSave(&skills)) {
        if (g.action == Pending::none && g_g1_native_pending == Pending::none) {
            g.set_skills = skills;
            g.action = Pending::g1_set;
            g_g1_native_pending = Pending::g1_set;
            PumpQueue();
        } else {
            g1_native::SaveResult(false, {});
        }
    }
    if (g1_native::TakeReset()) {
        if (g.action == Pending::none && g_g1_native_pending == Pending::none) {
            g.action = Pending::g1_reset;
            g_g1_native_pending = Pending::g1_reset;
            PumpQueue();
        } else {
            g1_native::ResetResult(false, 0);
        }
    }
    ApplyAttributeTitle(shown && page);
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
            UpdateAttributePage();
            if (g_ui.Alive()) l10n::InfoPage(g_ui.window_dialog(ui::window_info), g_ui.window_shown(ui::window_info));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tab = OcTab{};
    }
    SetLastError(error);
}

// The attribute page's UI events (vtable slot 14, FUN_008AE9F0): the Mod's
// system switch under its left panel.
using AttrEvent = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
AttrEvent g_attr_event_original = nullptr;
using AttrDestroy = void*(__thiscall*)(void*, std::uint32_t);
AttrDestroy g_attr_destroy_original = nullptr;

void __stdcall AttrEventHook(void* page, std::uint32_t event, std::uintptr_t second, void* control) {
    const int which = g1_native::SwitchOf(control);
    if (which) {
        if (event == kButtonClickEvent) {
            const bool want = which == 2;
            if (want != g_g1_want) {
                g_g1_want = want;
                ++g_g1_switch_clicks;
                g1_native::SetNotice(std::wstring());
                if (want) g_g1.want_loadout = true;
                LogF(SDGO_LOG_INFO, "attribute page switched to %s", want ? "G改 1.0" : "G改 2.0");
                PumpQueue();
            }
        }
        return;
    }
    g_attr_event_original(page, event, second, control);
}

void* __fastcall AttrDestroyHook(void* page, void*, std::uint32_t flags) {
    // The factory closes: the shared data manager and the unit record go back
    // to the native data before the page is gone.
    g1_native::Leave();
    g_attr_title.clear();
    return g_attr_destroy_original(page, flags);
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

// The factory's exit button sits in one of the factory main service's dialogs.
std::size_t FactoryDialogs(void* service, void** out, std::size_t capacity) noexcept {
    __try {
        if (Read<void*>(service) != g_factory_vtable) return 0;
        return ServiceDialogs(service, out, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* FindButton(void* dialog, const ui::NativeString* name) noexcept {
    __try {
        return g_bindings.find(dialog, name, 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Every live service (the service manager's list at +0x44).
std::size_t LiveServices(void** out, std::size_t capacity) noexcept {
    std::size_t count = 0;
    __try {
        if (!g_service_list || !*g_service_list) return 0;
        auto* owner = static_cast<std::uint8_t*>(*g_service_list);
        auto* head = Read<std::uint8_t*>(owner, 0x44);
        if (!head) return 0;
        auto* node = Read<std::uint8_t*>(head, 0);
        for (unsigned guard = 0; node && node != head && guard < 512 && count < capacity; ++guard) {
            if (void* service = Read<void*>(node, 8)) out[count++] = service;
            node = Read<std::uint8_t*>(node, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

std::size_t AnyServiceDialogs(void* service, void** out, std::size_t capacity) noexcept {
    __try {
        return ServiceDialogs(service, out, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The factory's exit button (uirs\0802 ExitBtn) belongs to one of the services
// that run with the factory; it is looked up once per factory visit.
void ExitButtonOf(void* service) {
    static void* done = nullptr;
    if (!service || service == done) return;
    done = service;
    void* services[256] = {};
    const std::size_t count = LiveServices(services, 256);
    const std::wstring name = L"ExitBtn";
    const auto native = ui::ViewOf(name);
    for (std::size_t index = 0; index < count; ++index) {
        void* dialogs[16] = {};
        const std::size_t dialog_count = AnyServiceDialogs(services[index], dialogs, 16);
        for (std::size_t at = 0; at < dialog_count; ++at) {
            if (void* control = FindButton(dialogs[at], &native)) {
                l10n::ExitButton(control);
                return;
            }
        }
    }
    Log(SDGO_LOG_INFO, "factory l10n: no exit button among the live services");
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
    snapshot.item_models_shown = g_ui.models_shown();
    snapshot.item_models_loaded = g_ui.models_loaded();
    snapshot.server_room_rule = s.mode_known && s.room_rule ? 1u : 0u;
    snapshot.room_known = g_room_view.known ? 1u : 0u;
    snapshot.room_system = g_room_view.known ? static_cast<std::uint32_t>(g_room_view.system) : 0u;
    snapshot.mobility_room_skips = g_mobility_room_skips;
    snapshot.room_list_marks = room_marks::ListMarks();
    snapshot.title_marks = room_marks::TitleMarks();
    snapshot.title_mark_system = room_marks::TitleSystem();
    snapshot.create_open = room_marks::CreateOpen() ? 1u : 0u;
    snapshot.create_shown = room_marks::CreateShown();
    snapshot.create_toggles = room_marks::CreateToggles();
    snapshot.create_system = room_marks::CreateSystem();
    snapshot.battle_skills_seen = g_battle_skills_seen;
    snapshot.battle_skills_offset = g_battle_skills_offset;
    for (std::size_t slot = 0; slot < kCustomSkillCount; ++slot) snapshot.battle_skills[slot] = g_battle_skills[slot];
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

int G1Snapshot(SdgoOcCustomClientG1SnapshotV1* output) {
    if (!output || output->size < sizeof(SdgoOcCustomClientG1SnapshotV1) || output->version != SDGO_OC_CUSTOM_CLIENT_G1_VERSION)
        return 0;
    const auto& g = g_g1;
    const auto& s = g_state_data;
    SdgoOcCustomClientG1SnapshotV1 snapshot{};
    snapshot.size = sizeof(snapshot);
    snapshot.version = SDGO_OC_CUSTOM_CLIENT_G1_VERSION;
    snapshot.available = g.available ? 1u : 0u;
    snapshot.rules_loaded = g.rules_done && !g.rules.empty() ? 1u : 0u;
    snapshot.rule_count = static_cast<std::uint32_t>(g.rules.size());
    snapshot.category_count = g.categories_done ? static_cast<std::uint32_t>(g.categories.size()) : 0u;
    const bool current = g.loadout_known && g.loadout.unit_uid == s.unit_uid;
    snapshot.loadout_known = current ? 1u : 0u;
    snapshot.unit_uid = s.unit_uid;
    if (current) {
        for (std::size_t slot = 0; slot < 4; ++slot) snapshot.skills[slot] = g.loadout.skills[slot];
        snapshot.normal_max = g.loadout.normal_max;
        snapshot.unit_level = g.loadout.unit_level;
        snapshot.user_level = g.loadout.user_level;
        snapshot.category = g.loadout.category;
        snapshot.reset_count = g.loadout.reset_count;
        snapshot.reset_cost = g.loadout.reset_cost;
        snapshot.revision = g.loadout.revision;
    }
    snapshot.busy = G1Busy() || Busy(s) ? 1u : 0u;
    snapshot.replies = g.replies;
    snapshot.last_ret_val = g.last_ret;
    snapshot.last_charged_gp = g.last_charged;
    snapshot.last_command = g.last_command;
    snapshot.page_shown = g_ui.Visible("g1") ? 1u : 0u;
    snapshot.page_category = g_g1_page.category;
    for (std::size_t slot = 0; slot < 4; ++slot) snapshot.page_pending[slot] = g_g1_page.pending[slot];
    snapshot.page_saves = g_g1_page.saves;
    snapshot.page_resets = g_g1_page.resets;
    // The account profile's active-unit record, as the native pilot copies get it.
    if (g_profile && *g_profile) {
        std::uint8_t record[kUnitRecordBytes];
        std::memcpy(record, *g_profile + kProfileActiveRecord, sizeof(record));
        Carrier carrier{};
        if (Read<std::uint64_t>(record, 8) == s.unit_uid && ReadCarrier(record, &carrier) &&
            (carrier.flags & carrier_g1_profile)) {
            snapshot.carrier_g1 = 1u;
            for (std::size_t slot = 0; slot < 4; ++slot) snapshot.carrier_skills[slot] = carrier.g1_skills[slot];
        }
    }
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return 1;
}

int G1Action(std::uint32_t action, const std::uint16_t* skills) {
    if (GetCurrentThreadId() != g_ui_thread && g_ui_thread) return 0;
    auto& g = g_g1;
    if (!g.available) return 0;
    switch (action) {
    case 1:
        g.rules.clear();
        g.total = 0;
        g.next_page = 0;
        g.rules_done = false;
        g.categories_done = false;
        g.want_categories = true;
        g.want_rules = true;
        g.want_loadout = true;
        break;
    case 2:
        if (!skills || g.action != Pending::none) return 0;
        for (std::size_t slot = 0; slot < 4; ++slot) g.set_skills[slot] = skills[slot];
        g.action = Pending::g1_set;
        break;
    case 3:
        if (g.action != Pending::none) return 0;
        g.action = Pending::g1_reset;
        break;
    case 4:
        g.want_loadout = true;
        break;
    default:
        return 0;
    }
    ArmTimer();
    PumpQueue();
    return 1;
}

int G1RuleAt(std::uint32_t index, SdgoOcCustomClientG1RuleV1* output) {
    if (!output || index >= g_g1.rules.size()) return 0;
    const auto& rule = g_g1.rules[index];
    *output = SdgoOcCustomClientG1RuleV1{rule.skill, rule.category, rule.type, rule.unit_level, rule.user_level,
                                         rule.premium, 0, rule.reset_cost};
    return 1;
}

int G1CategoryAt(std::uint32_t index, wchar_t* name, std::uint32_t capacity) {
    if (!name || !capacity || index >= g_g1.categories.size()) return 0;
    const auto& text = g_g1.categories[index];
    const std::size_t length = (std::min)(text.size(), static_cast<std::size_t>(capacity - 1));
    std::wmemcpy(name, text.c_str(), length);
    name[length] = 0;
    return static_cast<int>(length);
}

int Click(const char* window, const wchar_t* name) {
    if (GetCurrentThreadId() != g_ui_thread && g_ui_thread) return 0;
    if (window && !std::strcmp(window, "create")) return room_marks::ClickCreate(name) ? 1 : 0;
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

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientG1SnapshotV1(SdgoOcCustomClientG1SnapshotV1* output) {
    __try {
        return G1Snapshot(output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientG1ActionV1(std::uint32_t action, const std::uint16_t* skills) {
    __try {
        return G1Action(action, skills);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientG1RuleV1(std::uint32_t index, SdgoOcCustomClientG1RuleV1* output) {
    __try {
        return G1RuleAt(index, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientG1CategoryV1(std::uint32_t index, wchar_t* name,
                                                                          std::uint32_t capacity) {
    __try {
        return G1CategoryAt(index, name, capacity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int G1NativeSnapshot(SdgoOcCustomClientG1NativeV1* out) {
    if (!out || out->size < sizeof(SdgoOcCustomClientG1NativeV1) || out->version != SDGO_OC_CUSTOM_CLIENT_G1_NATIVE_VERSION)
        return 0;
    const auto stats = g1_native::GetStats();
    out->bound = g1_native::Bound() ? 1 : 0;
    out->tab_shown = AttributeTabSelected() ? 1 : 0;
    out->want = g_g1_want ? 1 : 0;
    out->active = g1_native::Active() ? 1 : 0;
    out->rules = g1_native::HasRules() ? 1 : 0;
    out->categories = stats.categories;
    out->entries = stats.entries;
    out->loads = stats.loads;
    out->entered = stats.entered;
    out->left = stats.left;
    out->saves = stats.saves;
    out->resets = stats.resets;
    out->switch_clicks = g_g1_switch_clicks;
    for (std::size_t slot = 0; slot < 4; ++slot) out->shown[slot] = stats.shown[slot];
    out->page = g1_native::FindPage();
    out->switch_g2 = g1_native::SwitchControl(1);
    out->switch_g1 = g1_native::SwitchControl(2);
    out->bar = g1_native::NativeService(1);
    out->list = g1_native::NativeService(2);
    out->save_popup = g1_native::NativeService(3);
    out->reset_popup = g1_native::NativeService(4);
    out->reset_check_popup = g1_native::NativeService(5);
    out->page_idle = g1_native::PageReady() ? 1 : 0;
    wcsncpy_s(out->title, g_attr_title.c_str(), _TRUNCATE);
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoOcCustomClientG1NativeV1(SdgoOcCustomClientG1NativeV1* out) {
    __try {
        return G1NativeSnapshot(out);
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
    g_bindings.log = [](const char* message) { Log(SDGO_LOG_INFO, message); };
    g_session =reinterpret_cast<NativeSession>(api->Symbol(mod, "storage.session"));
    g_native_send = reinterpret_cast<NativeSend>(api->Symbol(mod, "net.send_packet"));
    g_profile = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "hangar.profile_global"));
    g_grid = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "oc_custom.grid_singleton"));
    g_unit_manager = reinterpret_cast<std::uint8_t**>(api->Symbol(mod, "game.unit_manager"));
    g_service_list = static_cast<void**>(api->Symbol(mod, "flow.service_managed_pointer"));
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
    if (!api->HookSlot || !room_marks::Bind(api, mod, &LogMark) ||
        !api->HookSlot(mod, "oc_custom.client_room_list_update", reinterpret_cast<void*>(&RoomListUpdateHook),
                       reinterpret_cast<void**>(&g_room_list_update_original)) ||
        !api->HookSlot(mod, "oc_custom.client_titlebar_update", reinterpret_cast<void*>(&TitleBarUpdateHook),
                       reinterpret_cast<void**>(&g_titlebar_update_original)))
        Log(SDGO_LOG_WARN, "oc: room-system marks unavailable; rooms still play the system their title selects");
    if (!l10n::Bind(api, mod, &g_bindings, &LogMark, g_grid))
        Log(SDGO_LOG_WARN, "oc: factory Chinese labels unavailable");
    if (!g1_native::Bind(api, mod, &LogMark, &g_bindings) ||
        !api->HookSlot(mod, "oc_custom.attr_page_event", reinterpret_cast<void*>(&AttrEventHook),
                       reinterpret_cast<void**>(&g_attr_event_original)) ||
        !api->HookSlot(mod, "oc_custom.attr_page_destroy", reinterpret_cast<void*>(&AttrDestroyHook),
                       reinterpret_cast<void**>(&g_attr_destroy_original)))
        Log(SDGO_LOG_WARN, "oc: G改 1.0 on the native 强化部件 page is unavailable");
    if (!room_marks::BindCreate(api, mod) ||
        !api->HookSlot(mod, "oc_custom.client_make_room_event", reinterpret_cast<void*>(&MakeRoomEventHook),
                       reinterpret_cast<void**>(&g_make_room_event_original)) ||
        !api->HookSlot(mod, "oc_custom.client_make_room_ai_event", reinterpret_cast<void*>(&MakeRoomAiEventHook),
                       reinterpret_cast<void**>(&g_make_room_ai_event_original)))
        Log(SDGO_LOG_WARN, "oc: the create-window 体系 row is unavailable; the room title's !oc still selects OC");
    api->Log(mod, SDGO_LOG_INFO,
             "ready: original OC windows on the factory upgrade page; battle speed/agility gains on the local unit; "
             "room systems follow the room title (!oc) when the server runs room mode");
    return 1;
}
