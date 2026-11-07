#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

namespace native_data_guard_material {

using MaterialLookup = void*(__thiscall*)(void*, int, uint8_t, uint8_t);

inline MaterialLookup g_materialLookup = nullptr;
inline void* g_decalMaterialTrampoline = nullptr;
inline void* g_decalMaterialNext = nullptr;
inline volatile LONG g_unavailableMaterialSlots = 0;
inline volatile LONG g_nativeMaterialLookups = 0;
inline volatile LONG g_skippedDecalMaterials = 0;

struct MaterialVector {
    void** begin;
    void** end;
    void** capacity;
};

static_assert(sizeof(MaterialVector) == 12);

inline MaterialVector ReadMaterialVector(const void* model, uint8_t instance) {
    constexpr size_t kPrimaryMaterials = 0x154;
    constexpr size_t kInstanceMaterials = 0x164;
    const auto* bytes = static_cast<const uint8_t*>(model);
    MaterialVector materials{};
    std::memcpy(
        &materials, bytes + (instance ? kInstanceMaterials : kPrimaryMaterials),
        sizeof(materials));
    return materials;
}

inline bool ValidMaterialIndex(
    const MaterialVector& materials, int index) {
    if (index < 0) return false;

    const auto begin = reinterpret_cast<uintptr_t>(materials.begin);
    const auto end = reinterpret_cast<uintptr_t>(materials.end);
    const auto capacity = reinterpret_cast<uintptr_t>(materials.capacity);
    if (!begin) return false;
    if (!end || !capacity || begin > end || end > capacity) return false;
    if ((begin % alignof(void*)) || (end % alignof(void*)) ||
        (capacity % alignof(void*))) return false;
    if (((end - begin) % sizeof(void*)) ||
        ((capacity - begin) % sizeof(void*))) return false;

    const auto count = (end - begin) / sizeof(void*);
    return static_cast<uintptr_t>(index) < count;
}

inline void* __fastcall CheckedMaterialLookup(
    void* model, void*, int index, uint8_t shared, uint8_t instance) {
    // The shared-material branch uses a separate native registry rather than
    // either CModel vector. Preserve that lookup exactly.
    if (shared) {
        InterlockedIncrement(&g_nativeMaterialLookups);
        return g_materialLookup(model, index, shared, instance);
    }

    if (!model || !ValidMaterialIndex(ReadMaterialVector(model, instance), index)) {
        InterlockedIncrement(&g_unavailableMaterialSlots);
        return nullptr;
    }

    InterlockedIncrement(&g_nativeMaterialLookups);
    return g_materialLookup(model, index, shared, instance);
}

// The native decal loop immediately dereferences the material returned for
// slots 7..9. A rejected or empty slot advances through the loop's existing
// INC ESI continuation so the native finalizers after the loop still run.
__declspec(naked) inline void DecalMaterialResultSite() {
    __asm { test eax, eax }
    __asm { jnz material_present }
    __asm { pushfd }
    __asm { lock inc dword ptr [g_skippedDecalMaterials] }
    __asm { popfd }
    __asm { jmp dword ptr [g_decalMaterialNext] }
material_present:
    __asm { jmp dword ptr [g_decalMaterialTrampoline] }
}

inline bool Attach(const SdgoApi* api, SdgoMod* mod) {
    if (!api || !api->Symbol || !api->HookCode || !api->HookSite) return false;

    if (!api->Symbol(mod, "native_data_guard.material_lookup") ||
        !api->Symbol(mod, "native_data_guard.decal_material_result")) return false;
    g_decalMaterialNext =
        api->Symbol(mod, "native_data_guard.decal_material_next");
    if (!g_decalMaterialNext) return false;

    if (!api->HookCode(
            mod, "native_data_guard.material_lookup",
            reinterpret_cast<void*>(&CheckedMaterialLookup),
            reinterpret_cast<void**>(&g_materialLookup))) return false;
    if (!api->HookSite(
            mod, "native_data_guard.decal_material_result",
            reinterpret_cast<void*>(&DecalMaterialResultSite),
            &g_decalMaterialTrampoline)) return false;
    return g_materialLookup && g_decalMaterialTrampoline;
}

inline uint32_t UnavailableSlots() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_unavailableMaterialSlots, 0, 0));
}

inline uint32_t NativeMaterialLookups() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_nativeMaterialLookups, 0, 0));
}

inline uint32_t SkippedDecals() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_skippedDecalMaterials, 0, 0));
}

}  // namespace native_data_guard_material
