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

std::uint32_t ListMarks() { return g_list_marks; }
std::uint32_t TitleMarks() { return g_title_marks; }
std::int32_t TitleSystem() { return g_title_system; }

} // namespace sdgo::oc_custom::client::room_marks
