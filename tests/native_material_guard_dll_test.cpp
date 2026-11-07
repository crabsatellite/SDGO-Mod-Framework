// Execute the production native_data_guard DLL and its material-result site stub.
#include <windows.h>
#include <float.h>
#include <xmmintrin.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "sdgo/sdgo_api.h"
#include "sdgo/native_data_guard_observer.h"

namespace {

unsigned checks = 0;

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

template<class T>
void Put(void* data, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(data) + offset, &value, sizeof(value));
}

uint8_t* bounds = nullptr;
void* materialLookupHook = nullptr;
void* materialResultHook = nullptr;
unsigned nativeLookups = 0;
int observedIndex = 0;
uint8_t observedShared = 0, observedInstance = 0;
void* observedModel = nullptr;
void* sharedMaterial = reinterpret_cast<void*>(0x13572468u);

volatile LONG trampolineCalls = 0, nextCalls = 0;
uint32_t storedMaterial = 0, afterEax = 0, afterEcx = 0, afterEdx = 0;
uint32_t afterEsi = 0, afterEdi = 0, nextEsi = 0;
uint32_t trampolineFlags = 0, nextFlags = 0;

void* __fastcall OriginalMaterialLookup(
    void* model, void*, int index, uint8_t shared, uint8_t instance) {
    ++nativeLookups;
    observedModel = model;
    observedIndex = index;
    observedShared = shared;
    observedInstance = instance;
    if (shared) return sharedMaterial;
    const size_t offset = instance ? 0x164 : 0x154;
    void** begin = nullptr;
    std::memcpy(&begin, static_cast<uint8_t*>(model) + offset, sizeof(begin));
    return begin[index];
}

__declspec(naked) void OriginalMaterialResultSite() {
    __asm { mov [ebp-38h], eax }
    __asm { test edi, edi }
    __asm { pushfd }
    __asm { pop dword ptr [trampolineFlags] }
    __asm { lock inc dword ptr [trampolineCalls] }
    __asm { ret }
}

__declspec(naked) void DecalMaterialNext() {
    __asm { inc esi }
    __asm { mov dword ptr [nextEsi], esi }
    __asm { pushfd }
    __asm { pop dword ptr [nextFlags] }
    __asm { lock inc dword ptr [nextCalls] }
    __asm { ret }
}

__declspec(naked) void GenericOriginal() {
    __asm { ret }
}

__declspec(naked) void __stdcall InvokeMaterialResult(void*, int) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { sub esp, 38h }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { mov dword ptr [ebp-38h], 0DEADBEEFh }
    __asm { mov eax, [ebp+8] }
    __asm { mov edi, [ebp+0Ch] }
    __asm { mov esi, 7 }
    __asm { mov ecx, 22334455h }
    __asm { mov edx, 33445566h }
    __asm { call dword ptr [materialResultHook] }
    __asm { mov ebx, [ebp-38h] }
    __asm { mov dword ptr [storedMaterial], ebx }
    __asm { mov dword ptr [afterEax], eax }
    __asm { mov dword ptr [afterEcx], ecx }
    __asm { mov dword ptr [afterEdx], edx }
    __asm { mov dword ptr [afterEsi], esi }
    __asm { mov dword ptr [afterEdi], edi }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
}

void Log(SdgoMod*, int, const char*, ...) {}

void* Symbol(SdgoMod*, const char* key) {
    if (!std::strcmp(key, "native_data_guard.funnel_bounds")) return bounds + 6;
    if (!std::strcmp(key, "native_data_guard.decal_material_next"))
        return reinterpret_cast<void*>(&DecalMaterialNext);
    if (!std::strcmp(key, "native_data_guard.material_lookup"))
        return reinterpret_cast<void*>(&OriginalMaterialLookup);
    if (!std::strcmp(key, "native_data_guard.decal_material_result"))
        return reinterpret_cast<void*>(&OriginalMaterialResultSite);
    return reinterpret_cast<void*>(&GenericOriginal);
}

int HookCode(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.material_lookup")) {
        materialLookupHook = detour;
        *original = reinterpret_cast<void*>(&OriginalMaterialLookup);
    } else {
        *original = reinterpret_cast<void*>(&GenericOriginal);
    }
    return 1;
}

int HookSite(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.decal_material_result")) {
        materialResultHook = detour;
        *original = reinterpret_cast<void*>(&OriginalMaterialResultSite);
    } else {
        *original = reinterpret_cast<void*>(&GenericOriginal);
    }
    return 1;
}

int HookImport(SdgoMod*, const char*, const char*, void*, void** original) {
    *original = reinterpret_cast<void*>(&GenericOriginal);
    return 1;
}

using MaterialLookupHook =
    void*(__fastcall*)(void*, void*, int, uint8_t, uint8_t);

void SetVector(
    std::array<uint8_t, 0x180>& model, size_t offset,
    void** begin, void** end, void** capacity) {
    Put(model.data(), offset, begin);
    Put(model.data(), offset + sizeof(void*), end);
    Put(model.data(), offset + sizeof(void*) * 2, capacity);
}

void Run(SdgoModLoadFn entry) {
    bounds = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Check(bounds != nullptr, "owned funnel-bounds fixture allocated");
    const uint8_t branch[] = {
        0x8B,0x54,0x24,0x04,0x85,0xD2,0x7F,0x11,0x83,0xFA,0x06,0x7E,0x0C,
        0x31,0xC0,0xC2,0x04,0x00,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
        0xB8,0x01,0x00,0x00,0x00,0xC2,0x04,0x00};
    std::memcpy(bounds, branch, sizeof(branch));
    DWORD ignored = 0;
    Check(VirtualProtect(bounds, 4096, PAGE_EXECUTE_READ, &ignored) != 0,
          "fixture branch begins read-only");

    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = Symbol;
    api.HookCode = HookCode;
    api.HookSite = HookSite;
    api.HookImport = HookImport;
    api.Log = Log;
    Check(entry(&api, nullptr) && materialLookupHook && materialResultHook,
          "production DLL activates the checked material lookup and result branch");

    auto lookup = reinterpret_cast<MaterialLookupHook>(materialLookupHook);
    std::array<uint8_t, 0x180> model{};
    std::array<void*, 3> primary{
        reinterpret_cast<void*>(0x11110000u),
        reinterpret_cast<void*>(0x22220000u),
        reinterpret_cast<void*>(0x33330000u)};
    std::array<void*, 2> instance{
        reinterpret_cast<void*>(0x44440000u),
        reinterpret_cast<void*>(0x55550000u)};
    SetVector(model, 0x154, primary.data(), primary.data() + 3, primary.data() + 3);
    SetVector(model, 0x164, instance.data(), instance.data() + 2, instance.data() + 2);

    Check(lookup(model.data(), nullptr, 1, 0, 0) == primary[1] &&
          nativeLookups == 1 && observedModel == model.data() &&
          observedIndex == 1 && !observedShared && !observedInstance,
          "valid primary material delegates exactly once with the native ABI");
    Check(lookup(model.data(), nullptr, 1, 0, 1) == instance[1] &&
          nativeLookups == 2 && observedInstance == 1,
          "valid instance material uses the verified second vector and delegates once");

    const auto beforeRejected = nativeLookups;
    Check(!lookup(model.data(), nullptr, -1, 0, 0) &&
          !lookup(model.data(), nullptr, 3, 0, 0) &&
          !lookup(model.data(), nullptr, 7, 0, 1) &&
          nativeLookups == beforeRejected,
          "negative and short-vector indices are rejected before native indexing");

    std::array<uint8_t, 0x180> empty{};
    Check(!lookup(empty.data(), nullptr, 0, 0, 0) &&
          nativeLookups == beforeRejected,
          "an empty material vector returns null without native delegation");

    std::array<uint8_t, 0x180> malformed{};
    SetVector(malformed, 0x154, primary.data(), primary.data() + 3, primary.data() + 2);
    Check(!lookup(malformed.data(), nullptr, 0, 0, 0),
          "end beyond capacity is rejected as malformed vector metadata");
    SetVector(malformed, 0x154, nullptr, primary.data(), primary.data() + 3);
    Check(!lookup(malformed.data(), nullptr, 0, 0, 0),
          "partial null vector metadata is rejected without dereference");
    Check(nativeLookups == beforeRejected,
          "all malformed vectors remain outside the native getter");

    Check(lookup(malformed.data(), nullptr, 7, 1, 1) == sharedMaterial &&
          nativeLookups == beforeRejected + 1 && observedShared == 1 &&
          observedIndex == 7 && observedInstance == 1,
          "shared registry lookup delegates unchanged despite local vector metadata");

    const auto mxcsr = _mm_getcsr();
    unsigned int control = 0;
    _controlfp_s(&control, 0, 0);
    SetLastError(0x4567);
    InvokeMaterialResult(primary[2], 1);
    unsigned int afterControl = 0;
    _controlfp_s(&afterControl, 0, 0);
    Check(trampolineCalls == 1 && !nextCalls &&
          storedMaterial == reinterpret_cast<uintptr_t>(primary[2]) &&
          afterEax == reinterpret_cast<uintptr_t>(primary[2]) &&
          afterEcx == 0x22334455 && afterEdx == 0x33445566 &&
          afterEsi == 7 && afterEdi == 1 &&
          (trampolineFlags & 0x40u) == 0 &&
          GetLastError() == 0x4567 && _mm_getcsr() == mxcsr &&
          afterControl == control,
          "present material runs the HookSite trampoline with displaced state intact");

    SetLastError(0x5678);
    InvokeMaterialResult(nullptr, 1);
    _controlfp_s(&afterControl, 0, 0);
    Check(trampolineCalls == 1 && nextCalls == 1 &&
          storedMaterial == 0xDEADBEEF && afterEax == 0 &&
          afterEcx == 0x22334455 && afterEdx == 0x33445566 &&
          afterEsi == 8 && nextEsi == 8 && afterEdi == 1 &&
          (nextFlags & 0x40u) == 0 &&
          GetLastError() == 0x5678 && _mm_getcsr() == mxcsr &&
          afterControl == control,
          "null material advances through the supplied native next-iteration target");

    VirtualFree(bounds, 0, MEM_RELEASE);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        HMODULE dll = LoadLibraryW(argv[1]);
        auto entry = dll ? reinterpret_cast<SdgoModLoadFn>(
            GetProcAddress(dll, "SdgoModLoad")) : nullptr;
        Check(entry != nullptr, "production DLL exports its loader");
        Run(entry);
        using Status = int(__cdecl*)(SdgoNativeDataGuardStatusV5*);
        const auto status = reinterpret_cast<Status>(
            GetProcAddress(dll, "SdgoNativeDataGuardGetStatusV5"));
        SdgoNativeDataGuardStatusV5 value{};
        value.size = sizeof(value);
        Check(status && status(&value) && value.version == 5 &&
              value.unavailable_material_slots == 6 && value.skipped_decal_slots == 1,
              "V5 exposes actual rejected material lookups and the native decal skip branch");
        FreeLibrary(dll);
        std::printf("PASS %u production material-guard checks\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL %s\n", error.what());
        return 1;
    }
}
