#include "room_marks.h"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace sdgo::oc_custom::client::room_marks {
namespace {

struct HostString {
    union {
        wchar_t small[8];
        const wchar_t* pointer;
    } data{};
    std::uint32_t length = 0;
    std::uint32_t capacity = 7;
};
static_assert(sizeof(HostString) == 24, "native string ABI");

using TextItemFn = HostString*(__thiscall*)(void*, std::uint32_t);
using AssignFn = HostString*(__thiscall*)(HostString*, const HostString*, std::size_t, std::size_t);
using FindFn = void*(__thiscall*)(void*, const HostString*, int);
using AddFn = std::uint32_t(__thiscall*)(void*, const HostString*, int, int, int, int, std::uint32_t);
using SetTextItemFn = void(__thiscall*)(void*, int, const HostString*, int, int, int, int, std::uint32_t,
                                        std::uint32_t, std::uint8_t, std::uint32_t, int);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
void (*g_log)(const char*) = nullptr;
TextItemFn g_text_item = nullptr;
AssignFn g_assign = nullptr;
FindFn g_find = nullptr;
AddFn g_add = nullptr;
SetTextItemFn g_set_text_item = nullptr;
const void* g_button_vtable = nullptr;
std::uint32_t g_list_marks = 0;
std::uint32_t g_title_marks = 0;
std::int32_t g_title_system = -1;
void* g_label_dialog = nullptr;
void* g_label = nullptr;
std::int32_t g_label_system = -1;

// CServiceRoomList: sorted 0x90-byte entries in [+0x124, +0x128), one row
// button per entry at +0x94 + index * 0x0C; text item 1 is the room title.
constexpr std::size_t kListBegin = 0x124, kListEnd = 0x128, kListEntryBytes = 0x90;
constexpr std::size_t kListRows = 0x94, kListRowStride = 0x0C, kListTitleItem = 1;
constexpr std::size_t kMaxRows = 64;
// CUIButton text items: vector of item pointers at +0x190/+0x194.
constexpr std::size_t kItemsBegin = 0x190, kItemsEnd = 0x194;
// C3DWaitRoomTitleBar dialog at +0x48 with the TitleChangeBtn title button.
constexpr std::size_t kTitleBarDialog = 0x48;
// Control geometry and state (as in the factory pages).
constexpr std::size_t kX = 0x148, kY = 0x14C, kWidth = 0x154, kHeight = 0x158, kParent = 0x15C;
constexpr std::size_t kVisible = 0x162, kVisible2 = 0x163;
constexpr int kButton = 4;
constexpr std::uint32_t kLabelColor = 0xFFFFFFFF;
constexpr std::uint32_t kShadowColor = 0xFF000000;
constexpr int kLabelWidth = 132;

constexpr wchar_t kMarkOc[] = L"【OC】";
constexpr wchar_t kMarkParts[] = L"【G改】";
constexpr wchar_t kLabelName[] = L"OcRoomSystemStatic";
constexpr wchar_t kTitleButton[] = L"TitleChangeBtn";
constexpr std::size_t kTextMax = 1024;

HostString View(const wchar_t* text, std::size_t length) {
    HostString view;
    view.length = static_cast<std::uint32_t>(length);
    if (length < 8) {
        std::memcpy(view.data.small, text, length * sizeof(wchar_t));
        view.data.small[length] = 0;
        view.capacity = 7;
    } else {
        view.data.pointer = text;
        view.capacity = view.length;
    }
    return view;
}

template <class T>
bool ReadAt(const void* base, std::size_t offset, T* value) noexcept {
    __try {
        std::memcpy(value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool WriteAt(void* base, std::size_t offset, const T& value) noexcept {
    __try {
        std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies a native string into text (terminated); false when unreadable.
bool CopyString(const HostString* string, wchar_t* text, std::size_t capacity, std::size_t* length) noexcept {
    __try {
        const std::size_t size = string->length;
        if (size >= capacity) return false;
        const wchar_t* source = string->capacity < 8 ? string->data.small : string->data.pointer;
        if (size && !source) return false;
        std::memcpy(text, source, size * sizeof(wchar_t));
        text[size] = 0;
        *length = size;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Assign(HostString* item, const wchar_t* text, std::size_t length) noexcept {
    const HostString view = View(text, length);
    __try {
        g_assign(item, &view, 0, static_cast<std::size_t>(-1));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HostString* TitleItem(void* control) noexcept {
    __try {
        if (!control || *static_cast<const void* const*>(control) != g_button_vtable) return nullptr;
        std::uintptr_t begin = 0, end = 0;
        std::memcpy(&begin, static_cast<std::uint8_t*>(control) + kItemsBegin, sizeof(begin));
        std::memcpy(&end, static_cast<std::uint8_t*>(control) + kItemsEnd, sizeof(end));
        if (!begin || end < begin || (end - begin) / sizeof(void*) <= kListTitleItem) return nullptr;
        return g_text_item(control, static_cast<std::uint32_t>(kListTitleItem));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool StartsWith(const wchar_t* text, std::size_t length, const wchar_t* prefix) {
    const std::size_t size = std::wcslen(prefix);
    return length >= size && std::wmemcmp(text, prefix, size) == 0;
}

// The shared room-command grammar is ASCII; other characters cannot form a
// command token and become '?'.
RoomSystem SystemOfText(const wchar_t* text, std::size_t length) {
    char narrow[kTextMax] = {};
    std::size_t size = 0;
    for (; size < length && size + 1 < sizeof(narrow); ++size)
        narrow[size] = text[size] > 0 && text[size] < 0x80 ? static_cast<char>(text[size]) : '?';
    narrow[size] = 0;
    return RoomSystemOfTitle(g_api, narrow, static_cast<std::uint32_t>(size));
}

void* FindControl(void* dialog, const wchar_t* name, int type) noexcept {
    const HostString view = View(name, std::wcslen(name));
    __try {
        return g_find(dialog, &view, type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* CreateLabel(void* dialog, void* title_button) noexcept {
    float x = 0.0f, y = 0.0f;
    int width = 0, height = 0;
    if (!ReadAt(title_button, kX, &x) || !ReadAt(title_button, kY, &y) || !ReadAt(title_button, kWidth, &width) ||
        !ReadAt(title_button, kHeight, &height) || width <= 0 || height <= 0)
        return nullptr;
    const HostString name = View(kLabelName, std::wcslen(kLabelName));
    __try {
        g_add(dialog, &name, static_cast<int>(x) + width + 6, static_cast<int>(y), kLabelWidth, height, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    void* label = FindControl(dialog, kLabelName, kButton);
    void* parent = nullptr;
    if (!label || !ReadAt(label, kParent, &parent) || parent != dialog) return nullptr;
    WriteAt<std::uint8_t>(label, kVisible, 1);
    WriteAt<std::uint8_t>(label, kVisible2, 1);
    return label;
}

bool SetLabel(void* label, const wchar_t* text) noexcept {
    int height = 0;
    if (!ReadAt(label, kHeight, &height)) return false;
    const HostString view = View(text, std::wcslen(text));
    __try {
        g_set_text_item(label, 0, &view, 2, height > 14 ? (height - 14) / 2 : 0, kLabelWidth - 4, 14, kLabelColor,
                        kShadowColor, 0, 1, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool Bind(const SdgoApi* api, SdgoMod* mod, void (*log)(const char*)) {
    g_api = api;
    g_mod = mod;
    g_log = log;
    g_text_item = reinterpret_cast<TextItemFn>(api->Symbol(mod, "oc_custom.client_button_text_item"));
    g_assign = reinterpret_cast<AssignFn>(api->Symbol(mod, "oc_custom.client_native_string_assign"));
    g_find = reinterpret_cast<FindFn>(api->Symbol(mod, "hangar.ui_find_control"));
    g_add = reinterpret_cast<AddFn>(api->Symbol(mod, "hangar.ui_add_button"));
    g_set_text_item = reinterpret_cast<SetTextItemFn>(api->Symbol(mod, "hangar.ui_text_item"));
    g_button_vtable = api->Symbol(mod, "hangar.ui_button_vtable");
    return g_text_item && g_assign && g_find && g_add && g_set_text_item && g_button_vtable;
}

void MarkRoomList(void* service) {
    std::uintptr_t begin = 0, end = 0;
    if (!service || !ReadAt(service, kListBegin, &begin) || !ReadAt(service, kListEnd, &end) || !begin ||
        end < begin || (end - begin) % kListEntryBytes)
        return;
    const std::size_t count = (end - begin) / kListEntryBytes;
    if (count > kMaxRows) return;
    for (std::size_t index = 0; index < count; ++index) {
        void* row = nullptr;
        if (!ReadAt(service, kListRows + index * kListRowStride, &row) || !row) continue;
        HostString* item = TitleItem(row);
        if (!item) continue;
        wchar_t text[kTextMax];
        std::size_t length = 0;
        if (!CopyString(item, text, kTextMax, &length)) continue;
        if (StartsWith(text, length, kMarkOc) || StartsWith(text, length, kMarkParts)) continue;
        const wchar_t* mark = SystemOfText(text, length) == RoomSystem::oc ? kMarkOc : kMarkParts;
        const std::size_t mark_length = std::wcslen(mark);
        if (mark_length + length + 1 > kTextMax) continue;
        wchar_t marked[kTextMax];
        std::wmemcpy(marked, mark, mark_length);
        std::wmemcpy(marked + mark_length, text, length);
        marked[mark_length + length] = 0;
        if (Assign(item, marked, mark_length + length)) ++g_list_marks;
    }
}

void MarkTitleBar(void* title_bar, RoomSystem system) {
    void* dialog = nullptr;
    if (!title_bar || !ReadAt(title_bar, kTitleBarDialog, &dialog) || !dialog) return;
    // Looked up by name every update: the dialog owns the label, and a later
    // dialog may reuse an address.
    void* label = FindControl(dialog, kLabelName, kButton);
    if (!label) {
        void* title_button = FindControl(dialog, kTitleButton, kButton);
        if (!title_button) return;
        label = CreateLabel(dialog, title_button);
        if (!label) return;
        if (g_log) g_log("oc room system label created next to the waiting-room title");
    }
    if (label != g_label || dialog != g_label_dialog) {
        g_label = label;
        g_label_dialog = dialog;
        g_label_system = -1;
    }
    const auto wanted = static_cast<std::int32_t>(system);
    if (g_label_system == wanted) return;
    if (!SetLabel(g_label, system == RoomSystem::oc ? L"体系：OC" : L"体系：G改 1.0")) return;
    g_label_system = wanted;
    g_title_system = wanted;
    ++g_title_marks;
    if (g_log) {
        char line[96];
        std::snprintf(line, sizeof(line), "oc room system label shows %s", RoomSystemName(system));
        g_log(line);
    }
}

// --- Room creation: the 体系 row ---------------------------------------------------
//
// The PvP create windows (CServiceMakeRoom, CServiceMakeRoomAI; uirs\1304)
// get a 体系 row in the left panel between the minimap (bottom 177) and
// SettingBtn (top 211), drawn like the window's own selectors: the left and
// right arrows use the game-mode arrows' sprites (sprs\17915-17917,
// sprs\17918-17920, 19x19) and the value sits between them. The row edits
// the room name: OC adds the !oc command, G改 1.0 removes it; typing the
// command by hand shows the same value. Services: dialog root +0x50,
// room-name edit +0x54, ready from state (+0x44) 4.
namespace {

using ServiceFindFn = void*(__thiscall*)(void*, const void*);
using EditGetTextFn = const HostString*(__thiscall*)(void*);
using EditSetTextFn = void(__thiscall*)(void*, const HostString*, int);
using SkinFn = void(__thiscall*)(void*, const HostString*, std::uint8_t, std::uint32_t, std::uint32_t);
using RefreshFn = void(__thiscall*)(void*);

void** g_service_manager = nullptr;
const void* g_create_vtables[2] = {};
const void* g_edit_vtable = nullptr;
EditGetTextFn g_edit_get_text = nullptr;
EditSetTextFn g_edit_set_text = nullptr;
SkinFn g_skin = nullptr;
RefreshFn g_refresh = nullptr;
std::uint8_t** g_input_context = nullptr; // shared native text input; HWND at +0x134
std::uint32_t g_create_shown = 0;
std::uint32_t g_create_toggles = 0;
std::int32_t g_create_system = -1;
void* g_create_root = nullptr;
std::int32_t g_create_label_system = -1;

constexpr std::size_t kServiceState = 0x44, kServiceRoot = 0x50, kServiceEdit = 0x54;
constexpr std::size_t kEditLimitBytes = 0x2A0;
constexpr std::size_t kEditFocused = 0x165, kInputWindow = 0x134;
constexpr std::size_t kEnabled = 0x167, kDebugQuad = 0x168;
constexpr wchar_t kCreateLeft[] = L"OcSystemLeftBtn";
constexpr wchar_t kCreateRight[] = L"OcSystemRightBtn";
constexpr wchar_t kCreateValue[] = L"OcSystemStatic";
constexpr int kRowArrowY = 186, kRowLeftX = 41, kRowRightX = 180, kArrowSize = 19;
constexpr int kRowValueX = 62, kRowValueY = 189, kRowValueWidth = 116, kRowValueHeight = 15;
constexpr std::uint32_t kButtonClick = 0x101;

void* FindServiceByVtable(const void* vtable) noexcept {
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

struct CreateWindow {
    void* service = nullptr;
    void* root = nullptr;
    void* edit = nullptr;
};

bool ReadyWindow(void* service, CreateWindow* window) noexcept {
    std::uint32_t state = 0;
    void* root = nullptr;
    void* edit = nullptr;
    std::uint8_t visible = 0;
    const void* edit_vtable = nullptr;
    if (!ReadAt(service, kServiceState, &state) || state < 4 || !ReadAt(service, kServiceRoot, &root) || !root ||
        !ReadAt(root, kVisible, &visible) || !visible || !ReadAt(service, kServiceEdit, &edit) || !edit ||
        !ReadAt(edit, 0, &edit_vtable) || edit_vtable != g_edit_vtable)
        return false;
    *window = CreateWindow{service, root, edit};
    return true;
}

bool EditText(void* edit, wchar_t* text, std::size_t capacity, std::size_t* length) noexcept {
    const HostString* value = nullptr;
    __try {
        value = g_edit_get_text(edit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return value && CopyString(value, text, capacity, length);
}

// While the edit has focus the shared native input window holds the text the
// player types and the edit shows; it is replaced too, caret at the end.
HWND FocusedInputWindow(void* edit) noexcept {
    std::uint8_t focused = 0;
    if (!g_input_context || !ReadAt(edit, kEditFocused, &focused) || !focused) return nullptr;
    __try {
        auto* input = *g_input_context;
        HWND window = input ? *reinterpret_cast<HWND*>(input + kInputWindow) : nullptr;
        return window && IsWindow(window) ? window : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SetEditText(void* edit, const wchar_t* text, std::size_t length) noexcept {
    const HostString view = View(text, length);
    __try {
        g_edit_set_text(edit, &view, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (HWND window = FocusedInputWindow(edit)) {
        SendMessageW(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text));
        SendMessageW(window, EM_SETSEL, static_cast<WPARAM>(length), static_cast<LPARAM>(length));
    }
    return true;
}

bool Skin(void* control, const wchar_t* sprite) noexcept {
    const HostString view = View(sprite, std::wcslen(sprite));
    __try {
        g_skin(control, &view, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* EnsureButton(void* root, const wchar_t* name, int x, int y, int width, int height, const wchar_t* sprite_base,
                   int first_sprite) noexcept {
    void* control = FindControl(root, name, kButton);
    if (control) return control;
    const HostString view = View(name, std::wcslen(name));
    __try {
        g_add(root, &view, x, y, width, height, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    control = FindControl(root, name, kButton);
    void* parent = nullptr;
    if (!control || !ReadAt(control, kParent, &parent) || parent != root) return nullptr;
    // +0x162 visible, +0x163 drawn, +0x167 interactive; appended controls also
    // clear the debug rectangle (+0x168) that loaded controls turn off.
    WriteAt<std::uint8_t>(control, kVisible, 1);
    WriteAt<std::uint8_t>(control, kVisible2, 1);
    WriteAt<std::uint8_t>(control, kEnabled, sprite_base ? 1 : 0);
    WriteAt<std::uint8_t>(control, kDebugQuad, 0);
    if (sprite_base) {
        for (int state = 0; state < 3; ++state) {
            wchar_t sprite[48];
            std::swprintf(sprite, 48, L"%ls%d.spr", sprite_base, first_sprite + state);
            Skin(control, sprite);
        }
    }
    __try {
        if (g_refresh) {
            g_refresh(control);
            g_refresh(root);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return control;
}

// Removes every standalone !oc token (and the space before it) from text.
std::size_t WithoutOc(const wchar_t* text, std::size_t length, wchar_t* out) {
    std::size_t size = 0;
    for (std::size_t index = 0; index < length;) {
        const bool start = index == 0 || text[index - 1] == L' ';
        const bool token = start && index + 3 <= length && text[index] == L'!' &&
                           (text[index + 1] == L'o' || text[index + 1] == L'O') &&
                           (text[index + 2] == L'c' || text[index + 2] == L'C') &&
                           (index + 3 == length || text[index + 3] == L' ');
        if (token) {
            if (size && out[size - 1] == L' ') --size;
            index += 3;
            continue;
        }
        out[size++] = text[index++];
    }
    out[size] = 0;
    return size;
}

// Bytes the title takes in the native narrow field (ASCII 1, others 2).
std::size_t NativeBytes(const wchar_t* text, std::size_t length) {
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < length; ++index) bytes += text[index] < 0x80 ? 1 : 2;
    return bytes;
}

bool ToggleCreate(const CreateWindow& window) {
    wchar_t text[kTextMax];
    std::size_t length = 0;
    if (!EditText(window.edit, text, kTextMax, &length)) return false;
    const bool oc = SystemOfText(text, length) == RoomSystem::oc;
    wchar_t updated[kTextMax];
    std::size_t size = WithoutOc(text, length, updated);
    if (!oc) {
        // The native field holds 20 bytes unless the long-title Mod widened the
        // edit (UTF-16 byte limit at +0x2A0); keep the command inside it.
        std::uint32_t limit = 0;
        ReadAt(window.edit, kEditLimitBytes, &limit);
        const bool wide = limit >= 128;
        const std::size_t suffix = size ? 4 : 3; // " !oc" or "!oc"
        while (size && (wide ? (size + suffix) * 2 > limit : NativeBytes(updated, size) + suffix > 20)) --size;
        while (size && updated[size - 1] == L' ') --size;
        if (size) updated[size++] = L' ';
        std::wmemcpy(updated + size, L"!oc", 3);
        size += 3;
        updated[size] = 0;
    }
    if (!SetEditText(window.edit, updated, size)) return false;
    ++g_create_toggles;
    if (g_log) g_log(oc ? "oc create window: system set to the current custom parts" : "oc create window: system set to OC (!oc)");
    return true;
}

// Every live service with one of the create-window vtables (diagnostic list).
std::size_t CreateServices(void** found, std::size_t capacity) noexcept {
    std::size_t count = 0;
    __try {
        if (!g_service_manager || !*g_service_manager) return 0;
        auto* owner = static_cast<std::uint8_t*>(*g_service_manager);
        auto* head = *reinterpret_cast<std::uint8_t**>(owner + 0x44);
        auto* node = head ? *reinterpret_cast<std::uint8_t**>(head) : nullptr;
        for (unsigned n = 0; node && node != head && n < 512; ++n) {
            void* service = *reinterpret_cast<void**>(node + 8);
            const void* vtable = service ? *static_cast<const void* const*>(service) : nullptr;
            if (vtable && (vtable == g_create_vtables[0] || vtable == g_create_vtables[1]) && count < capacity)
                found[count++] = service;
            node = *reinterpret_cast<std::uint8_t**>(node);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

// The newest ready create window. The native page normally keeps one; when
// several are open the topmost (the most recently created) is the one shown.
bool FindReadyWindow(CreateWindow* window) {
    void* found[16] = {};
    const std::size_t count = CreateServices(found, 16);
    static std::size_t logged = 0;
    if (count != logged && g_log) {
        logged = count;
        char line[96];
        std::snprintf(line, sizeof(line), "oc create window services=%u", static_cast<unsigned>(count));
        g_log(line);
    }
    for (std::size_t index = count; index-- > 0;)
        if (ReadyWindow(found[index], window)) return true;
    return false;
}

} // namespace

bool BindCreate(const SdgoApi* api, SdgoMod* mod) {
    g_service_manager = static_cast<void**>(api->Symbol(mod, "flow.service_managed_pointer"));
    g_create_vtables[0] = api->Symbol(mod, "oc_custom.client_make_room_vtable");
    g_create_vtables[1] = api->Symbol(mod, "oc_custom.client_make_room_ai_vtable");
    g_edit_vtable = api->Symbol(mod, "oc_custom.client_edit_vtable");
    g_edit_get_text = reinterpret_cast<EditGetTextFn>(api->Symbol(mod, "oc_custom.client_edit_get_text"));
    g_edit_set_text = reinterpret_cast<EditSetTextFn>(api->Symbol(mod, "oc_custom.client_edit_set_text"));
    g_skin = reinterpret_cast<SkinFn>(api->Symbol(mod, "hangar.ui_add_skin"));
    g_refresh = reinterpret_cast<RefreshFn>(api->Symbol(mod, "friend_unit_gift.ui_refresh_geometry"));
    g_input_context = static_cast<std::uint8_t**>(api->Symbol(mod, "room_titles.ui_input_context"));
    return g_service_manager && g_create_vtables[0] && g_create_vtables[1] && g_edit_vtable && g_edit_get_text &&
           g_edit_set_text && g_skin && g_refresh && g_input_context && g_add && g_find && g_set_text_item;
}

void UpdateCreateWindow() {
    CreateWindow window;
    if (!FindReadyWindow(&window)) {
        g_create_root = nullptr;
        g_create_label_system = -1;
        return;
    }
    void* left = EnsureButton(window.root, kCreateLeft, kRowLeftX, kRowArrowY, kArrowSize, kArrowSize, L"sprs\\", 17915);
    void* right = EnsureButton(window.root, kCreateRight, kRowRightX, kRowArrowY, kArrowSize, kArrowSize, L"sprs\\", 17918);
    void* value = EnsureButton(window.root, kCreateValue, kRowValueX, kRowValueY, kRowValueWidth, kRowValueHeight,
                               nullptr, 0);
    if (!left || !right || !value) return;
    if (window.root != g_create_root) {
        g_create_root = window.root;
        g_create_label_system = -1;
        ++g_create_shown;
        if (g_log) g_log("oc create window: 体系 row shown");
    }
    wchar_t text[kTextMax];
    std::size_t length = 0;
    if (!EditText(window.edit, text, kTextMax, &length)) return;
    const auto system = static_cast<std::int32_t>(SystemOfText(text, length));
    g_create_system = system;
    if (system == g_create_label_system) return;
    const wchar_t* label = system == static_cast<std::int32_t>(RoomSystem::oc) ? L"体系：OC" : L"体系：G改 1.0";
    const HostString view = View(label, std::wcslen(label));
    __try {
        g_set_text_item(value, 0, &view, 0, 0, kRowValueWidth, kRowValueHeight, kLabelColor, kShadowColor, 0, 1, 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    g_create_label_system = system;
}

bool HandleCreateEvent(void* service, std::uint32_t event, void* control) {
    if (event != kButtonClick || !control) return false;
    CreateWindow window;
    if (!service || !ReadyWindow(service, &window)) return false;
    void* left = FindControl(window.root, kCreateLeft, kButton);
    void* right = FindControl(window.root, kCreateRight, kButton);
    if (control != left && control != right) return false;
    ToggleCreate(window);
    g_create_label_system = -1; // refresh the value on the next update
    return true;
}

bool ClickCreate(const wchar_t* name) {
    CreateWindow window;
    if (!name || !FindReadyWindow(&window)) return false;
    void* control = FindControl(window.root, name, kButton);
    if (!control) return false;
    // The window's own UI callback (vtable slot 14), as a mouse click delivers it.
    using Callback = void(__stdcall*)(void*, std::uint32_t, std::uintptr_t, void*);
    Callback callback = nullptr;
    __try {
        callback = (*static_cast<Callback* const*>(window.service))[14];
        callback(window.service, kButtonClick, 0, control);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool CreateOpen() {
    CreateWindow window;
    return FindReadyWindow(&window);
}

std::uint32_t CreateShown() { return g_create_shown; }
std::uint32_t CreateToggles() { return g_create_toggles; }
std::int32_t CreateSystem() { return g_create_system; }

std::uint32_t ListMarks() { return g_list_marks; }
std::uint32_t TitleMarks() { return g_title_marks; }
std::int32_t TitleSystem() { return g_title_system; }

} // namespace sdgo::oc_custom::client::room_marks
