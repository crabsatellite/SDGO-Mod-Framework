#pragma once

void* beamLoopHook = nullptr;
void* beamActiveLoop = nullptr;
uint32_t beamRemaining = 0, beamCalls = 0, beamFinished = 0;
uint32_t beamCountAfter = 0, beamCookieAfter = 0;
uint32_t beamRegisters[8]{}, beamFlags = 0, beamExpectedFlags = 0;
uint32_t beamExpectedEsp = 0, beamExpectedEbp = 0;
alignas(16) uint8_t beamFpSaved[512]{}, beamFpBefore[512]{}, beamFpAfter[512]{};

__declspec(naked) void BeamCapture() {
    __asm {
        mov dword ptr [beamRegisters], eax
        mov dword ptr [beamRegisters+4], ecx
        mov dword ptr [beamRegisters+8], edx
        mov dword ptr [beamRegisters+12], ebx
        mov dword ptr [beamRegisters+16], esi
        mov dword ptr [beamRegisters+20], edi
        mov dword ptr [beamRegisters+24], ebp
        lea eax, [esp+4]
        mov dword ptr [beamRegisters+28], eax
        pushfd
        pop dword ptr [beamFlags]
        ret
    }
}
__declspec(naked) void BeamFinish() {
    __asm {
        call BeamCapture
        inc dword ptr [beamFinished]
        ret
    }
}
__declspec(naked) void BeamLoopOriginal() {
    __asm {
        call BeamCapture
        movzx eax, byte ptr [ebp-394h]
        mov dword ptr [ebp+eax*4-40h], 8EBh
        inc byte ptr [ebp-394h]
        inc dword ptr [beamCalls]
        dec dword ptr [beamRemaining]
        jnz another_target
        ret
    another_target:
        jmp dword ptr [beamActiveLoop]
    }
}
__declspec(naked) void __stdcall InvokeBeamLoop(uint32_t) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 400h
        push ebx
        push esi
        push edi
        mov eax, [ebp+8]
        mov byte ptr [ebp-394h], al
        mov dword ptr [ebp-10h], 0DEADFACEh
        mov dword ptr [beamExpectedEsp], esp
        mov dword ptr [beamExpectedEbp], ebp
        fxsave [beamFpSaved]
        fninit
        fld1
        fxsave [beamFpBefore]
        mov eax, 7FFFFFFFh
        add eax, 1
        stc
        pushfd
        pop dword ptr [beamExpectedFlags]
        mov eax, 11223344h
        mov ecx, 22334455h
        mov edx, 33445566h
        mov ebx, 44556677h
        mov esi, 55667788h
        mov edi, 66778899h
        call dword ptr [beamActiveLoop]
        fxsave [beamFpAfter]
        fxrstor [beamFpSaved]
        movzx eax, byte ptr [ebp-394h]
        mov dword ptr [beamCountAfter], eax
        mov eax, dword ptr [ebp-10h]
        mov dword ptr [beamCookieAfter], eax
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret 4
    }
}
void* BeamGuardSymbol(const char* key) {
    if (!std::strcmp(key, "native_data_guard.beam_target_loop"))
        return reinterpret_cast<void*>(&BeamLoopOriginal);
    if (!std::strcmp(key, "native_data_guard.beam_target_finish"))
        return reinterpret_cast<void*>(&BeamFinish);
    return nullptr;
}
bool BeamGuardHook(const char* key, void* detour, void** original) {
    if (std::strcmp(key, "native_data_guard.beam_target_loop")) return false;
    beamLoopHook = detour;
    *original = reinterpret_cast<void*>(&BeamLoopOriginal);
    return true;
}
void RunBeamGuardCases(HMODULE dll) {
    Check(beamLoopHook != nullptr, "actual DLL installs the native sorted target-loop guard");
    using Status = int(__cdecl*)(SdgoNativeBeamTargetGuardStatusV1*);
    auto status = reinterpret_cast<Status>(
        GetProcAddress(dll, "SdgoNativeBeamTargetGuardGetStatusV1"));
    SdgoNativeBeamTargetGuardStatusV1 before{sizeof(before)};
    Check(status && status(&before) && before.version == 1 && before.capacity == 12,
          "independent observer reports the native twelve-target capacity");
    Check(!status(nullptr), "target observer rejects null output");
    SdgoNativeBeamTargetGuardStatusV1 shortOutput{4};
    Check(!status(&shortOutput), "target observer rejects undersized output");
    const DWORD originalMxcsr = _mm_getcsr();
    _mm_setcsr(0x3F80);
    for (uint32_t initial : {0u, 1u, 10u, 11u, 12u, 13u, 255u}) {
        beamActiveLoop = beamLoopHook;
        beamRemaining = 1; beamCalls = beamFinished = 0;
        SetLastError(0xCAFE1234);
        InvokeBeamLoop(initial);
        const DWORD error = GetLastError();
        Check(beamCalls == (initial < 12 ? 1u : 0u) &&
              beamFinished == (initial >= 12 ? 1u : 0u),
              "only a full native target bank takes the original finished-loop path");
        Check(beamCountAfter == initial + (initial < 12 ? 1u : 0u) &&
              beamCookieAfter == 0xDEADFACE && error == 0xCAFE1234 && _mm_getcsr() == 0x3F80,
              "boundaries preserve the cookie, target count, native thread error and SIMD control");
        const uint32_t expected[] = {0x11223344, 0x22334455, 0x33445566,
            0x44556677, 0x55667788, 0x66778899, beamExpectedEbp, beamExpectedEsp - 4};
        Check(!std::memcmp(beamRegisters, expected, sizeof(expected)) && beamFlags == beamExpectedFlags,
              "both real x86 guard branches preserve every GPR, stack and arithmetic flag");
        Check(!std::memcmp(beamFpBefore, beamFpAfter, 160),
              "both guard branches preserve native x87 registers, tags and control state");
    }
    beamActiveLoop = reinterpret_cast<void*>(&BeamLoopOriginal);
    beamRemaining = 4; beamCalls = beamFinished = 0;
    InvokeBeamLoop(10);
    Check(beamCalls == 4 && beamCountAfter == 14 && beamCookieAfter == 0x8EB,
          "unbounded mixed-target continuation reproduces the damage value overwriting the cookie");
    beamActiveLoop = beamLoopHook;
    beamRemaining = 4; beamCalls = beamFinished = 0;
    InvokeBeamLoop(10);
    Check(beamCalls == 2 && beamFinished == 1 && beamRemaining == 2 &&
          beamCountAfter == 12 && beamCookieAfter == 0xDEADFACE,
          "actual guard admits the remaining bank slots then stops before a thirteenth write");
    _mm_setcsr(originalMxcsr);
    SdgoNativeBeamTargetGuardStatusV1 after{sizeof(after)};
    Check(status(&after) && after.bounded_loops == before.bounded_loops + 4,
          "observer counts the three full-bank inputs and the bounded mixed-target loop");
}
