#include "factory_l10n.h"

#include <windows.h>

#include <array>
#include <cstdarg>
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <string>

namespace sdgo::oc_custom::client::l10n {
namespace {

constexpr int kIcon = 1, kButton = 4;
constexpr std::size_t kVisible = 0x162, kVisible2 = 0x163, kEnabled = 0x167, kDebugQuad = 0x168, kParent = 0x15C,
                      kZ = 0x150;
constexpr std::uint32_t kTextType = 0x3B; // client text table (gdrs\0076)
// z order: smaller draws later (on top). Dialog art 60, controls 30, overlays 29.
constexpr float kPatchZ = 28.0f, kLabelZ = 27.0f, kHeaderArtZ = 45.0f;
// Category bar: NewIcon0..7 (+0x198).
constexpr std::size_t kBarNewIcons = 0x198;
// A control's skins (hangar.ui_add_skin, FUN_00C770B0): vector<Skin*> at
// +0x1A4/+0x1A8, the shown skin index at +0x18C; vtable +0x1C applies it.
constexpr std::size_t kSkinsBegin = 0x1A4, kSkinsEnd = 0x1A8, kSkinIndex = 0x18C, kApplySkinSlot = 0x1C / 4;
std::array<void*, 8> g_swapped{};

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
const ui::Bindings* g_ui = nullptr;
void (*g_log)(const char*) = nullptr;
std::uint8_t** g_grid = nullptr;
bool g_texts_done = false;
void* g_header_dialog = nullptr;
void* g_info_dialog = nullptr;
std::array<void*, 4> g_info_controls{};
std::array<void*, 2> g_info_labels{};
void* g_bar_dialog = nullptr;
std::array<void*, 8> g_badges{};
Stats g_stats;

void Log(const char* format, ...) {
    if (!g_log) return;
    char line[256];
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

std::uint8_t* GridRecord(std::int32_t id, std::uint32_t type) noexcept {
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
        if (!value || value[0] != type) return nullptr;
        return reinterpret_cast<std::uint8_t*>(value[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Replaces a text table entry in place when it still holds the expected text.
bool Translate(std::int32_t id, const wchar_t* expected, const wchar_t* chinese) noexcept {
    auto* record = GridRecord(id, kTextType);
    if (!record) return false;
    __try {
        auto* text = reinterpret_cast<wchar_t*>(record + 5);
        const std::size_t length = std::wcslen(expected);
        if (std::wcsncmp(text, expected, length) || text[length]) return false;
        const std::size_t chinese_length = std::wcslen(chinese);
        if (chinese_length > length) return false;
        std::wmemcpy(text, chinese, chinese_length + 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindRaw(void* dialog, const ui::NativeString* name, int type) noexcept {
    __try {
        return g_ui->find(dialog, name, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool PrepareRaw(void* control, void* dialog, float z) noexcept {
    __try {
        if (Read<void*>(control, kParent) != dialog) return false;
        Write<std::uint8_t>(control, kVisible, 0);
        Write<std::uint8_t>(control, kVisible2, 0);
        Write<std::uint8_t>(control, kEnabled, 0);
        Write<std::uint8_t>(control, kDebugQuad, 0);
        Write<float>(control, kZ, z);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void AddIconRaw(void* dialog, const ui::NativeString* name, int x, int y, int width, int height) noexcept {
    __try {
        g_ui->add_icon(dialog, name, x, y, width, height);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AddButtonRaw(void* dialog, const ui::NativeString* name, int x, int y, int width, int height) noexcept {
    __try {
        g_ui->add(dialog, name, x, y, width, height, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void SkinRaw(void* control, const ui::NativeString* sprite) noexcept {
    __try {
        g_ui->skin(control, sprite, 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TextRaw(void* control, const ui::NativeString* text, int width, int height, std::uint32_t color, int align) noexcept {
    __try {
        g_ui->text(control, 0, text, 0, (std::max)(0, (height - 14) / 2), width, 14, color, 0xFF000000, 0, 1, align);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Show(void* control, bool visible) noexcept {
    if (!control) return;
    __try {
        Write<std::uint8_t>(control, kVisible, visible ? 1 : 0);
        Write<std::uint8_t>(control, kVisible2, visible ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* Icon(void* dialog, const wchar_t* name, int x, int y, int width, int height, int sprite, float z) {
    const std::wstring text = name;
    const auto native = ui::ViewOf(text);
    void* control = FindRaw(dialog, &native, kIcon);
    if (!control) {
        AddIconRaw(dialog, &native, x, y, width, height);
        control = FindRaw(dialog, &native, kIcon);
        if (!control || !PrepareRaw(control, dialog, z)) return nullptr;
        const std::wstring path = L"sprs\\" + std::to_wstring(sprite) + L".spr";
        const auto native_path = ui::ViewOf(path);
        SkinRaw(control, &native_path);
    }
    return control;
}

// A caption holder: a button without sprites draws only its text item.
void* Label(void* dialog, const wchar_t* name, int x, int y, int width, int height, const wchar_t* value,
            std::uint32_t color, int align) {
    const std::wstring text = name;
    const auto native = ui::ViewOf(text);
    void* control = FindRaw(dialog, &native, kButton);
    if (!control) {
        AddButtonRaw(dialog, &native, x, y, width, height);
        control = FindRaw(dialog, &native, kButton);
        if (!control || !PrepareRaw(control, dialog, kLabelZ)) return nullptr;
        const std::wstring caption = value;
        const auto native_caption = ui::ViewOf(caption);
        TextRaw(control, &native_caption, width, height, color, align);
    }
    return control;
}

void* ReadPointer(void* base, std::size_t offset) noexcept {
    __try {
        return base ? Read<void*>(base, offset) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool Visible(void* control) noexcept {
    __try {
        return control && Read<std::uint8_t>(control, kVisible);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::size_t SkinCount(void* control) noexcept {
    __try {
        const auto begin = Read<std::uintptr_t>(control, kSkinsBegin);
        const auto end = Read<std::uintptr_t>(control, kSkinsEnd);
        return begin && end >= begin ? (end - begin) / sizeof(void*) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The appended skins move to the front (the vector keeps owning all of them)
// and the first one is applied, as the native skin call does.
bool FrontSkinsRaw(void* control, std::size_t old_count, std::size_t added) noexcept {
    __try {
        auto** skins = Read<void**>(control, kSkinsBegin);
        for (std::size_t index = 0; index < added; ++index) {
            void* moved = skins[old_count + index];
            for (std::size_t at = old_count + index; at > index; --at) skins[at] = skins[at - 1];
            skins[index] = moved;
        }
        Write<std::int32_t>(control, kSkinIndex, 0);
        auto** vtable = Read<void**>(control, 0);
        reinterpret_cast<void(__thiscall*)(void*)>(vtable[kApplySkinSlot])(control);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Swapped(void* control) {
    for (auto* done : g_swapped)
        if (done == control) return true;
    return false;
}

// Replaces a control's art with the client's own text-free sprites, once.
bool ReplaceSkins(void* control, std::initializer_list<int> sprites) {
    if (!control || Swapped(control)) return control != nullptr;
    const std::size_t before = SkinCount(control);
    if (!before || before > 8) return false;
    for (const int sprite : sprites) {
        const std::wstring path = L"sprs\\" + std::to_wstring(sprite) + L".spr";
        const auto native = ui::ViewOf(path);
        SkinRaw(control, &native);
    }
    if (SkinCount(control) != before + sprites.size() || !FrontSkinsRaw(control, before, sprites.size())) return false;
    for (auto& done : g_swapped)
        if (!done) {
            done = control;
            break;
        }
    return true;
}

void* ParentOf(void* control) noexcept {
    __try {
        return control ? Read<void*>(control, kParent) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace

bool Bind(const SdgoApi* api, SdgoMod* mod, const ui::Bindings* bindings, void (*log)(const char*), std::uint8_t** grid) {
    g_api = api;
    g_mod = mod;
    g_ui = bindings;
    g_log = log;
    g_grid = grid;
    return g_ui && g_ui->add && g_ui->add_icon && g_ui->find && g_ui->skin && g_ui->text && g_grid;
}

void PatchTexts() {
    if (g_texts_done || !g_grid || !*g_grid) return;
    // The table is loaded once the grade text exists.
    if (!GridRecord(126637, kTextType)) return;
    g_texts_done = true;
    if (Translate(126637, L"%s RANK", L"%s级")) ++g_stats.texts;
    if (Translate(127604, L"%dLv", L"%d级")) ++g_stats.texts;
    Log("factory l10n: %u text table entries translated", g_stats.texts);
}

void Header(void* title_control) {
    if (!g_ui) return;
    void* dialog = ParentOf(title_control);
    if (!dialog || dialog == g_header_dialog) return;
    // 0804: its art (sprs\8538, 397x51) spells FACTORY at (60,12)-(178,30); the
    // dialog takes the client's own twin without the word (sprs\13875).
    if (!ReplaceSkins(dialog, {13875})) return;
    void* text = Label(dialog, L"OcL10nHeaderText", 60, 10, 120, 22, L"整备室", 0xFFE8F6FF, 0);
    if (!text) return;
    Show(text, true);
    g_header_dialog = dialog;
    ++g_stats.header;
    Log("factory l10n: header shows 整备室");
}

void InfoPage(void* dialog, bool shown) {
    if (!g_ui || !dialog) return;
    if (dialog != g_info_dialog) {
        g_info_controls = {};
        // 0800 art (dialog coordinates): the panel (sprs\8487) spells
        // "<- UNIT INFORMATION" at (120,29)-(242,41) on flat navy, the level
        // track frame (BackIcon0, sprs\8472) "QUER CUSTOM" at (66,181)-(143,189)
        // and the ITEM box frame (BackIcon1, sprs\8485) "ITEM" at
        // (104,421)-(145,433). Flat sprites of the client's own panels cover the
        // words (sprs\17731 and 17733, 118x23; sprs\13238, 48x24) and Chinese
        // labels take their place; added later, both draw above the art.
        g_info_controls[0] = Icon(dialog, L"OcL10nInfoPatch", 124, 24, 118, 23, 17731, kPatchZ);
        g_info_controls[1] = Icon(dialog, L"OcL10nTrackPatch", 46, 174, 118, 23, 17733, kPatchZ);
        g_info_controls[2] = Icon(dialog, L"OcL10nItemPatch", 101, 415, 48, 24, 13238, kPatchZ);
        g_info_controls[3] = Label(dialog, L"OcL10nInfoText", 124, 26, 118, 18, L"← 机体信息", 0xFF6CCBF0, 2);
        g_info_labels[0] = Label(dialog, L"OcL10nTrackText", 46, 176, 118, 18, L"终极强化", 0xFF6CCBF0, 2);
        g_info_labels[1] = Label(dialog, L"OcL10nItemText", 101, 418, 48, 18, L"道具", 0xFF6CCBF0, 2);
        g_info_dialog = dialog;
        ++g_stats.info;
        Log("factory l10n: OC page labels in Chinese");
    }
    for (auto* control : g_info_controls) Show(control, shown);
    for (auto* control : g_info_labels) Show(control, shown);
}

void CategoryBar(void* bar, std::uint32_t new_mask) {
    if (!g_ui || !bar) return;
    void* dialog = ParentOf(ReadPointer(bar, kBarNewIcons));
    if (!dialog) return;
    if (dialog != g_bar_dialog) {
        g_badges = {};
        g_bar_dialog = dialog;
    }
    for (std::size_t index = 0; index < 8; ++index) {
        void* badge = ReadPointer(bar, kBarNewIcons + index * 4);
        const bool shown = Visible(badge);
        if (!g_badges[index]) {
            // NewIcon<n> at (36, 27 + 56n) in the bar dialog, a red 36x17 badge.
            const std::wstring name = L"OcL10nNew" + std::to_wstring(index);
            g_badges[index] = Label(dialog, name.c_str(), 36, 27 + 56 * static_cast<int>(index), 36, 17, L"新", 0xFFFF5A5A, 1);
        }
        if (shown) {
            Show(badge, false);
            ++g_stats.badges;
        }
        // The native badge stays hidden; the category's own NEW flag decides.
        Show(g_badges[index], ((new_mask >> index) & 1) != 0);
    }
}

void ExitButton(void* control) {
    // 0802 ExitBtn: sprs\7565-7567 engrave EXIT under its 返回游戏 caption; the
    // client's own text-free button (sprs\12833-12835) replaces them.
    if (!control || Swapped(control)) return;
    if (ReplaceSkins(control, {12833, 12834, 12835})) Log("factory l10n: exit button without EXIT");
}

Stats GetStats() { return g_stats; }

} // namespace sdgo::oc_custom::client::l10n
