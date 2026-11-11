#include "source_list.h"
#include "sdgo/sdgo_api.h"
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {
using BuildFn = void(__thiscall*)(void*);
using RefreshFn = void(__thiscall*)(void*, int);
using AssignFn = void*(__thiscall*)(void*, const wchar_t*, std::uint32_t);
using GridDataFn = void*(__cdecl*)();
using GridFindFn = bool(__thiscall*)(void*, const std::uint8_t**, std::uint32_t, int, char);
BuildFn originalEvents = nullptr;
RefreshFn originalRefresh = nullptr;
AssignFn assign = nullptr;
GridDataFn gridData = nullptr;
GridFindFn gridFind = nullptr;
volatile LONG installed = 0;
volatile LONG uiThread = 0;
struct String {
    union { wchar_t small[8]; const wchar_t* pointer; } data{};
    std::uint32_t size = 0, capacity = 7;
};
static_assert(sizeof(String) == 24);
String View(const std::wstring& text) {
    String result;
    result.size = static_cast<std::uint32_t>(text.size());
    if (result.size < 8) std::memcpy(result.data.small, text.c_str(), (result.size + 1) * 2);
    else { result.data.pointer = text.data(); result.capacity = result.size; }
    return result;
}
struct Context {
    std::uint32_t unit = 0;
    std::uintptr_t first = 0, last = 0;
    std::array<std::uint32_t, 5> counts{};
};
struct Display { std::uint32_t unit = 0; std::wstring text; bool extra = false; void* control = nullptr; };
std::map<void*, Display> displays;

bool ReadContext(void* self, Context* context) noexcept {
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(self);
        std::memcpy(&context->unit, bytes + 0x44, 4);
        std::memcpy(&context->first, bytes + 0x178, 4);
        std::memcpy(&context->last, bytes + 0x17C, 4);
        std::memcpy(context->counts.data(), bytes + 0x1AC, 20);
        if (!context->unit || !context->first || context->last < context->first ||
            context->last - context->first > 28 * 512 || (context->last - context->first) % 28) return false;
        std::uint32_t rows = 0;
        for (const auto count : context->counts) {
            if (count > 128) return false;
            rows += (std::max)(6u, count);
        }
        return rows == (context->last - context->first) / 28;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadText(std::uintptr_t pointer, wchar_t* text, std::uint32_t* length) noexcept {
    __try {
        const auto* value = reinterpret_cast<const String*>(pointer);
        if (value->size > sdgo::acquisition_sources::kMaxText || value->size > value->capacity ||
            (value->capacity < 8 && value->size > 7)) return false;
        *length = value->size;
        const auto* source = value->capacity < 8 ? value->data.small : value->data.pointer;
        if (!source) return false;
        std::memcpy(text, source, *length * 2);
        text[*length] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Localize(void* self) noexcept {
    const auto module = GetModuleHandleW(L"ui_localization.dll");
    const auto translate = module ? reinterpret_cast<SdgoUiLocalizationAcquisitionEventsV1Fn>(
        GetProcAddress(module, SDGO_ACQUISITION_LOCALIZE_EXPORT)) : nullptr;
    if (!translate) return;
    __try { translate(self); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
std::vector<std::wstring> Collect(std::uint32_t subject, std::uint32_t id) {
    std::vector<std::wstring> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{snapshot};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            const auto provider = reinterpret_cast<SdgoAcquisitionSourcesV1Fn>(
                GetProcAddress(entry.hModule, SDGO_ACQUISITION_SOURCES_EXPORT));
            if (!provider) continue;
            auto rows = sdgo::acquisition_sources::Query(provider, subject, id);
            for (auto& text : rows) {
                if (result.size() == 128) break;
                if (std::find(result.begin(), result.end(), text) == result.end()) result.push_back(std::move(text));
            }
        } while (Module32NextW(snapshot, &entry));
    }
    return result;
}
struct Node {
    std::uintptr_t left = 0, right = 0;
    std::uint32_t id = 0, kind = 0;
    const std::uint8_t* record = nullptr;
    bool nil = true;
};
bool Tree(void* grid, std::uintptr_t* root) noexcept {
    __try {
        const auto head = *reinterpret_cast<std::uintptr_t*>(static_cast<std::uint8_t*>(grid) + 4);
        if (!head) return false;
        *root = *reinterpret_cast<std::uintptr_t*>(head + 4);
        return *root != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadNode(std::uintptr_t pointer, Node* output) noexcept {
    if (!pointer) return false;
    __try {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(pointer);
        output->nil = bytes[0x15] != 0;
        if (output->nil) return true;
        std::memcpy(&output->left, bytes, 4);
        std::memcpy(&output->right, bytes + 8, 4);
        std::memcpy(&output->id, bytes + 12, 4);
        const std::uint8_t* value = nullptr;
        std::memcpy(&value, bytes + 16, 4);
        if (!value) return false;
        std::memcpy(&output->kind, value, 4);
        std::memcpy(&output->record, value + 4, 4);
        return output->record && output->left != pointer && output->right != pointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BlueprintName(const std::uint8_t* row, std::uint32_t id, std::uint32_t unit, wchar_t* name) noexcept {
    if (!row) return false;
    __try {
        std::uint32_t identity = 0, target = 0;
        std::memcpy(&identity, row, 4);
        std::memcpy(&target, row + 229, 4);
        if (row[4] || identity != id || target != unit) return false;
        for (std::size_t i = 0; i < 50; ++i) {
            std::memcpy(name + i, row + 5 + i * 2, 2);
            if (!name[i]) return i != 0;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Primary(const std::uint8_t* row, std::uint32_t unit, std::uint32_t* recipe) noexcept {
    __try {
        std::uint32_t target = 0;
        if (!row || row[4] || row[5] != 1) return false;
        std::memcpy(&target, row + 6, 4);
        std::memcpy(recipe, row + 10, 4);
        return target == unit && *recipe != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
std::uint32_t Availability(std::uint32_t recipe, std::uint32_t unit) noexcept {
    const auto module = GetModuleHandleW(L"unit_catalog.dll");
    if (!module) return 0;
    const auto query = reinterpret_cast<SdgoBlueprintAcquisitionStateV1Fn>(
        GetProcAddress(module, SDGO_BLUEPRINT_ACQUISITION_STATE_EXPORT));
    return sdgo::acquisition_sources::BlueprintState(query, recipe, unit);
}
std::vector<std::wstring> BlueprintSources(std::uint32_t unit) {
    std::vector<std::wstring> result;
    std::vector<std::uint32_t> seen;
    std::vector<std::uint32_t> primary;
    std::vector<std::pair<std::uint32_t, std::wstring>> recipes;
    auto* grid = gridData();
    std::uintptr_t root = 0;
    if (!grid || !Tree(grid, &root)) return result;
    std::vector<std::uintptr_t> pending{root};
    for (std::uint32_t count = 0; !pending.empty() && count < 200000; ++count) {
        const auto pointer = pending.back(); pending.pop_back();
        Node node;
        if (!ReadNode(pointer, &node)) return {};
        if (node.nil) continue;
        if (pending.size() >= 128) return {};
        pending.push_back(node.left); pending.push_back(node.right);
        if (node.kind == 85) {
            std::uint32_t recipe = 0;
            if (Primary(node.record, unit, &recipe)) primary.push_back(recipe);
        }
        if (node.kind != 33 || std::find(seen.begin(), seen.end(), node.id) != seen.end()) continue;
        const std::uint8_t* definition = nullptr;
        wchar_t name[50]{};
        if (!gridFind(grid, &definition, node.id, 33, 0) || !BlueprintName(definition, node.id, unit, name)) continue;
        seen.push_back(node.id);
        recipes.emplace_back(node.id, name);
    }
    if (!pending.empty()) return {};
    for (const auto& recipe : recipes) {
        const auto state = Availability(recipe.first, unit);
        const bool nativePrimary = std::find(primary.begin(), primary.end(), recipe.first) != primary.end();
        if (!sdgo::acquisition_sources::AllowBlueprint(state, nativePrimary)) continue;
        result.push_back(std::wstring(L"设计图合成：") + recipe.second);
        for (auto& text : Collect(SDGO_ACQUISITION_BLUEPRINT, recipe.first))
            result.push_back(std::wstring(L"设计图《") + recipe.second + L"》获得：" + text);
    }
    return result;
}
std::wstring Lines(std::wstring_view value) {
    std::wstring result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size() && value[i + 1] == L'n') { result += L'\n'; ++i; }
        else result += value[i];
    }
    return result;
}
std::size_t EventStart(const Context& context) {
    std::size_t rows = 0;
    for (std::size_t i = 0; i < 4; ++i) rows += (std::max)(6u, context.counts[i]);
    return rows;
}
bool EventMarker(void* self, std::uintptr_t item) noexcept {
    __try {
        assign(reinterpret_cast<void*>(item), L"获得方式", 4);
        *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(self) + 0x1BC) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void CompleteSources(void* self) {
    Context context;
    if (!ReadContext(self, &context)) { displays.erase(self); return; }
    Display display;
    display.unit = context.unit;
    const auto start = EventStart(context);
    for (std::uint32_t i = 0; i < context.counts[4]; ++i) {
        wchar_t text[sdgo::acquisition_sources::kMaxText + 1]{};
        std::uint32_t length = 0;
        if (ReadText(context.first + (start + i) * 28, text, &length) && length) {
            if (!display.text.empty()) display.text += L'\n';
            display.text += Lines(std::wstring_view(text, length));
        }
    }
    auto sources = Collect(SDGO_ACQUISITION_UNIT, context.unit);
    auto blueprints = BlueprintSources(context.unit);
    sources.insert(sources.end(), blueprints.begin(), blueprints.end());
    for (const auto& text : sources) {
        if (display.text.find(text) != std::wstring::npos) continue;
        if (display.text.size() + text.size() + 1 > 16384) break;
        if (!display.text.empty()) display.text += L'\n';
        display.text += text;
        display.extra = true;
    }
    if (display.text.empty()) { displays.erase(self); return; }
    // The native refresh converts its first event string through a 256-wchar
    // stack buffer. Contributed text goes straight to the existing CStatic
    // after that refresh; only a short marker enters an empty native slot.
    if (!context.counts[4] && !EventMarker(self, context.first + start * 28)) return;
    if (displays.size() >= 64 && !displays.count(self)) displays.clear();
    displays[self] = std::move(display);
}
void __fastcall BuildEvents(void* self, void*) {
    InterlockedExchange(&uiThread, static_cast<LONG>(GetCurrentThreadId()));
    originalEvents(self);
    const auto error = GetLastError();
    Localize(self);
    try { CompleteSources(self); } catch (...) { displays.erase(self); }
    SetLastError(error);
}
bool SetStatic(void* self, std::size_t offset, const String* text, void** observed = nullptr) noexcept {
    using Set = void(__thiscall*)(void*, const String*);
    __try {
        auto* control = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(self) + offset);
        if (!control || !control[0x162]) return false;
        const auto method = reinterpret_cast<Set>((*reinterpret_cast<void***>(control))[0x88 / 4]);
        if (!method) return false;
        method(control, text);
        if (observed) *observed = control;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void RefreshDisplay(void* self, int firstCategory) {
    if (firstCategory < 0 || firstCategory > 4) return;
    Context context;
    if (!ReadContext(self, &context)) return;
    const auto found = displays.find(self);
    if (found == displays.end() || found->second.unit != context.unit) return;
    found->second.control = nullptr;
    int skipped = 0;
    for (int category = firstCategory; category < 5; ++category) {
        if (!context.counts[category]) { ++skipped; continue; }
        const int slot = category - skipped;
        if (slot > 3) return;
        if (category != 4) continue;
        const auto text = View(found->second.text);
        SetStatic(self, 0xC4 + slot * 4, &text, &found->second.control);
        if (found->second.extra) {
            const auto title = View(L"获得方式");
            SetStatic(self, 0x54 + slot * 4, &title);
        }
    }
}
void __fastcall Refresh(void* self, void*, int firstCategory) {
    originalRefresh(self, firstCategory);
    const auto error = GetLastError();
    try { RefreshDisplay(self, firstCategory); } catch (...) {}
    SetLastError(error);
}
std::uint32_t CopyStatic(void* control, wchar_t* output, std::uint32_t capacity) noexcept {
    if (!control || !output || !capacity) return 0;
    __try {
        const auto* bytes = static_cast<const std::uint8_t*>(control);
        const auto* parent = bytes;
        bool visible = false;
        for (unsigned depth = 0; depth < 64; ++depth) {
            if (!parent[0x162]) return 0;
            const std::uint8_t* next = nullptr;
            std::memcpy(&next, parent + 0x15C, 4);
            if (!next) { visible = true; break; }
            if (next == parent) return 0;
            parent = next;
        }
        if (!visible) return 0;
        const auto* text = reinterpret_cast<const String*>(bytes + 0x21C);
        if (!text->size || text->size > 16384 || text->size >= capacity || text->size > text->capacity ||
            (text->capacity < 8 && text->size > 7)) return 0;
        const auto* source = text->capacity < 8 ? text->data.small : text->data.pointer;
        if (!source) return 0;
        std::memcpy(output, source, text->size * 2);
        output[text->size] = 0;
        return text->size;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl SdgoAcquisitionDisplayInstalledV1() {
    return InterlockedCompareExchange(&installed, 0, 0) == 1 ? 1 : 0;
}
extern "C" __declspec(dllexport) std::uint32_t __cdecl SdgoAcquisitionDisplayTextV1(
    void* self, wchar_t* output, std::uint32_t capacity) {
    if (!InterlockedCompareExchange(&installed, 0, 0)) return 0;
    if (static_cast<DWORD>(InterlockedCompareExchange(&uiThread, 0, 0)) != GetCurrentThreadId()) return 0;
    Context context;
    if (!ReadContext(self, &context)) return 0;
    const auto found = displays.find(self);
    return found != displays.end() && found->second.unit == context.unit ?
        CopyStatic(found->second.control, output, capacity) : 0;
}
SDGO_MOD_ENTRY {
    InterlockedExchange(&installed, 0);
    InterlockedExchange(&uiThread, 0);
    displays.clear();
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol || !api->HookCode) return 0;
    assign = reinterpret_cast<AssignFn>(api->Symbol(mod, "acquisition_sources.assign"));
    gridData = reinterpret_cast<GridDataFn>(api->Symbol(mod, "acquisition_sources.grid_data"));
    gridFind = reinterpret_cast<GridFindFn>(api->Symbol(mod, "acquisition_sources.grid_find"));
    if (!assign || !gridData || !gridFind ||
        !api->HookCode(mod, "acquisition_sources.build_events", reinterpret_cast<void*>(&BuildEvents),
                       reinterpret_cast<void**>(&originalEvents)) || !originalEvents ||
        !api->HookCode(mod, "acquisition_sources.refresh", reinterpret_cast<void*>(&Refresh),
                       reinterpret_cast<void**>(&originalRefresh)) || !originalRefresh) return 0;
    InterlockedExchange(&installed, 1);
    return 1;
}
