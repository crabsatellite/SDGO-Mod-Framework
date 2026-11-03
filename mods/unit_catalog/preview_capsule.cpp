#include "preview_capsule.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>

#include "preview_runtime.h"

namespace sdgo::unit_preview {
namespace {

// MSVC 2010 x86 std::wstring as embedded by GOnline.exe.
struct HostWString24 {
    union {
        wchar_t inlineBuffer[8];
        const wchar_t* pointer;
    };
    uint32_t size;
    uint32_t capacity;
};
static_assert(sizeof(void*) == 4, "GOnline capsule preview requires the x86 client");
static_assert(sizeof(HostWString24) == 24, "GOnline HostWString24 layout changed");

using SlotEventFn = void(__thiscall*)(void*, const HostWString24*, uintptr_t);
using CustomUnitViewFn = void(__thiscall*)(void*, void*);
using EventDispatchFn = void(__thiscall*)(void*, void*, uint8_t);
using CapsuleInitFn = void(__thiscall*)(void*);
using CapsuleVisibilityFn = void(__thiscall*)(void*, void*);
using CapsuleModeVisibilityFn = void(__thiscall*)(void*, int);
using FindControlFn = void*(__thiscall*)(void*, const HostWString24*, uint32_t);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
SlotEventFn g_slotEvent = nullptr;
CustomUnitViewFn g_customUnitView = nullptr;
EventDispatchFn g_eventDispatch = nullptr;
CapsuleInitFn g_capsuleInit = nullptr;
CapsuleVisibilityFn g_capsuleVisibility = nullptr;
CapsuleModeVisibilityFn g_capsuleModeVisibility = nullptr;
FindControlFn g_findControl = nullptr;
const void* g_unitViewEventVtable = nullptr;
volatile LONG g_hideFailureReported = 0;

struct PendingDispatch {
    uint32_t unit;
    bool armed;
    bool observed;
    const char* renderer;
};
thread_local PendingDispatch g_pending{};

constexpr size_t kCapsulePanelOffset = 0x74;
constexpr size_t kMachineTypeOffset = 0x1A0;
constexpr size_t kMachineRowsOffset = 0x64;
constexpr size_t kSelectedMachineOffset = 0x80;
constexpr size_t kControlVisibleOffset = 0x162;
constexpr size_t kFixedMachineStride = 0x3FB;
constexpr size_t kFixedSlotCountOffset = 0x0B;
constexpr size_t kFixedUnitIdsOffset = 0x1B;
constexpr size_t kCustomCandidateControlsOffset = 0x78;
constexpr size_t kCustomSelectedControlsOffset = 0x2FC;
constexpr size_t kCustomRareControlsOffset = 0x374;
constexpr size_t kCustomCandidateRowsOffset = 0x3B0;
constexpr size_t kCustomCandidateRowsEndOffset = 0x3B4;
constexpr size_t kCustomSelectedUnitIdsOffset = 0x3FC;
constexpr size_t kCustomRareUnitIdsOffset = 0x414;
constexpr size_t kCustomPageOffset = 0x428;
constexpr size_t kCustomCandidateStride = 0x12;
constexpr size_t kCustomCandidateUnitOffset = 9;
constexpr uint32_t kFixedMachineType = 1;
constexpr uint16_t kUnitViewEvent = 0x01F8;
constexpr size_t kEventUnitOffset = 0x2C;
constexpr uint32_t kButtonControlType = 4;
constexpr uint32_t kMaximumMachineIndex = 8192;
constexpr uint32_t kMaximumFixedSlots = 6;
constexpr uint32_t kCustomCandidatesPerPage = 24;
constexpr uint32_t kCustomSelectedSlots = 6;
constexpr uint32_t kCustomRareSlots = 2;

template<class T>
bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return target != nullptr;
}

const wchar_t* HostText(const HostWString24* text) {
    if (!text || text->capacity < text->size || text->size > 31) return nullptr;
    const wchar_t* value = text->capacity < 8 ? text->inlineBuffer : text->pointer;
    return value && value[text->size] == L'\0' ? value : nullptr;
}

bool SlotFromName(const HostWString24* source, uint32_t* slot) {
    __try {
        const wchar_t* value = HostText(source);
        static constexpr wchar_t prefix[] = L"UnitRadio";
        constexpr uint32_t prefixLength =
            static_cast<uint32_t>((sizeof(prefix) / sizeof(prefix[0])) - 1);
        if (!value || source->size <= prefixLength ||
            std::wmemcmp(value, prefix, prefixLength) != 0)
            return false;

        uint32_t result = 0;
        for (uint32_t index = prefixLength; index < source->size; ++index) {
            if (value[index] < L'0' || value[index] > L'9') return false;
            result = result * 10 + static_cast<uint32_t>(value[index] - L'0');
            if (result >= kMaximumFixedSlots) return false;
        }
        *slot = result;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint32_t ReadU32(const uint8_t* address) {
    uint32_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

bool SelectedTarget(void* self, const HostWString24* source, uint32_t target) {
    __try {
        if (!self || *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(self) + kMachineTypeOffset) !=
                kFixedMachineType)
            return false;

        uint32_t slot = 0;
        if (!SlotFromName(source, &slot)) return false;

        const auto* bytes = static_cast<const uint8_t*>(self);
        const auto* rows = *reinterpret_cast<const uint8_t* const*>(
            bytes + kMachineRowsOffset);
        const uint32_t machine = *reinterpret_cast<const uint32_t*>(
            bytes + kSelectedMachineOffset);
        if (!rows || machine >= kMaximumMachineIndex) return false;

        const uint8_t* row = rows + machine * kFixedMachineStride;
        const uint32_t count = ReadU32(row + kFixedSlotCountOffset);
        if (!count || count > kMaximumFixedSlots || slot >= count) return false;
        return ReadU32(row + kFixedUnitIdsOffset + slot * sizeof(uint32_t)) ==
               target;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CustomSelectedTarget(void* self, const void* sourceControl, uint32_t target) {
    __try {
        if (!self || !sourceControl || !target) return false;
        const auto* bytes = static_cast<const uint8_t*>(self);
        const uintptr_t control = reinterpret_cast<uintptr_t>(sourceControl);

        uint32_t slot = 0;
        for (; slot < kCustomCandidatesPerPage; ++slot) {
            if (ReadU32(bytes + kCustomCandidateControlsOffset +
                        slot * sizeof(uint32_t)) == control)
                break;
        }
        if (slot < kCustomCandidatesPerPage) {
            const auto* begin = *reinterpret_cast<const uint8_t* const*>(
                bytes + kCustomCandidateRowsOffset);
            const auto* end = *reinterpret_cast<const uint8_t* const*>(
                bytes + kCustomCandidateRowsEndOffset);
            const uintptr_t beginAddress = reinterpret_cast<uintptr_t>(begin);
            const uintptr_t endAddress = reinterpret_cast<uintptr_t>(end);
            if (!beginAddress || endAddress < beginAddress) return false;
            const uintptr_t span = endAddress - beginAddress;
            if (span % kCustomCandidateStride != 0) return false;
            const uintptr_t count = span / kCustomCandidateStride;
            if (!count) return false;

            const uint32_t page = ReadU32(bytes + kCustomPageOffset);
            const uint64_t index =
                static_cast<uint64_t>(page) * kCustomCandidatesPerPage + slot;
            if (index >= count) return false;
            return ReadU32(begin + static_cast<size_t>(index) *
                                      kCustomCandidateStride +
                           kCustomCandidateUnitOffset) == target;
        }

        for (slot = 0; slot < kCustomSelectedSlots; ++slot) {
            if (ReadU32(bytes + kCustomSelectedControlsOffset +
                        slot * sizeof(uint32_t)) == control)
                return ReadU32(bytes + kCustomSelectedUnitIdsOffset +
                               slot * sizeof(uint32_t)) == target;
        }
        for (slot = 0; slot < kCustomRareSlots; ++slot) {
            if (ReadU32(bytes + kCustomRareControlsOffset +
                        slot * sizeof(uint32_t)) == control)
                return ReadU32(bytes + kCustomRareUnitIdsOffset +
                               slot * sizeof(uint32_t)) == target;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsExpectedUnitViewEvent(const void* event, uint32_t target) {
    __try {
        if (!event || *reinterpret_cast<const void* const*>(event) !=
                g_unitViewEventVtable)
            return false;
        const auto* bytes = static_cast<const uint8_t*>(event);
        return *reinterpret_cast<const uint16_t*>(bytes + sizeof(void*)) ==
                   kUnitViewEvent &&
               ReadU32(bytes + kEventUnitOffset) == target;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HideCapsuleDropButton(void* service) {
    __try {
        if (!service || !g_findControl) return false;
        void* panel = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(service) + kCapsulePanelOffset);
        if (!panel) return false;

        static constexpr wchar_t name[] = L"CapsuleDropBtn";
        HostWString24 nativeName{};
        nativeName.pointer = name;
        nativeName.size =
            static_cast<uint32_t>((sizeof(name) / sizeof(name[0])) - 1);
        nativeName.capacity = nativeName.size;
        void* control = g_findControl(panel, &nativeName, kButtonControlType);
        if (!control) return false;

        *reinterpret_cast<uint8_t*>(
            static_cast<uint8_t*>(control) + kControlVisibleOffset) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ReportHideFailureOnce() {
    if (InterlockedCompareExchange(&g_hideFailureReported, 1, 0) == 0)
        Failed("capsule", "native CapsuleDropBtn control was not found");
}

void __fastcall SlotEventDetour(
    void* self, void*, const HostWString24* source, uintptr_t sourceControl) {
    uint32_t target = 0;
    const bool selected = Target(&target, nullptr) &&
                          SelectedTarget(self, source, target);
    if (!selected) {
        g_slotEvent(self, source, sourceControl);
        return;
    }

    const PendingDispatch previous = g_pending;
    g_pending = PendingDispatch{
        target, true, false,
        "native fixed-machine UnitRadio -> unitview event 0x01F8"};
    g_slotEvent(self, source, sourceControl);
    const bool observed = g_pending.observed;
    g_pending = previous;
    if (!observed)
        Failed("capsule", "target slot did not dispatch native unitview event 0x01F8");
}

void __fastcall CustomUnitViewDetour(
    void* self, void*, void* sourceControl) {
    uint32_t target = 0;
    const bool selected = Target(&target, nullptr) &&
                          CustomSelectedTarget(self, sourceControl, target);
    if (!selected) {
        g_customUnitView(self, sourceControl);
        return;
    }

    const PendingDispatch previous = g_pending;
    g_pending = PendingDispatch{
        target, true, false,
        "native custom-machine candidate -> unitview event 0x01F8"};
    g_customUnitView(self, sourceControl);
    const bool observed = g_pending.observed;
    g_pending = previous;
    if (!observed)
        Failed("capsule",
               "target custom candidate did not dispatch native unitview event 0x01F8");
}

void __fastcall EventDispatchDetour(
    void* self, void*, void* event, uint8_t immediate) {
    const bool expected =
        g_pending.armed && !g_pending.observed &&
        IsExpectedUnitViewEvent(event, g_pending.unit);
    g_eventDispatch(self, event, immediate);
    if (expected) {
        g_pending.observed = true;
        Seen("capsule", g_pending.unit, g_pending.renderer);
    }
}

void __fastcall CapsuleInitDetour(void* self, void*) {
    g_capsuleInit(self);
    if (!HideCapsuleDropButton(self)) ReportHideFailureOnce();
}

void __fastcall CapsuleVisibilityDetour(void* self, void*, void* packet) {
    g_capsuleVisibility(self, packet);
    HideCapsuleDropButton(self);
}

void __fastcall CapsuleModeVisibilityDetour(void* self, void*, int mode) {
    g_capsuleModeVisibility(self, mode);
    HideCapsuleDropButton(self);
}

} // namespace

bool InstallCapsule(const SdgoApi* api, SdgoMod* mod) {
    g_api = api;
    g_mod = mod;
    if (!api || !mod ||
        !Bind(g_findControl, "preview.capsule.find_control") ||
        !Bind(g_unitViewEventVtable, "preview.capsule.unitview_event_vtable")) {
        Failed("capsule", "required native capsule preview symbols are unavailable");
        return false;
    }

    if (!api->HookCode(
            mod, "preview.capsule.slot_event",
            reinterpret_cast<void*>(&SlotEventDetour),
            reinterpret_cast<void**>(&g_slotEvent)) ||
        !api->HookCode(
            mod, "preview.capsule.custom_unitview",
            reinterpret_cast<void*>(&CustomUnitViewDetour),
            reinterpret_cast<void**>(&g_customUnitView)) ||
        !api->HookCode(
            mod, "preview.capsule.event_dispatch",
            reinterpret_cast<void*>(&EventDispatchDetour),
            reinterpret_cast<void**>(&g_eventDispatch)) ||
        !api->HookCode(
            mod, "preview.capsule.ui_init",
            reinterpret_cast<void*>(&CapsuleInitDetour),
            reinterpret_cast<void**>(&g_capsuleInit)) ||
        !api->HookCode(
            mod, "preview.capsule.visibility_update",
            reinterpret_cast<void*>(&CapsuleVisibilityDetour),
            reinterpret_cast<void**>(&g_capsuleVisibility)) ||
        !api->HookCode(
            mod, "preview.capsule.mode_visibility_update",
            reinterpret_cast<void*>(&CapsuleModeVisibilityDetour),
            reinterpret_cast<void**>(&g_capsuleModeVisibility))) {
        Failed("capsule", "native capsule preview hooks were rejected");
        return false;
    }
    return true;
}

} // namespace sdgo::unit_preview
