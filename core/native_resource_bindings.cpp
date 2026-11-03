#include "sdgo/resource_bindings.h"
#include "sdgo/script_registration.h"
#include <windows.h>
#include <cstring>

namespace sdgo::resources {
namespace {
static_assert(sizeof(void*) == 4, "native constructor resource binding requires x86");
struct Registers { std::uint32_t edi, esi, ebp, savedEsp, ebx, edx, ecx, eax, flags; };
std::shared_ptr<const Bindings> g_bindings;
void* g_original = nullptr;
CloseCandidateClient g_close = nullptr;
void* g_closeContext = nullptr;
alignas(16) const std::uint32_t g_defaultMxcsr = 0x1f80;
constexpr DWORD kFatalCandidate = 0xe17d1001;

[[noreturn]] void Fatal(const char* reason) noexcept {
    try { if (g_close) g_close(g_closeContext, reason); } catch (...) { }
    TerminateProcess(GetCurrentProcess(), kFatalCandidate);
    for (;;) Sleep(INFINITE);
}
bool ReadEntry(const Registers* r, std::uint32_t* id) noexcept {
    __try { std::memcpy(id, reinterpret_cast<const void*>(r->ebp + 8), 4); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadOwned(const Registers* r, std::uint8_t* record, const wchar_t*** argument) noexcept {
    __try {
        const std::uint8_t* source = nullptr; std::memcpy(&source, reinterpret_cast<const void*>(r->ebx + 0x9c8), 4);
        std::memcpy(record, source, 1871);
        *argument = reinterpret_cast<const wchar_t**>(r->savedEsp + 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ResolveOwned(const Bindings* bindings, std::uint32_t id, const std::uint8_t* record, const wchar_t** argument, Result* result) noexcept {
    __try { *result = bindings->resolve(id, record, 1871, *argument); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __cdecl PatchArgument(const Registers* registers) noexcept {
    const DWORD lastError = GetLastError(); std::uint32_t id = 0;
    if (!ReadEntry(registers, &id)) Fatal("native constructor frame is unavailable");
    if (!g_bindings || !g_bindings->owns(id)) { SetLastError(lastError); return; }
    std::uint8_t record[1871] = {}; const wchar_t** argument = nullptr;
    if (!ReadOwned(registers, record, &argument)) Fatal("owned native constructor record is unavailable");
    Result result;
    if (!ResolveOwned(g_bindings.get(), id, record, argument, &result)) Fatal("owned native resource argument is unreadable");
    if (result.resolution != Resolution::Redirected || !result.path) Fatal(result.reason);
    *argument = result.path;
    SetLastError(lastError);
}
__declspec(naked) void FixedEffectArgument() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 0x210 }
    __asm { and esp, 0xfffffff0 }
    __asm { mov [esp + 0x200], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push eax }
    __asm { call PatchArgument }
    __asm { add esp, 4 }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp + 0x200] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_original] }
}
}

bool InstallFixedEffectHook(const SdgoApi* api, SdgoMod* mod, std::shared_ptr<const Bindings> bindings,
    CloseCandidateClient closeClient, void* closeContext, std::string& error) {
    error.clear();
    if (g_bindings || g_original) { error = "native fixed-effect hook already has an owner"; return false; }
    if (!bindings || !bindings->fixedEffectCount() || !closeClient || !api || api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->Symbol || !api->HookSite) { error = "native fixed-effect hook prerequisites unavailable"; return false; }
    auto* constructor = static_cast<const std::uint8_t*>(api->Symbol(mod, "missile.resource.constructor"));
    auto* site = static_cast<const std::uint8_t*>(api->Symbol(mod, "missile.resource.fixed_effect_site"));
    if (!constructor || !site || site != constructor + 0x359) { error = "verified native constructor/site relation differs"; return false; }
    const std::vector<std::uint8_t> bytes(constructor, constructor + 1408);
    if (scripts::Sha256(bytes) != "80b2bd77367d4294ced6df49fecc9bfc40f5bddd502d334e5f6ac40942606bd3") { error = "whole native constructor fingerprint differs"; return false; }
    g_bindings = std::move(bindings); g_close = closeClient; g_closeContext = closeContext;
    if (!api->HookSite(mod, "missile.resource.fixed_effect_site", reinterpret_cast<void*>(&FixedEffectArgument), &g_original)) { g_bindings.reset(); g_close = nullptr; g_closeContext = nullptr; error = "native fixed-effect instruction hook was refused"; return false; }
    return true;
}
} // namespace sdgo::resources
