#pragma once

// Both native hit-collection passes share one twelve-entry damage/message bank.
// The first pass already checks this bound; the sorted actor pass must do so
// before invoking the same native damage writer.
namespace sdgo::native_beam_target_guard {
inline void* continue_ = nullptr;
inline void* finish_ = nullptr;
inline volatile LONG bounded_loops_ = 0;
inline constexpr uint32_t kCapacity = 12;

__declspec(naked) inline void TargetLoop() {
    __asm { pushfd }
    __asm { cmp byte ptr [ebp-394h], 12 }
    __asm { jae full_bank }
    __asm { popfd }
    __asm { jmp dword ptr [continue_] }
full_bank:
    __asm { lock inc dword ptr [bounded_loops_] }
    __asm { popfd }
    __asm { jmp dword ptr [finish_] }
}

inline bool Attach(const SdgoApi* api, SdgoMod* mod) {
    finish_ = api->Symbol(mod, "native_data_guard.beam_target_finish");
    if (!finish_ || !api->Symbol(mod, "native_data_guard.beam_target_loop"))
        return false;
    return api->HookSite(mod, "native_data_guard.beam_target_loop",
                         reinterpret_cast<void*>(&TargetLoop), &continue_) != 0;
}
}  // namespace sdgo::native_beam_target_guard
