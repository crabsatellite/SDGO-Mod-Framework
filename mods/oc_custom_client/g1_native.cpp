#include "g1_native.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

namespace sdgo::oc_custom::client::g1_native {
namespace {

// CAttributeMgr (singleton, 0x48 bytes): +4 loaded, +8 category count,
// +0xC vector<Category> (Category 0x14 bytes: vector<EntryState> first/last/
// end at +0/+4/+8, flags at +0x10..+0x13), +0x1C vector<std::wstring> names.
constexpr std::size_t kMgrCount = 0x8, kMgrCategories = 0xC, kMgrNames = 0x1C;
constexpr std::size_t kCategoryBytes = 0x14;
// Client tables: 0261 entries (grid type 0x5D, ids from 3664001), 0x212 bytes.
constexpr std::int32_t kFirstEntryId = 3664001;
constexpr std::uint32_t kEntryType = 0x5D;
constexpr std::size_t kRowBytes = 0x212;
constexpr std::size_t kRowSkill = 0x5, kRowCategory = 0x9, kRowType = 0xA, kRowUnitLevel = 0xB, kRowUserLevel = 0xC,
                      kRowPremium = 0xD, kRowNew = 0x17;
// Profile: current unit record at +0x1A9; its open slot count (+0x88) and four
// u16 custom skills (+0x8A).
constexpr std::size_t kProfileOpenSlots = 0x231, kProfileSkills = 0x233;
// Attribute page: pending packet list +0x9C, request progress +0x48 (float).
constexpr std::size_t kPagePending = 0x9C, kPageProgress = 0x48, kPagePendingSkills = 0x15A;
constexpr std::uint32_t kSaveOpcode = 0x591, kResetOpcode = 0x867;
// FUN_008638B0 callers that apply the native "mission" category (5) rule on the
// page; G改 1.0 index 5 is an ordinary type.
constexpr std::uintptr_t kMissionRuleReturns[] = {0x8AA74C, 0x8ACE27, 0x8AD8BE};
// FUN_006F51B0 (range + category text) called by the page's TypeStatic refresh.
constexpr std::uintptr_t kTypeTextPageReturn = 0x866382;
// Page busy state: +0xAC initialised, +0x44 state queue, +0x169 dragging.
constexpr std::size_t kPageReady = 0xAC, kPageStates = 0x44, kPageDragging = 0x169;
// Skill list (CServiceFactoryRoomAttributeList): +0x48C shown category index,
// +0x490 open, +0x94C CloseBtn. Category tooltip: +0x58 frame control.
constexpr std::size_t kListCategory = 0x48C, kListOpen = 0x490, kListClose = 0x94C, kTooltipFrame = 0x58;
constexpr std::size_t kEventSlot = 14;
// Category bar: RadioIcon0..7 (+0x1D8), the G改 2.0 type icons of the eight buttons.
constexpr std::size_t kBarTypeIcons = 0x1D8;

using InstanceFn = void*(__cdecl*)();
using ThisFn = void(__thiscall*)(void*);
using AssignFn = void*(__thiscall*)(void*, const wchar_t*, std::size_t);
using PushFn = void(__thiscall*)(void*, const void*);
using NewFn = void*(__cdecl*)(std::size_t);
using DeleteFn = void(__cdecl*)(void*);
using CategoryOfFn = int(__thiscall*)(void*, std::uint32_t);
using SendFn = bool(__thiscall*)(void*);
using ReplyFn = void(__thiscall*)(void*, const void*);
using PendingFn = bool(__thiscall*)(void*, std::uint32_t);
using CountFn = int(__thiscall*)(void*);
using TypeTextFn = void*(__thiscall*)(void*, void*, const std::uint8_t*);
using SlotRenderFn = void(__thiscall*)(void*, int, void*, void*, int);
using SlotRenderAltFn = void(__thiscall*)(void*, int, void*, void*);
using GridFindFn = void**(__thiscall*)(void*, void**, const std::int32_t*);
using EventFn = void(__stdcall*)(void*, std::uint32_t, const void*, void*);

// The host's std::wstring (24 bytes: inline buffer or pointer, size, capacity).
struct HostString {
    union {
        wchar_t small[8];
        wchar_t* pointer;
    } data{};
    std::uint32_t size = 0;
    std::uint32_t capacity = 7;
};
static_assert(sizeof(HostString) == 24, "host std::wstring ABI");

struct EntryState {
    const std::uint8_t* record;
    std::uint32_t weapons;
    std::uint32_t available;
    std::uint32_t met;
};
static_assert(sizeof(EntryState) == 16, "native EntryState");

struct Row {
    std::uint8_t bytes[kRowBytes];
};

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
void (*g_log)(const char*) = nullptr;
const ui::Bindings* g_ui = nullptr;
void** g_service_manager = nullptr;
const void* g_page_vtable = nullptr;
// Native control fields (see original_ui.cpp).
constexpr int kButton = 4, kStatic = 3;
constexpr std::size_t kVisible = 0x162, kVisible2 = 0x163, kHover = 0x164, kEnabled = 0x167, kDebugQuad = 0x168,
                      kPressed = 0x21C, kCapture = 0x18C, kParent = 0x15C, kX = 0x148, kY = 0x14C, kWidth = 0x154,
                      kHeight = 0x158;
constexpr std::size_t kPageResetButton = 0x558;
constexpr std::uint32_t kLabelColor = 0xFFFFFFFF, kShadowColor = 0xFF000000;
// The page's system switch: two buttons under its left panel and a notice line.
struct Switch {
    void* dialog = nullptr;
    void* g2 = nullptr;
    void* g1 = nullptr;
    void* notice = nullptr;
    int width = 0, height = 22;
    std::wstring g2_text, g1_text, notice_text, notice_shown;
    bool g2_set = false, g1_set = false;
};
Switch g_switch;
std::uint8_t** g_grid = nullptr;
std::uint8_t** g_profile = nullptr;
InstanceFn g_instance = nullptr;
ThisFn g_clear = nullptr, g_init = nullptr;
AssignFn g_assign = nullptr;
PushFn g_names_push = nullptr, g_category_push = nullptr;
NewFn g_new = nullptr;
DeleteFn g_delete = nullptr;
PendingFn g_pending_add = nullptr, g_pending_remove = nullptr;
ReplyFn g_save_reply = nullptr, g_reset_reply = nullptr;
ThisFn g_refresh = nullptr;
ThisFn g_bar_relabel = nullptr;
CountFn g_state_busy = nullptr, g_pending_count = nullptr;
const void* g_bar_vtable = nullptr;
const void* g_list_vtable = nullptr;
TypeTextFn g_type_text_original = nullptr;
SlotRenderFn g_slot_render_original = nullptr;
SlotRenderAltFn g_slot_render_alt_original = nullptr;
GridFindFn g_grid_find_original = nullptr;
// While a slot renderer draws a G改 1.0 skill, its 0261 lookup (from 3664001)
// lands on this node, whose value is the Mod's row for that skill.
thread_local const std::uint8_t* t_slot_row = nullptr;
std::uint32_t g_fake_value[2] = {kEntryType, 0};
std::uint8_t g_fake_node[0x18] = {};
bool g_refresh_pending = false;
// Originals of the hooked functions.
ThisFn g_load_categories_original = nullptr, g_load_entries_original = nullptr;
CategoryOfFn g_category_of_original = nullptr;
SendFn g_send_save_original = nullptr, g_send_reset_original = nullptr;
ReplyFn g_reset_cost_original = nullptr;
ThisFn g_category_tooltip_original = nullptr;
bool g_bound = false;

std::vector<Row> g_rows;            // G改 1.0 entries, category index already +1
std::vector<std::wstring> g_names;  // [0] placeholder, then the G改 1.0 types
bool g_active = false;              // the manager and the page hold G改 1.0 data
void* g_page = nullptr;             // the attribute page the mode applies to
std::array<std::uint16_t, 4> g_view{};   // G改 1.0 skills the page should show
std::uint8_t g_view_open = 2;
std::array<std::uint16_t, 4> g_written{}; // what the Mod (or the native reply it fed) last left in the record
std::array<std::uint16_t, 4> g_saved{};  // the record's own skills while swapped
std::uint8_t g_saved_open = 0;
bool g_swapped = false;
std::uint32_t g_view_revision = 0xFFFFFFFF;
bool g_save_request = false, g_save_waiting = false;
std::array<std::uint16_t, 4> g_save_skills{};
bool g_reset_request = false, g_reset_waiting = false;
std::int32_t g_reset_cost = 0;
Stats g_stats;

void Log(const char* format, ...) {
    if (!g_log) return;
    char line[320];
    va_list args;
    va_start(args, format);
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    va_end(args);
    g_log(line);
}

template <class T>
T Read(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

template <class T>
void Write(void* base, std::size_t offset, const T& value) {
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

const std::uint8_t* GridRecord(std::int32_t id, std::uint32_t* type) noexcept {
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
        auto* value = Read<std::uint32_t*>(best, 0x10);
        if (!value) return nullptr;
        *type = value[0];
        return reinterpret_cast<const std::uint8_t*>(value[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::uint8_t* Profile() noexcept {
    __try {
        return g_profile ? *g_profile : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* Manager() noexcept {
    __try {
        return g_instance ? g_instance() : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// --- Loaders --------------------------------------------------------------------

void PushName(void* manager, const std::wstring& name) {
    HostString text;
    g_assign(&text, name.c_str(), name.size());
    g_names_push(static_cast<std::uint8_t*>(manager) + kMgrNames, &text);
    if (text.capacity >= 8 && text.data.pointer) g_delete(text.data.pointer);
}

void __fastcall LoadCategoriesHook(void* manager, void*) {
    if (!g_active) {
        g_load_categories_original(manager);
        return;
    }
    __try {
        for (const auto& name : g_names) {
            Write<std::int32_t>(manager, kMgrCount, Read<std::int32_t>(manager, kMgrCount) + 1);
            PushName(manager, name);
            std::uint8_t category[kCategoryBytes] = {};
            g_category_push(static_cast<std::uint8_t*>(manager) + kMgrCategories, category);
        }
        g_stats.categories = static_cast<std::uint32_t>(Read<std::int32_t>(manager, kMgrCount));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("g1 native: category load faulted");
    }
}

bool PushEntry(std::uint8_t* vector, const std::uint8_t* record) {
    auto* first = Read<EntryState*>(vector, 0);
    auto* last = Read<EntryState*>(vector, 4);
    auto* end = Read<EntryState*>(vector, 8);
    if (last == end) {
        const std::size_t size = static_cast<std::size_t>(last - first);
        const std::size_t capacity = static_cast<std::size_t>(end - first);
        const std::size_t grown = (std::max)(size + 1, capacity + capacity / 2);
        auto* buffer = static_cast<EntryState*>(g_new(grown * sizeof(EntryState)));
        if (!buffer) return false;
        if (size) std::memcpy(buffer, first, size * sizeof(EntryState));
        if (first) g_delete(first);
        first = buffer;
        last = buffer + size;
        end = buffer + grown;
        Write(vector, 0, first);
        Write(vector, 8, end);
    }
    *last = EntryState{record, 0, 0, 0};
    Write(vector, 4, last + 1);
    return true;
}

void __fastcall LoadEntriesHook(void* manager, void*) {
    if (!g_active) {
        g_load_entries_original(manager);
        return;
    }
    __try {
        const auto count = Read<std::int32_t>(manager, kMgrCount);
        auto* categories = Read<std::uint8_t*>(manager, kMgrCategories);
        std::uint32_t entries = 0;
        for (std::int32_t index = 0; index < count && categories; ++index)
            for (const auto& row : g_rows)
                if (row.bytes[kRowCategory] == index &&
                    PushEntry(categories + static_cast<std::size_t>(index) * kCategoryBytes, row.bytes))
                    ++entries;
        g_stats.entries = entries;
        ++g_stats.loads;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("g1 native: entry load faulted");
    }
}

int __fastcall CategoryOfHook(void* manager, void*, std::uint32_t skill) {
    if (g_active) {
        const auto from = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        for (const auto address : kMissionRuleReturns)
            if (from == address) return -1;
    }
    return g_category_of_original(manager, skill);
}

void __fastcall CategoryTooltipHook(void* tooltip, void*) {
    // The category tooltip reads the native 0263 descriptions by index; G改 1.0
    // types have none, so in this mode the frame its caller showed is hidden
    // again, as the native code does when a category has no row.
    if (g_active) {
        __try {
            if (auto* frame = Read<std::uint8_t*>(tooltip, kTooltipFrame)) Write<std::uint8_t>(frame, kVisible, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    g_category_tooltip_original(tooltip);
}

const std::uint8_t* RowOf(std::uint32_t skill) noexcept {
    for (const auto& row : g_rows)
        if (Read<std::uint32_t>(row.bytes, kRowSkill) == skill) return row.bytes;
    return nullptr;
}

// TypeStatic: range name, a space and the category of the record's first skill.
void ComposeType(void* out) {
    auto* text = static_cast<HostString*>(out);
    const wchar_t* chars = text->capacity >= 8 ? text->data.pointer : text->data.small;
    std::wstring native(chars ? chars : L"", chars ? text->size : 0);
    const auto space = native.find(L' ');
    std::wstring range = space == std::wstring::npos ? native : native.substr(0, space);
    std::uint8_t category = 1;
    for (const auto skill : g_view) {
        if (!skill) break;
        if (const auto* row = RowOf(skill)) category = row[kRowCategory];
        break;
    }
    if (category >= g_names.size()) return;
    const std::wstring composed = range.empty() ? g_names[category] : range + L" " + g_names[category];
    g_assign(out, composed.c_str(), composed.size());
}

void* __fastcall TypeTextHook(void* self, void*, void* out, const std::uint8_t* record) {
    void* result = g_type_text_original(self, out, record);
    if (g_active && reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == kTypeTextPageReturn && out) ComposeType(out);
    return result;
}

void __fastcall SlotRenderHook(void* page, void*, int skill, void* icon, void* frame, int slot) {
    t_slot_row = g_active ? RowOf(static_cast<std::uint32_t>(skill)) : nullptr;
    g_slot_render_original(page, skill, icon, frame, slot);
    t_slot_row = nullptr;
}

void __fastcall SlotRenderAltHook(void* page, void*, int skill, void* icon, void* frame) {
    t_slot_row = g_active ? RowOf(static_cast<std::uint32_t>(skill)) : nullptr;
    g_slot_render_alt_original(page, skill, icon, frame);
    t_slot_row = nullptr;
}

void** __fastcall GridFindHook(void* manager, void*, void** out, const std::int32_t* id) {
    if (t_slot_row && id && out && *id == kFirstEntryId) {
        g_fake_value[1] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(t_slot_row));
        Write<std::int32_t>(g_fake_node, 0xC, kFirstEntryId);
        Write<std::uint32_t*>(g_fake_node, 0x10, g_fake_value);
        *out = g_fake_node;
        return out;
    }
    return g_grid_find_original(manager, out, id);
}

// --- Requests ---------------------------------------------------------------------

bool __fastcall SendSaveHook(void* page, void*) {
    if (!g_active) return g_send_save_original(page);
    __try {
        if (g_save_request || g_save_waiting || !g_pending_add(static_cast<std::uint8_t*>(page) + kPagePending, kSaveOpcode))
            return false;
        for (std::size_t slot = 0; slot < 4; ++slot)
            g_save_skills[slot] = Read<std::uint16_t>(page, kPagePendingSkills + slot * 2);
        g_page = page;
        g_save_request = true;
        Log("g1 native: save requested %u/%u/%u/%u", g_save_skills[0], g_save_skills[1], g_save_skills[2],
            g_save_skills[3]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __fastcall SendResetHook(void* page, void*) {
    if (!g_active) return g_send_reset_original(page);
    __try {
        if (g_reset_request || g_reset_waiting ||
            !g_pending_add(static_cast<std::uint8_t*>(page) + kPagePending, kResetOpcode))
            return false;
        g_page = page;
        g_reset_request = true;
        Log("g1 native: reset requested");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall ResetCostHook(void* popup, void*, std::uint8_t* packet) {
    // The popup asks the server for the native reset cost (0x566 -> 0x959) and
    // shows u16 +0x12; in G改 1.0 mode it shows the G改 1.0 cost instead.
    if (g_active && packet) {
        __try {
            Write<std::uint16_t>(packet, 0x12,
                                 static_cast<std::uint16_t>((std::min)((std::max)(g_reset_cost, 0), 0xFFFF)));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_reset_cost_original(popup, packet);
}

// --- System switch ---------------------------------------------------------------

void* FindRaw(void* dialog, const ui::NativeString* name, int type) noexcept {
    __try {
        return g_ui->find(dialog, name, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool AddRaw(void* dialog, const ui::NativeString* name, int x, int y, int width, int height) noexcept {
    __try {
        g_ui->add(dialog, name, x, y, width, height, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SkinRaw(void* control, const ui::NativeString* sprite) noexcept {
    __try {
        g_ui->skin(control, sprite, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A created button starts hidden; false when the control is not the dialog's own button.
bool PrepareRaw(void* control, void* dialog) noexcept {
    __try {
        if (Read<void*>(control, kParent) != dialog || Read<void*>(control, 0) != g_ui->button_vtable) return false;
        Write<std::uint8_t>(control, kVisible, 0);
        Write<std::uint8_t>(control, kVisible2, 0);
        Write<std::uint8_t>(control, kEnabled, 0);
        Write<std::uint8_t>(control, kDebugQuad, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RefreshRaw(void* control, void* dialog) noexcept {
    __try {
        if (g_ui->refresh) {
            g_ui->refresh(control);
            g_ui->refresh(dialog);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* Control(void* dialog, const wchar_t* name, int type) {
    const std::wstring text = name;
    const auto native = ui::ViewOf(text);
    return FindRaw(dialog, &native, type);
}

void* CreateSwitchButton(void* dialog, const wchar_t* name, int x, int y, int width, int height, bool skin) {
    const std::wstring text = name;
    const auto native = ui::ViewOf(text);
    void* control = FindRaw(dialog, &native, kButton);
    if (!control && AddRaw(dialog, &native, x, y, width, height)) control = FindRaw(dialog, &native, kButton);
    if (!control || !PrepareRaw(control, dialog)) return nullptr;
    if (skin) {
        // The page's own button sprites (设置保存 / 还原 / 强化槽初始化).
        for (int state = 0; state < 3; ++state) {
            const std::wstring sprite = L"sprs\\" + std::to_wstring(16819 + state) + L".spr";
            const auto native_sprite = ui::ViewOf(sprite);
            SkinRaw(control, &native_sprite);
        }
    }
    RefreshRaw(control, dialog);
    return control;
}

void SetState(void* control, bool visible, bool enabled) noexcept {
    if (!control) return;
    __try {
        if (!visible || !enabled) {
            Write<std::uint16_t>(control, kPressed, 0);
            Write<std::uint8_t>(control, kHover, 0);
            Write<std::uint32_t>(control, kCapture, 0);
        }
        Write<std::uint8_t>(control, kVisible, visible ? 1 : 0);
        Write<std::uint8_t>(control, kVisible2, visible ? 1 : 0);
        Write<std::uint8_t>(control, kEnabled, enabled ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TextRaw(void* control, const ui::NativeString* text, int width, int height, std::uint32_t color) noexcept {
    __try {
        g_ui->text(control, 0, text, 2, (std::max)(0, (height - 14) / 2), (std::max)(1, width - 4), 14, color,
                   kShadowColor, 0, 1, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SetText(void* control, const std::wstring& value, int width, int height, std::uint32_t color) {
    if (!control) return;
    const auto native = ui::ViewOf(value);
    TextRaw(control, &native, width, height, color);
}

// The page's 强化槽初始化 button (+0x558) and its dialog (the uirs\1252 layout).
void* PageResetButton(void* page) noexcept {
    __try {
        return page ? Read<void*>(page, kPageResetButton) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* ParentOf(void* control) noexcept {
    __try {
        return control ? Read<void*>(control, kParent) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool GeometryRaw(void* control, int* x, int* y, int* width, int* height) noexcept {
    __try {
        *x = static_cast<int>(Read<float>(control, kX));
        *y = static_cast<int>(Read<float>(control, kY));
        *width = Read<int>(control, kWidth);
        *height = Read<int>(control, kHeight);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Under the left panel, aligned with its 强化槽初始化 button.
bool AttachSwitch(void* dialog, void* reset) {
    if (g_switch.dialog == dialog && g_switch.g2 && g_switch.g1) return true;
    g_switch = Switch{};
    if (!reset || !dialog) return false;
    int x = 0, y = 0, width = 0, height = 0;
    if (!GeometryRaw(reset, &x, &y, &width, &height)) return false;
    if (width < 100 || width > 400) width = 284;
    if (height < 16 || height > 40) height = 22;
    const int half = (width - 4) / 2;
    const int top = y + height + 18;
    g_switch.dialog = dialog;
    g_switch.width = half;
    g_switch.height = height;
    g_switch.g2 = CreateSwitchButton(dialog, L"OcAttrG2Btn", x, top, half, height, true);
    g_switch.g1 = CreateSwitchButton(dialog, L"OcAttrG1Btn", x + width - half, top, half, height, true);
    g_switch.notice = CreateSwitchButton(dialog, L"OcAttrNotice", x, top + height + 4, width, 18, false);
    if (!g_switch.g2 || !g_switch.g1) {
        Log("g1 native: the page switch could not be created");
        return false;
    }
    Log("g1 native: page switch created under the left panel at x=%d y=%d", x, top);
    return true;
}

void DrawSwitch(void* page, bool g1_available) {
    void* reset = PageResetButton(page);
    void* dialog = ParentOf(reset);
    if (!page || !dialog || !AttachSwitch(dialog, reset)) {
        if (!page && g_switch.dialog) {
            SetState(g_switch.g2, false, false);
            SetState(g_switch.g1, false, false);
            SetState(g_switch.notice, false, false);
        }
        return;
    }
    // The system the page shows is marked; the other one is the way there.
    const std::wstring g2_text = g_active ? L"G改 2.0" : L"● G改 2.0";
    const std::wstring g1_text = g_active ? L"● G改 1.0" : L"G改 1.0";
    if (g2_text != g_switch.g2_text) {
        SetText(g_switch.g2, g2_text, g_switch.width, g_switch.height, kLabelColor);
        g_switch.g2_text = g2_text;
    }
    if (g1_text != g_switch.g1_text) {
        SetText(g_switch.g1, g1_text, g_switch.width, g_switch.height, g1_available ? kLabelColor : 0xFF808080);
        g_switch.g1_text = g1_text;
    }
    SetState(g_switch.g2, true, g_active);
    SetState(g_switch.g1, true, !g_active && g1_available);
    if (g_switch.notice) {
        if (g_switch.notice_text != g_switch.notice_shown) {
            SetText(g_switch.notice, g_switch.notice_text, g_switch.width * 2 + 4, 18, 0xFFFFD040);
            g_switch.notice_shown = g_switch.notice_text;
        }
        SetState(g_switch.notice, !g_switch.notice_text.empty(), false);
    }
}

bool CopyRow(Row* row, const std::uint8_t* source) noexcept {
    __try {
        std::memcpy(row->bytes, source, kRowBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- Mode ------------------------------------------------------------------------

std::uint8_t OpenSlots(const G1Loadout& loadout) {
    return static_cast<std::uint8_t>(loadout.normal_max >= 4 ? 4 : loadout.normal_max >= 3 ? 3 : 2);
}

// Keeps the profile record showing the G改 1.0 loadout while the mode lasts. A
// native refresh of the record (a new unit record from the server) lands
// between frames: whatever is not the G改 1.0 view is the record's own data.
void ApplyView() noexcept {
    auto* profile = Profile();
    if (!profile) return;
    __try {
        std::array<std::uint16_t, 4> current{};
        std::memcpy(current.data(), profile + kProfileSkills, sizeof(current));
        const auto open = Read<std::uint8_t>(profile, kProfileOpenSlots);
        if (!g_swapped || current != g_written) {
            // Not what the Mod left there: the record's own data.
            g_saved = current;
            g_saved_open = open;
        }
        if (!g_swapped || current != g_view || open != g_view_open) {
            std::memcpy(profile + kProfileSkills, g_view.data(), sizeof(g_view));
            Write<std::uint8_t>(profile, kProfileOpenSlots, g_view_open);
        }
        g_written = g_view;
        g_swapped = true;
        g_stats.shown = g_view;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RestoreView() noexcept {
    if (!g_swapped) return;
    auto* profile = Profile();
    __try {
        if (profile) {
            std::array<std::uint16_t, 4> current{};
            std::memcpy(current.data(), profile + kProfileSkills, sizeof(current));
            // Only the Mod's own view is replaced; a record the server sent
            // meanwhile is the record's data already.
            if (current == g_written) {
                std::memcpy(profile + kProfileSkills, g_saved.data(), sizeof(g_saved));
                Write<std::uint8_t>(profile, kProfileOpenSlots, g_saved_open);
            }
            std::memcpy(g_stats.shown.data(), profile + kProfileSkills, sizeof(g_stats.shown));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_swapped = false;
}

void Reload() noexcept {
    void* manager = Manager();
    if (!manager || !g_clear || !g_init) return;
    __try {
        g_clear(manager);
        g_init(manager);
        g_stats.categories = static_cast<std::uint32_t>(Read<std::int32_t>(manager, kMgrCount));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("g1 native: manager reload faulted");
    }
}

void* FindService(const void* vtable) noexcept;

// The page takes input and accepts a refresh only when it is initialised, no
// state runs, no reply is pending and nothing is being dragged.
bool PageIdle(void* page) noexcept {
    if (!page) return false;
    __try {
        auto* base = static_cast<std::uint8_t*>(page);
        return Read<std::uint8_t>(page, kPageReady) && !g_state_busy(base + kPageStates) &&
               !g_pending_count(base + kPagePending) && !Read<std::uint8_t>(page, kPageDragging);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CloseListRaw(void* list, const HostString* name) noexcept {
    __try {
        void* close = Read<void*>(list, kListClose);
        auto** vtable = Read<void**>(list, 0);
        if (close && vtable) reinterpret_cast<EventFn>(vtable[kEventSlot])(list, 0x101, name, close);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The category bar and the skill list follow the reloaded manager: the bar
// relabels its buttons, an open list closes (its category index stays valid
// for both rule sets: the native list rebuilds it on save and reset messages
// even when closed).
void RefreshBarAndList() {
    void* list = FindService(g_list_vtable);
    if (list) {
        bool open = false;
        __try {
            Write<std::int32_t>(list, kListCategory, 1);
            open = Read<std::uint8_t>(list, kListOpen) == 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (open) {
            HostString name;
            g_assign(&name, L"CloseBtn", 8);
            CloseListRaw(list, &name);
            if (name.capacity >= 8 && name.data.pointer) g_delete(name.data.pointer);
        }
    }
    if (void* bar = FindService(g_bar_vtable)) {
        __try {
            g_bar_relabel(bar);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("g1 native: category bar relabel faulted");
        }
    }
}

// The bar's type icons belong to the G改 2.0 types; in G改 1.0 mode the buttons
// show the type names alone (the native relabel shows the icons again).
void HideTypeIcons() noexcept {
    void* bar = FindService(g_bar_vtable);
    if (!bar) return;
    __try {
        for (std::size_t index = 0; index < 8; ++index) {
            auto* icon = Read<std::uint8_t*>(bar, kBarTypeIcons + index * 4);
            if (icon && Read<std::uint8_t>(icon, kVisible)) {
                Write<std::uint8_t>(icon, kVisible, 0);
                Write<std::uint8_t>(icon, kVisible2, 0);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void RefreshPage(void* page) noexcept {
    g_refresh_pending = true;
    if (!page || !g_refresh || !PageIdle(page)) return;
    __try {
        g_refresh(page);
        g_refresh_pending = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_refresh_pending = false;
        Log("g1 native: page refresh faulted");
    }
}

void Enter(void* page, const G1Loadout& loadout) {
    g_view = loadout.skills;
    g_view_open = OpenSlots(loadout);
    g_view_revision = loadout.revision;
    g_active = true;
    g_page = page;
    ApplyView();
    Reload();
    RefreshBarAndList();
    RefreshPage(page);
    ++g_stats.entered;
    Log("g1 native: G改 1.0 on the 强化部件 page (%u categories, %u entries, skills %u/%u/%u/%u)", g_stats.categories,
        g_stats.entries, g_view[0], g_view[1], g_view[2], g_view[3]);
}

void Exit(void* page) {
    g_active = false;
    RestoreView();
    Reload();
    RefreshBarAndList();
    if (page) RefreshPage(page);
    else g_refresh_pending = false;
    g_page = nullptr;
    g_save_request = g_reset_request = false;
    ++g_stats.left;
    Log("g1 native: native 强化部件 data restored (%u categories)", g_stats.categories);
}

} // namespace

bool Bind(const SdgoApi* api, SdgoMod* mod, void (*log)(const char*), const ui::Bindings* bindings) {
    g_api = api;
    g_mod = mod;
    g_log = log;
    g_ui = bindings;
    g_service_manager = static_cast<void**>(api->Symbol(mod, "flow.service_managed_pointer"));
    g_page_vtable = api->Symbol(mod, "oc_custom.attr_page_vtable");
    if (!g_ui || !g_ui->add || !g_ui->find || !g_ui->text || !g_ui->skin || !g_ui->button_vtable || !g_service_manager ||
        !g_page_vtable)
        return false;
    auto symbol = [&](const char* name) { return api->Symbol(mod, name); };
    g_grid = reinterpret_cast<std::uint8_t**>(symbol("oc_custom.grid_singleton"));
    g_profile = reinterpret_cast<std::uint8_t**>(symbol("hangar.profile_global"));
    g_instance = reinterpret_cast<InstanceFn>(symbol("oc_custom.attr_mgr_instance"));
    g_clear = reinterpret_cast<ThisFn>(symbol("oc_custom.attr_mgr_clear"));
    g_init = reinterpret_cast<ThisFn>(symbol("oc_custom.attr_mgr_init"));
    g_assign = reinterpret_cast<AssignFn>(symbol("oc_custom.native_wstring_assign"));
    g_names_push = reinterpret_cast<PushFn>(symbol("oc_custom.attr_names_push"));
    g_category_push = reinterpret_cast<PushFn>(symbol("oc_custom.attr_category_push"));
    g_new = reinterpret_cast<NewFn>(symbol("crt.operator_new"));
    g_delete = reinterpret_cast<DeleteFn>(symbol("crt.operator_delete"));
    g_pending_add = reinterpret_cast<PendingFn>(symbol("oc_custom.attr_pending_add"));
    g_pending_remove = reinterpret_cast<PendingFn>(symbol("oc_custom.attr_pending_remove"));
    g_save_reply = reinterpret_cast<ReplyFn>(symbol("oc_custom.attr_save_reply"));
    g_reset_reply = reinterpret_cast<ReplyFn>(symbol("oc_custom.attr_reset_reply"));
    g_refresh = reinterpret_cast<ThisFn>(symbol("oc_custom.attr_page_refresh"));
    g_bar_relabel = reinterpret_cast<ThisFn>(symbol("oc_custom.attr_bar_relabel"));
    g_state_busy = reinterpret_cast<CountFn>(symbol("oc_custom.attr_state_busy"));
    g_pending_count = reinterpret_cast<CountFn>(symbol("oc_custom.attr_pending_count"));
    g_bar_vtable = symbol("oc_custom.attr_bar_vtable");
    g_list_vtable = symbol("oc_custom.attr_list_vtable");
    if (!g_bar_relabel || !g_state_busy || !g_pending_count || !g_bar_vtable || !g_list_vtable) return false;
    if (!g_grid || !g_profile || !g_instance || !g_clear || !g_init || !g_assign || !g_names_push || !g_category_push ||
        !g_new || !g_delete || !g_pending_add || !g_pending_remove || !g_save_reply || !g_reset_reply || !g_refresh)
        return false;
    g_bound =
        api->HookCode(mod, "oc_custom.attr_load_categories", reinterpret_cast<void*>(&LoadCategoriesHook),
                      reinterpret_cast<void**>(&g_load_categories_original)) &&
        api->HookCode(mod, "oc_custom.attr_load_entries", reinterpret_cast<void*>(&LoadEntriesHook),
                      reinterpret_cast<void**>(&g_load_entries_original)) &&
        api->HookCode(mod, "oc_custom.attr_category_of_skill", reinterpret_cast<void*>(&CategoryOfHook),
                      reinterpret_cast<void**>(&g_category_of_original)) &&
        api->HookCode(mod, "oc_custom.attr_category_tooltip", reinterpret_cast<void*>(&CategoryTooltipHook),
                      reinterpret_cast<void**>(&g_category_tooltip_original)) &&
        api->HookCode(mod, "oc_custom.attr_send_save", reinterpret_cast<void*>(&SendSaveHook),
                      reinterpret_cast<void**>(&g_send_save_original)) &&
        api->HookCode(mod, "oc_custom.attr_send_reset", reinterpret_cast<void*>(&SendResetHook),
                      reinterpret_cast<void**>(&g_send_reset_original)) &&
        api->HookCode(mod, "oc_custom.attr_reset_cost_reply", reinterpret_cast<void*>(&ResetCostHook),
                      reinterpret_cast<void**>(&g_reset_cost_original)) &&
        api->HookCode(mod, "oc_custom.attr_type_text", reinterpret_cast<void*>(&TypeTextHook),
                      reinterpret_cast<void**>(&g_type_text_original)) &&
        api->HookCode(mod, "oc_custom.attr_slot_render", reinterpret_cast<void*>(&SlotRenderHook),
                      reinterpret_cast<void**>(&g_slot_render_original)) &&
        api->HookCode(mod, "oc_custom.attr_slot_render_alt", reinterpret_cast<void*>(&SlotRenderAltHook),
                      reinterpret_cast<void**>(&g_slot_render_alt_original)) &&
        api->HookCode(mod, "oc_custom.attr_grid_find", reinterpret_cast<void*>(&GridFindHook),
                      reinterpret_cast<void**>(&g_grid_find_original));
    return g_bound;
}

bool Bound() { return g_bound; }

namespace {
void* FindService(const void* vtable) noexcept {
    __try {
        if (!vtable || !g_service_manager || !*g_service_manager) return nullptr;
        auto* owner = static_cast<std::uint8_t*>(*g_service_manager);
        auto* head = *reinterpret_cast<std::uint8_t**>(owner + 0x44);
        if (!head) return nullptr;
        auto* node = *reinterpret_cast<std::uint8_t**>(head);
        for (unsigned count = 0; node && node != head && count < 512; ++count) {
            void* service = *reinterpret_cast<void**>(node + 8);
            if (service && *static_cast<const void* const*>(service) == vtable) return service;
            node = *reinterpret_cast<std::uint8_t**>(node);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}
} // namespace

void* FindPage() { return FindService(g_page_vtable); }

bool PageReady() { return g_state_busy && PageIdle(FindPage()); }

void* CategoryBarService() { return FindService(g_bar_vtable); }

std::uint32_t CategoryNewMask() {
    void* manager = Manager();
    if (!manager) return 0;
    std::uint32_t mask = 0;
    __try {
        const auto count = Read<std::int32_t>(manager, kMgrCount);
        auto* categories = Read<std::uint8_t*>(manager, kMgrCategories);
        for (std::int32_t index = 0; index < 8 && categories; ++index)
            if (index + 1 < count && Read<std::uint8_t>(categories + static_cast<std::size_t>(index + 1) * kCategoryBytes, 0x10))
                mask |= 1u << index;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return mask;
}

void* NativeService(int which) {
    static const char* const kNames[] = {nullptr, "oc_custom.attr_bar_vtable", "oc_custom.attr_list_vtable",
                                         "oc_custom.attr_save_popup_vtable", "oc_custom.attr_reset_popup_vtable",
                                         "oc_custom.attr_reset_check_popup_vtable"};
    if (which < 1 || which > 5 || !g_api) return nullptr;
    return FindService(g_api->Symbol(g_mod, kNames[which]));
}

int SwitchOf(void* control) {
    if (!control) return 0;
    if (control == g_switch.g2) return 1;
    if (control == g_switch.g1) return 2;
    return 0;
}

void* SwitchControl(int which) { return which == 1 ? g_switch.g2 : which == 2 ? g_switch.g1 : nullptr; }

void SetNotice(const std::wstring& text) { g_switch.notice_text = text; }

void SetRules(const std::vector<G1Rule>& rules, const std::vector<std::wstring>& categories) {
    // Native rows by skill: the G改 1.0 skills are the client's own skills, so
    // each entry starts from the native row of the same skill (icon frame,
    // option colours, weapon flag) and takes the G改 1.0 rule's values.
    std::unordered_map<std::uint32_t, const std::uint8_t*> native;
    for (std::int32_t id = kFirstEntryId; id < kFirstEntryId + 4000; ++id) {
        std::uint32_t type = 0;
        const auto* record = GridRecord(id, &type);
        if (!record || type != kEntryType) break;
        native.emplace(Read<std::uint32_t>(record, kRowSkill), record);
    }
    std::vector<Row> rows;
    std::uint32_t missing = 0;
    for (const auto& rule : rules) {
        const auto found = native.find(rule.skill);
        if (found == native.end()) {
            ++missing;
            continue;
        }
        Row row{};
        if (!CopyRow(&row, found->second)) {
            ++missing;
            continue;
        }
        row.bytes[kRowCategory] = static_cast<std::uint8_t>(rule.category + 1);
        row.bytes[kRowType] = static_cast<std::uint8_t>((std::min<int>)((std::max<int>)(rule.type, 1), 4));
        row.bytes[kRowUnitLevel] = static_cast<std::uint8_t>((std::min<int>)((std::max<int>)(rule.unit_level, 2), 13));
        row.bytes[kRowUserLevel] = rule.user_level;
        row.bytes[kRowPremium] = rule.premium ? 1 : 0;
        row.bytes[kRowNew] = 0;
        rows.push_back(row);
    }
    g_rows = std::move(rows);
    g_names.clear();
    g_names.push_back(L"G改 1.0");
    for (const auto& name : categories) g_names.push_back(name);
    Log("g1 native: rule set ready (%u entries, %u types, %u without a native row)", static_cast<unsigned>(g_rows.size()),
        static_cast<unsigned>(categories.size()), missing);
}

bool HasRules() { return !g_rows.empty() && g_names.size() > 1; }

void Update(void* page, bool g1_available, bool want_g1, const G1Loadout* loadout) {
    if (!g_bound) return;
    // The G改 1.0 button works as soon as the server offers G改 1.0; the rule
    // set is read after the click.
    DrawSwitch(page, g1_available);
    const bool waiting = g_save_request || g_save_waiting || g_reset_request || g_reset_waiting;
    const bool should = page && want_g1 && loadout && HasRules();
    if (g_active && (!should || page != g_page) && !waiting) {
        // Back to the native data: at once when the tab is gone, else when idle.
        if (!page || page != g_page) Exit(nullptr);
        else if (PageIdle(page)) Exit(page);
    } else if (!g_active && should && PageIdle(page)) {
        Enter(page, *loadout);
    }
    if (g_refresh_pending && page && !waiting) RefreshPage(page);
    if (g_active && page) HideTypeIcons();
    if (!g_active) return;
    if (loadout && loadout->revision != g_view_revision && !waiting) {
        // A new loadout from the server (another save or a reset elsewhere).
        g_view = loadout->skills;
        g_view_open = OpenSlots(*loadout);
        g_view_revision = loadout->revision;
        ApplyView();
        RefreshPage(g_page);
        return;
    }
    ApplyView();
}

bool Active() { return g_active; }

void Leave() {
    if (g_active) Exit(nullptr);
}

bool TakeSave(std::array<std::uint16_t, 4>* skills) {
    if (!g_save_request) return false;
    g_save_request = false;
    g_save_waiting = true;
    *skills = g_save_skills;
    return true;
}

bool TakeReset() {
    if (!g_reset_request) return false;
    g_reset_request = false;
    g_reset_waiting = true;
    return true;
}

void SaveResult(bool ok, const std::array<std::uint16_t, 4>& skills) {
    if (!g_save_waiting) return;
    g_save_waiting = false;
    void* page = g_page;
    if (!g_active || !page) return;
    __try {
        if (ok) {
            // The native success reply (0x984): skills at +0x15/+0x17/+0x19/+0x1B.
            std::uint8_t packet[0x40] = {};
            Write<std::uint16_t>(packet, 0, 0x984);
            for (std::size_t slot = 0; slot < 4; ++slot) Write<std::uint16_t>(packet, 0x15 + slot * 2, skills[slot]);
            g_view = skills;
            g_save_reply(page, packet);  // writes the skills into the record
            g_written = skills;
            ApplyView();
            ++g_stats.saves;
        } else {
            g_pending_remove(static_cast<std::uint8_t*>(page) + kPagePending, kSaveOpcode);
            Write<float>(page, kPageProgress, 1.0f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("g1 native: save result faulted");
    }
}

void ResetResult(bool ok, std::int32_t charged) {
    if (!g_reset_waiting) return;
    g_reset_waiting = false;
    void* page = g_page;
    if (!g_active || !page) return;
    __try {
        if (ok) {
            // The native success reply (0x683): money to deduct at +0x14.
            std::uint8_t packet[0x40] = {};
            Write<std::uint16_t>(packet, 0, 0x683);
            Write<std::uint16_t>(packet, 0x14, static_cast<std::uint16_t>((std::min)((std::max)(charged, 0), 0xFFFF)));
            g_view = {};
            g_reset_reply(page, packet);  // clears the record's skills
            g_written = {};
            ApplyView();
            ++g_stats.resets;
        } else {
            g_pending_remove(static_cast<std::uint8_t*>(page) + kPagePending, kResetOpcode);
            Write<float>(page, kPageProgress, 1.0f);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("g1 native: reset result faulted");
    }
}

void SetResetCost(std::int32_t cost) { g_reset_cost = cost; }

Stats GetStats() { return g_stats; }

} // namespace sdgo::oc_custom::client::g1_native
