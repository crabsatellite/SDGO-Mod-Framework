// Execute the production DLL, its x86 site stub and patched branch in owned memory.
#include <windows.h>
#include <dbghelp.h>
#include <xmmintrin.h>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/native_data_guard_observer.h"

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class T> void Put(void* data, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(data) + offset, &value, sizeof(value));
}
uint8_t* code = nullptr;
uint8_t* bounds = nullptr;
void* lookupHook = nullptr;
void* unitHook = nullptr;
void* informationHook = nullptr;
void* exceptionHook = nullptr;
void* stackWalkHook = nullptr;
bool missingUnit = false, missingName = false, missingSymbol = false;
unsigned nativeLookups = 0, gridCalls = 0, warnings = 0;
uint32_t observedUnit = 0, observedIndex = 0;
void* observedSelf = nullptr;
std::array<uint8_t, 4976> unit{};
std::array<uint8_t, 32> name{};
const uint32_t kUnit = 10014, kIndex = 2, kNameGroup = 29;
const uint32_t kNameId = (kNameGroup + 0x63F) * 100 + kIndex;
volatile LONG originalUnitCalls = 0, finalized = 0;
uint32_t afterEax = 0, afterEcx = 0, afterEdx = 0;
uint32_t informationDenominator = 0, informationEax = 0;
bool clobberFp = false;
std::vector<std::string> logs;

using StackWalkFn = BOOL(WINAPI*)(
    DWORD, HANDLE, HANDLE, LPSTACKFRAME, LPVOID,
    PREAD_PROCESS_MEMORY_ROUTINE, PFUNCTION_TABLE_ACCESS_ROUTINE,
    PGET_MODULE_BASE_ROUTINE, PTRANSLATE_ADDRESS_ROUTINE);
using ExceptionWriter = LONG(WINAPI*)(EXCEPTION_POINTERS*);
EXCEPTION_POINTERS* writerPointers = nullptr;
LPVOID writerContext = nullptr, walkContext = nullptr;
CONTEXT writerAfter{}, walkAfter{};

void Log(SdgoMod*, int level, const char* format, ...) {
    char text[1024]{};
    va_list args; va_start(args, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    logs.emplace_back(text);
    SetLastError(0xBAD0);
    if (level == SDGO_LOG_WARN) ++warnings;
    if (clobberFp) {
        _mm_setcsr(0x3F80);
        __asm { fninit }
        __asm { fld1 }
    }
}
void* Grid() { return unit.data(); }
bool __fastcall Find(void* self, void*, const uint8_t** out, uint32_t id, int type, char logging) {
    ++gridCalls;
    Check(self == unit.data() && !logging, "guard uses native silent lookup and bool ABI");
    *out = nullptr;
    if (type == 2) {
        Check(id == kUnit, "unit lookup retains the real unit key");
        if (!missingUnit) *out = unit.data();
        return !missingUnit;
    }
    Check(type == 24 && id == kNameId, "packed name-group field derives the shipped type-24 key");
    if (!missingName) *out = name.data();
    return !missingName;
}
const uint8_t* __fastcall OriginalName(void* self, void*, uint32_t id, uint32_t index) {
    ++nativeLookups; observedSelf = self; observedUnit = id; observedIndex = index;
    return missingName || missingUnit ? nullptr : name.data();
}
__declspec(naked) void FinalizeUnit() {
    __asm { mov dword ptr [afterEax], eax }
    __asm { mov dword ptr [afterEcx], ecx }
    __asm { mov dword ptr [afterEdx], edx }
    __asm { inc dword ptr [finalized] }
    __asm { ret }
}
__declspec(naked) void OriginalUnitSite() {
    __asm { cmp dword ptr [ebx+13Ch], 0 }
    __asm { inc dword ptr [originalUnitCalls] }
    __asm { jmp FinalizeUnit }
}
__declspec(naked) void OriginalInformationSite() {
    __asm { mov edi, [eax+39h] }
    __asm { mov dword ptr [informationDenominator], edi }
    __asm { mov dword ptr [informationEax], eax }
    __asm { ret }
}
__declspec(naked) void __stdcall InvokeUnitSite(const uint8_t*, const uint8_t*) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push ebx }
    __asm { push esi }
    __asm { mov ebx, [ebp+8] }
    __asm { mov esi, [ebp+12] }
    __asm { mov eax, 12345678h }
    __asm { mov ecx, 23456789h }
    __asm { mov edx, 3456789Ah }
    __asm { call dword ptr [unitHook] }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { pop ebp }
    __asm { ret 8 }
}
__declspec(naked) void __stdcall InvokeInformationSite(const uint8_t*) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { push edi }
    __asm { mov eax, [ebp+8] }
    __asm { call dword ptr [informationHook] }
    __asm { pop edi }
    __asm { pop ebp }
    __asm { ret 4 }
}
BOOL WINAPI OriginalStackWalk(
    DWORD, HANDLE, HANDLE, LPSTACKFRAME, LPVOID context,
    PREAD_PROCESS_MEMORY_ROUTINE, PFUNCTION_TABLE_ACCESS_ROUTINE,
    PGET_MODULE_BASE_ROUTINE, PTRANSLATE_ADDRESS_ROUTINE) {
    walkContext = context;
    auto* native = static_cast<CONTEXT*>(context);
    native->Eax = 0xDEADBEEF;
    native->Ebp = 0x001AFFEC;
    native->Esp = 0x001AFFE4;
    walkAfter = *native;
    return FALSE;
}
LONG WINAPI OriginalExceptionWriter(EXCEPTION_POINTERS* pointers) {
    writerPointers = pointers;
    writerContext = pointers ? pointers->ContextRecord : nullptr;
    STACKFRAME frame{};
    reinterpret_cast<StackWalkFn>(stackWalkHook)(
        IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame,
        pointers->ContextRecord, nullptr, nullptr, nullptr, nullptr);
    writerAfter = *pointers->ContextRecord;
    return 1;
}
void* Symbol(SdgoMod*, const char* key) {
    if (!std::strcmp(key, "grid.data")) return reinterpret_cast<void*>(&Grid);
    if (!std::strcmp(key, "grid.find")) return missingSymbol ? nullptr : reinterpret_cast<void*>(&Find);
    if (!std::strcmp(key, "native_data_guard.funnel_bounds")) return bounds + 6;
    if (!std::strcmp(key, "native_data_guard.name_call_plain")) return code + 8;
    if (!std::strcmp(key, "native_data_guard.name_call_bracketed")) return code + 72;
    if (!std::strcmp(key, "native_data_guard.unit_finalize")) return reinterpret_cast<void*>(&FinalizeUnit);
    if (!std::strcmp(key, "native_data_guard.information_denominator"))
        return reinterpret_cast<void*>(&OriginalInformationSite);
    if (!std::strcmp(key, "native_data_guard.exception_writer"))
        return reinterpret_cast<void*>(&OriginalExceptionWriter);
    return nullptr;
}
int HookCode(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.name_lookup")) {
        lookupHook = detour; *original = reinterpret_cast<void*>(&OriginalName); return 1;
    }
    Check(!std::strcmp(key, "native_data_guard.exception_writer"),
          "only the verified exception writer is additionally hooked");
    exceptionHook = detour; *original = reinterpret_cast<void*>(&OriginalExceptionWriter); return 1;
}
int HookSite(SdgoMod*, const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.unit_transform")) {
        unitHook = detour; *original = reinterpret_cast<void*>(&OriginalUnitSite); return 1;
    }
    Check(!std::strcmp(key, "native_data_guard.information_denominator"),
          "only the verified missing-information read is additionally hooked");
    informationHook = detour; *original = reinterpret_cast<void*>(&OriginalInformationSite); return 1;
}
int HookImport(SdgoMod*, const char* dll, const char* function, void* detour, void** original) {
    Check(!_stricmp(dll, "dbghelp.dll") && !std::strcmp(function, "StackWalk"),
          "only the host StackWalk import is hooked");
    stackWalkHook = detour; *original = reinterpret_cast<void*>(&OriginalStackWalk); return 1;
}
void MakeCaller(uint8_t* target) {
    const uint8_t bytes[] = {0xFF,0x74,0x24,0x08,0xFF,0x74,0x24,0x08,0xE8,0,0,0,0,0xC2,0x08,0};
    std::memcpy(target, bytes, sizeof(bytes));
    const auto relative = reinterpret_cast<uintptr_t>(lookupHook) - reinterpret_cast<uintptr_t>(target + 13);
    Put(target, 9, static_cast<uint32_t>(relative));
}
using Lookup = const uint8_t*(__thiscall*)(void*, uint32_t, uint32_t);
using DirectLookup = const uint8_t*(__fastcall*)(void*, void*, uint32_t, uint32_t);
using IndexCheck = int(__stdcall*)(int);

void Run(SdgoModLoadFn entry, HMODULE dll) {
    code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    bounds = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    Check(code && bounds, "owned executable fixture memory allocated");
    // The observed two-branch OR check followed by empty/valid return stubs.
    const uint8_t branch[] = {
        0x8B,0x54,0x24,0x04,0x85,0xD2,0x7F,0x11,0x83,0xFA,0x06,0x7E,0x0C,
        0x31,0xC0,0xC2,0x04,0x00,0x90,0x90,0x90,0x90,0x90,0x90,0x90,
        0xB8,0x01,0x00,0x00,0x00,0xC2,0x04,0x00};
    std::memcpy(bounds, branch, sizeof(branch));
    const auto indexCheck = reinterpret_cast<IndexCheck>(bounds);
    for (int i : {-100, -1, 0, 7, 100}) Check(indexCheck(i) == 1, "original branch incorrectly admits invalid indices");
    DWORD ignored = 0;
    Check(VirtualProtect(bounds, 4096, PAGE_EXECUTE_READ, &ignored) != 0, "fixture branch begins read-only");
    Put(unit.data(), 0x1363, kNameGroup);
    const wchar_t label[] = L"valid";
    std::memcpy(name.data()+13, label, sizeof(label));
    SdgoApi api{}; api.size = sizeof(api); api.version = SDGO_API_VERSION;
    api.Symbol = Symbol; api.HookCode = HookCode; api.HookSite = HookSite;
    api.HookImport = HookImport; api.Log = Log;
    missingSymbol = true;
    Check(!entry(&api, nullptr) && !lookupHook && bounds[6] == 0x7F,
          "missing verified dependency refuses activation before mutation");
    missingSymbol = false;
    Check(entry(&api, nullptr) && lookupHook && unitHook && informationHook &&
          exceptionHook && stackWalkHook, "production DLL activates all verified guard paths");
    MakeCaller(code); MakeCaller(code + 64);
    Check(VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &ignored) != 0, "native call fixtures become executable read-only");
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    MEMORY_BASIC_INFORMATION memory{}; VirtualQuery(bounds, &memory, sizeof(memory));
    Check(memory.Protect == PAGE_EXECUTE_READ && bounds[6] == 0x7E && bounds[7] == 5 &&
          !std::memcmp(bounds+8, branch+8, sizeof(branch)-8), "only the verified two branch bytes change; protection and neighbors preserved");
    for (int i : {-2147483647-1, -100, -1, 0, 7, 100, 2147483647})
        Check(indexCheck(i) == 0, "production correction rejects signed invalid indices");
    for (int i = 1; i <= 6; ++i) Check(indexCheck(i) == 1, "all six valid mount indices remain native");
    void* self = reinterpret_cast<void*>(0x12345678);
    for (size_t offset : {size_t{0}, size_t{64}}) {
        const auto lookup = reinterpret_cast<Lookup>(code + offset);
        const auto before = nativeLookups;
        Check(lookup(self, kUnit, kIndex) == name.data() && nativeLookups == before+1 &&
              observedSelf == self && observedUnit == kUnit && observedIndex == kIndex,
              "both formatter callers preserve successful record, owner register, arguments and single delegation");
        missingName = true;
        const auto missing = lookup(self, kUnit, kIndex);
        Check(missing && !std::wcscmp(reinterpret_cast<const wchar_t*>(missing+13), L"未命名") && nativeLookups == before+1,
              "missing name yields stable packed text without entering the crashing native lookup");
        missingName = false; missingUnit = true;
        Check(lookup(self, kUnit, kIndex) == missing, "missing unit in the display lookup shares safe text only");
        missingUnit = false;
    }
    missingName = true;
    Check(reinterpret_cast<DirectLookup>(lookupHook)(self, nullptr, kUnit, kIndex) == nullptr,
          "an unrelated native caller retains its original null result");
    missingName = false;
    std::array<uint8_t, 0x40> information{};
    Put(information.data(), 0x39, uint32_t{240});
    InvokeInformationSite(information.data());
    Check(informationDenominator == 240 && informationEax == reinterpret_cast<uintptr_t>(information.data()),
          "present information data retains the original denominator record");
    const auto informationMxcsr = _mm_getcsr(); clobberFp = true;
    SetLastError(0x2345);
    InvokeInformationSite(nullptr); clobberFp = false;
    Check(GetLastError() == 0x2345 && informationDenominator == 0 && informationEax != 0 &&
          _mm_getcsr() == informationMxcsr,
          "missing information takes the native zero-denominator path while preserving thread state");
    std::array<uint8_t, 0x140> metadata{};
    std::array<uint8_t, 12> object{}; Put(object.data(), 4, kUnit);
    InvokeUnitSite(metadata.data(), object.data());
    Put(metadata.data(), 0x13C, uint32_t{1}); InvokeUnitSite(metadata.data(), object.data());
    Check(originalUnitCalls == 2 && finalized == 2, "present metadata follows the original branch and finalization once");
    const auto mxcsr = _mm_getcsr(); clobberFp = true;
    SetLastError(0x1234);
    InvokeUnitSite(nullptr, object.data()); clobberFp = false;
    Check(GetLastError() == 0x1234, "site diagnostics preserve native thread last-error state");
    Check(originalUnitCalls == 2 && finalized == 3, "missing optional metadata skips only unsafe read and still finalizes once");
    Check(afterEax == 0x12345678 && afterEcx == 0x23456789 && afterEdx == 0x3456789A && _mm_getcsr() == mxcsr,
          "site diagnostics preserve volatile native registers and SIMD control state");
    CONTEXT original{};
    original.ContextFlags = CONTEXT_FULL;
    original.Eax = 0x11111111; original.Ebx = 0x22222222;
    original.Ecx = 0x33333333; original.Edx = 0x44444444;
    original.Esi = 0x55555555; original.Edi = 0x66666666;
    original.Ebp = 0x77777777; original.Esp = 0x88888888;
    original.Eip = 0x99999999;
    const CONTEXT originalBefore = original;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void*>(&OriginalExceptionWriter);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1;
    record.ExceptionInformation[1] = 0xDEADC0DE;
    EXCEPTION_POINTERS pointers{&record, &original};
    SetLastError(0x3456);
    Check(reinterpret_cast<ExceptionWriter>(exceptionHook)(&pointers) == 1,
          "wrapped native exception writer retains its original return contract");
    Check(GetLastError() == 0x3456 && !std::memcmp(&original, &originalBefore, sizeof(original)),
          "exception diagnostics preserve caller context and last-error state");
    Check(writerPointers && writerPointers != &pointers && writerContext &&
          writerContext != &original && walkContext && walkContext != writerContext,
          "native writer and StackWalk receive independent owned context copies");
    Check(writerAfter.Eax == originalBefore.Eax && writerAfter.Ebp == originalBefore.Ebp &&
          writerAfter.Esp == originalBefore.Esp && walkAfter.Eax == 0xDEADBEEF &&
          walkAfter.Ebp == 0x001AFFEC && walkAfter.Esp == 0x001AFFE4,
          "StackWalk mutation cannot pollute the registers printed by the native writer");
    bool hasRecord = false, hasRegisters = false, hasModule = false;
    for (const auto& line : logs) {
        hasRecord = hasRecord || (line.find("code=C0000005") != std::string::npos &&
            line.find("access=1") != std::string::npos &&
            line.find("fault=DEADC0DE") != std::string::npos);
        hasRegisters = hasRegisters || (line.find("eax=11111111") != std::string::npos &&
            line.find("ebp=77777777") != std::string::npos &&
            line.find("esp=88888888") != std::string::npos &&
            line.find("eip=99999999") != std::string::npos);
        hasModule = hasModule || (line.find("module=00000000..00000000") == std::string::npos &&
            line.find("module=") != std::string::npos);
    }
    Check(hasRecord && hasRegisters && hasModule,
          "accurate exception, access, module-range and original-register diagnostics are recorded");
    CONTEXT outside{}; outside.Eax = 7;
    STACKFRAME outsideFrame{}; walkContext = nullptr;
    reinterpret_cast<StackWalkFn>(stackWalkHook)(
        IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &outsideFrame,
        &outside, nullptr, nullptr, nullptr, nullptr);
    Check(walkContext == &outside && outside.Eax == 0xDEADBEEF,
          "StackWalk calls outside the native writer retain their original context pointer");
    using StatusFn = int(__cdecl*)(SdgoNativeDataGuardStatusV1*);
    using StatusV2Fn = int(__cdecl*)(SdgoNativeDataGuardStatusV2*);
    const auto status = reinterpret_cast<StatusFn>(GetProcAddress(dll, "SdgoNativeDataGuardGetStatusV1"));
    const auto statusV2 = reinterpret_cast<StatusV2Fn>(
        GetProcAddress(dll, "SdgoNativeDataGuardGetStatusV2"));
    SdgoNativeDataGuardStatusV1 snapshot{}; snapshot.size = sizeof(snapshot);
    Check(status && status(&snapshot) && snapshot.version == 1 && snapshot.funnel_bounds_corrected &&
          snapshot.missing_names == 4 && snapshot.missing_transform_metadata == 1,
          "read-only native observer counts actual guards without changing data");
    Check(!status(nullptr), "observer rejects null output");
    snapshot.size = 4; Check(!status(&snapshot), "observer rejects undersized output");
    SdgoNativeDataGuardStatusV2 snapshotV2{}; snapshotV2.size = sizeof(snapshotV2);
    Check(statusV2 && statusV2(&snapshotV2) && snapshotV2.version == 2 &&
          snapshotV2.funnel_bounds_corrected && snapshotV2.missing_names == 4 &&
          snapshotV2.missing_transform_metadata == 1 &&
          snapshotV2.missing_information_records == 1 &&
          snapshotV2.preserved_exception_contexts == 1,
          "additive V2 observer exposes the new guards without changing V1");
    Check(!statusV2(nullptr), "V2 observer rejects null output");
    snapshotV2.size = 4; Check(!statusV2(&snapshotV2), "V2 observer rejects undersized output");
    Check(gridCalls > 0 && warnings == 6, "warnings are emitted for missing data only");
    VirtualFree(code, 0, MEM_RELEASE); VirtualFree(bounds, 0, MEM_RELEASE);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    try {
        HMODULE dll = LoadLibraryW(argv[1]);
        auto entry = dll ? reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll, "SdgoModLoad")) : nullptr;
        Check(entry != nullptr, "production DLL exports its loader");
        Run(entry, dll); FreeLibrary(dll);
        std::printf("PASS %u production native-data guard checks\n", checks); return 0;
    } catch (const std::exception& error) { std::printf("FAIL %s\n", error.what()); return 1; }
}
