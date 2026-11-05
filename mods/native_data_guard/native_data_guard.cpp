// Guards verified native data reads and preserves the original exception
// context without changing valid battle data.
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/native_data_guard_observer.h"

namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
using GridData = void*(__cdecl*)();
using GridFind = bool(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
using NameLookup = const uint8_t*(__thiscall*)(void*, uint32_t, uint32_t);
using ExceptionWriter = LONG(WINAPI*)(EXCEPTION_POINTERS*);
using InformationRefresh = void(__thiscall*)(void*);
using NativeMessageClone = void*(__thiscall*)(void*);
using NativeMessageDestroy = void*(__thiscall*)(void*, uint8_t);
using UnitIndexingCopy = void*(__thiscall*)(void*, const void*);
using UnitIndexingDestroy = void*(__thiscall*)(void*, uint8_t);
using StackWalkFn = BOOL(WINAPI*)(
    DWORD, HANDLE, HANDLE, LPSTACKFRAME, LPVOID,
    PREAD_PROCESS_MEMORY_ROUTINE, PFUNCTION_TABLE_ACCESS_ROUTINE,
    PGET_MODULE_BASE_ROUTINE, PTRANSLATE_ADDRESS_ROUTINE);
GridData g_grid = nullptr;
GridFind g_find = nullptr;
NameLookup g_name = nullptr;
ExceptionWriter g_exceptionWriter = nullptr;
InformationRefresh g_informationRefresh = nullptr;
UnitIndexingCopy g_unitIndexingCopy = nullptr;
UnitIndexingDestroy g_unitIndexingDestroy = nullptr;
StackWalkFn g_stackWalk = nullptr;
void* g_unitContinue = nullptr;
void* g_unitSkip = nullptr;
void* g_informationContinue = nullptr;
void* g_deferredContinue = nullptr;
void* g_deferredTrampoline = nullptr;
void* g_completionTrampoline = nullptr;
std::array<uintptr_t, 2> g_nameCallers{};
volatile LONG g_missingNames = 0, g_missingTransforms = 0, g_missingInformation = 0;
volatile LONG g_preservedExceptions = 0;
volatile LONG g_missingBaseInformation = 0;
volatile LONG g_deferredMessageCopies = 0, g_deferredMessageReplacements = 0;
volatile LONG g_deferredMessageCopyFailures = 0;
volatile LONG g_deferredMessageCompletions = 0, g_outstandingDeferredMessages = 0;
bool g_boundsCorrected = false;
const DWORD g_defaultMxcsr = 0x1F80;
const uint8_t g_missingInformationRecord[0x40]{};

struct ExceptionFrame {
    CONTEXT original{};
    CONTEXT walk{};
    EXCEPTION_RECORD record{};
    EXCEPTION_POINTERS pointers{};
    ExceptionFrame* previous = nullptr;
};
thread_local ExceptionFrame* g_exceptionFrame = nullptr;

#pragma pack(push, 1)
struct NameRecord {
    uint8_t header[13];
    wchar_t text[4];
};
#pragma pack(pop)
static_assert(offsetof(NameRecord, text) == 13);
const NameRecord g_unnamed = {{}, L"未命名"};

uint32_t Read32(const uint8_t* bytes, size_t offset) {
    uint32_t value;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

bool HasName(uint32_t unit, uint32_t index) {
    const uint8_t* record = nullptr;
    void* grid = g_grid();
    if (!grid || !g_find(grid, &record, unit, 2, 0) || !record) return false;
    // The shipped lookup derives its type-24 name key from this packed field.
    const uint32_t key = (Read32(record, 0x1363) + 0x63Fu) * 100u + index;
    record = nullptr;
    return g_find(grid, &record, key, 24, 0) && record;
}

const uint8_t* __fastcall FindName(void* self, void*, uint32_t unit, uint32_t index) {
    const DWORD error = GetLastError();
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if ((caller == g_nameCallers[0] || caller == g_nameCallers[1]) && !HasName(unit, index)) {
        if (InterlockedIncrement(&g_missingNames) <= 16)
            g_api->Log(g_mod, SDGO_LOG_WARN,
                "name data unavailable for unit %lu index %lu; display uses an unnamed label",
                static_cast<unsigned long>(unit), static_cast<unsigned long>(index));
        // Only the formatter's two verified callers read this record's text.
        // No native table, model, weapon, identifier or owned record is replaced.
        SetLastError(error);
        return reinterpret_cast<const uint8_t*>(&g_unnamed);
    }
    SetLastError(error);
    return g_name(self, unit, index);
}

void __stdcall MissingTransform(const uint8_t* self) {
    const DWORD error = GetLastError();
    if (InterlockedIncrement(&g_missingTransforms) <= 16)
        g_api->Log(g_mod, SDGO_LOG_WARN,
            "unit %lu has no optional transform metadata; base-model finalization retained",
            static_cast<unsigned long>(Read32(self, 4)));
    SetLastError(error);
}

void __stdcall MissingInformation() {
    const DWORD error = GetLastError();
    if (InterlockedIncrement(&g_missingInformation) <= 16)
        g_api->Log(g_mod, SDGO_LOG_WARN,
            "derived information data is unavailable; the native default percentage is retained");
    SetLastError(error);
}

// The whole-page refresh uses a different type-2 key from the derived
// percentage lookup. Its native error branch logs a miss but then passes null
// to the detail readers (or reads null+0x13C in transform mode). Leave this
// refresh untouched until the real base record is available. Never fabricate
// a gameplay record, change the selected unit, or swallow a native exception.
void __fastcall RefreshInformation(void* self, void*) {
    const DWORD error = GetLastError();
    const auto unit = Read32(static_cast<const uint8_t*>(self), 0x114);
    const uint8_t* record = nullptr;
    void* grid = g_grid();
    if (!grid || !g_find(grid, &record, unit, 2, 0) || !record) {
        if (InterlockedIncrement(&g_missingBaseInformation) <= 16)
            g_api->Log(g_mod, SDGO_LOG_WARN,
                "unit %lu base information unavailable; waiting for a later native detail refresh",
                static_cast<unsigned long>(unit));
        SetLastError(error);
        return;
    }
    SetLastError(error);
    g_informationRefresh(self);
}

void* CloneNativeMessage(void* message) {
    if (!message) return nullptr;
    auto** table = *static_cast<void***>(message);
    auto clone = reinterpret_cast<NativeMessageClone>(table[1]);
    return clone(message);
}

void DestroyNativeMessage(void* message) {
    if (!message) return;
    auto** table = *static_cast<void***>(message);
    auto destroy = reinterpret_cast<NativeMessageDestroy>(table[0]);
    destroy(message, 1);
}

void LogDeferredCopyFailure() {
    if (InterlockedIncrement(&g_deferredMessageCopyFailures) <= 16)
        g_api->Log(g_mod, SDGO_LOG_WARN,
            "unit-management request could not be copied; reopen the page after loading completes");
}

void __stdcall OwnDeferredUnitIndexingMessage(void* service, void* incoming) {
    const DWORD error = GetLastError();
    void* copy = CloneNativeMessage(incoming);
    if (!copy) {
        LogDeferredCopyFailure();
        SetLastError(error);
        return;
    }
    auto** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(service) + 0x58);
    void* replaced = *slot;
    *slot = copy;
    InterlockedIncrement(&g_deferredMessageCopies);
    if (replaced) {
        InterlockedIncrement(&g_deferredMessageReplacements);
        DestroyNativeMessage(replaced);
    } else {
        InterlockedIncrement(&g_outstandingDeferredMessages);
    }
    SetLastError(error);
}

// Recv_Msg_BeginUnitIndexing used to retain the dispatcher-owned incoming
// message directly. The completion path later calls its clone virtual and
// deletes it, after the dispatcher may already have reclaimed it. Preserve the
// original register/flag/FPU state while installing an independently owned
// native clone in the service's existing deferred-message slot.
__declspec(naked) void UnitIndexingDeferredSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { mov eax, [ebp-160h] }
    __asm { mov ecx, [ebp+8] }
    __asm { push ecx }
    __asm { push eax }
    __asm { call OwnDeferredUnitIndexingMessage }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { mov ecx, [ebp+8] }
    __asm { mov edx, [ebp-160h] }
    __asm { jmp dword ptr [g_deferredContinue] }
}

void __stdcall ObserveUnitIndexingCompletion(void* service) {
    const DWORD error = GetLastError();
    if (*reinterpret_cast<void**>(static_cast<uint8_t*>(service) + 0x58)) {
        InterlockedIncrement(&g_deferredMessageCompletions);
        InterlockedDecrement(&g_outstandingDeferredMessages);
    }
    SetLastError(error);
}

// Count the genuine native completion immediately before its unchanged
// virtual destruction and slot clear. This is read-only acceptance evidence;
// the trampoline still performs the original release.
__declspec(naked) void UnitIndexingCompletionSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push esi }
    __asm { call ObserveUnitIndexingCompletion }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_completionTrampoline] }
}

void* __fastcall CopyUnitIndexingService(void* self, void*, const void* source) {
    void* result = g_unitIndexingCopy(self, source);
    const DWORD error = GetLastError();
    auto** destination =
        reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x58);
    void* pending =
        *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(source) + 0x58);
    // The native copy constructor copied this pointer shallowly. Never release
    // that borrowed value from the destination; replace it with a deep copy.
    *destination = nullptr;
    if (pending) {
        void* copy = CloneNativeMessage(pending);
        if (copy) {
            *destination = copy;
            InterlockedIncrement(&g_deferredMessageCopies);
            InterlockedIncrement(&g_outstandingDeferredMessages);
        } else {
            LogDeferredCopyFailure();
        }
    }
    SetLastError(error);
    return result;
}

void* __fastcall DestroyUnitIndexingService(void* self, void*, uint8_t deleting) {
    const DWORD error = GetLastError();
    auto** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x58);
    void* pending = *slot;
    *slot = nullptr;
    if (pending) {
        InterlockedDecrement(&g_outstandingDeferredMessages);
        DestroyNativeMessage(pending);
    }
    SetLastError(error);
    return g_unitIndexingDestroy(self, deleting);
}

// At this site the primary model has already been loaded. EBX is the result
// of the subsequent optional transform-data lookup. A miss skips only that
// optional block, then runs the same finalizers and native completion state.
__declspec(naked) void UnitTransformSite() {
    __asm { pushfd }
    __asm { test ebx, ebx }
    __asm { jnz present }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push esi }
    __asm { call MissingTransform }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_unitSkip] }
present:
    __asm { popfd }
    __asm { jmp dword ptr [g_unitContinue] }
}

// The original information refresh reads [EAX+0x39] immediately after a
// lookup which may legally return null. Its next block already treats a zero
// denominator as the native default 100 percent. Supply a zeroed, private
// read-only-shaped record only to the displaced MOV so the original block and
// all later arithmetic remain unchanged.
__declspec(naked) void InformationReadSite() {
    __asm { test eax, eax }
    __asm { jnz present }
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 210h }
    __asm { and esp, 0FFFFFFF0h }
    __asm { mov [esp+200h], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { call MissingInformation }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { lea eax, g_missingInformationRecord }
present:
    __asm { jmp dword ptr [g_informationContinue] }
}

bool ModuleRange(const void* address, uintptr_t& base, uintptr_t& end) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !VirtualQuery(address, &memory, sizeof(memory)) || !memory.AllocationBase)
        return false;
    base = reinterpret_cast<uintptr_t>(memory.AllocationBase);
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.SizeOfImage) return false;
        end = base + nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        base = end = 0;
        return false;
    }
}

void LogOriginalException(const ExceptionFrame& frame) {
    const auto& record = frame.record;
    const auto& context = frame.original;
    uintptr_t moduleBase = 0, moduleEnd = 0;
    ModuleRange(record.ExceptionAddress, moduleBase, moduleEnd);
    ULONG_PTR access = static_cast<ULONG_PTR>(-1), fault = 0;
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        access = record.ExceptionInformation[0];
        fault = record.ExceptionInformation[1];
    }
    g_api->Log(g_mod, SDGO_LOG_ERROR,
        "native exception original code=%08lX address=%08lX access=%lu fault=%08lX module=%08lX..%08lX",
        static_cast<unsigned long>(record.ExceptionCode),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(record.ExceptionAddress)),
        static_cast<unsigned long>(access),
        static_cast<unsigned long>(fault),
        static_cast<unsigned long>(moduleBase),
        static_cast<unsigned long>(moduleEnd));
    g_api->Log(g_mod, SDGO_LOG_ERROR,
        "native exception original registers eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX "
        "esi=%08lX edi=%08lX ebp=%08lX esp=%08lX eip=%08lX",
        static_cast<unsigned long>(context.Eax), static_cast<unsigned long>(context.Ebx),
        static_cast<unsigned long>(context.Ecx), static_cast<unsigned long>(context.Edx),
        static_cast<unsigned long>(context.Esi), static_cast<unsigned long>(context.Edi),
        static_cast<unsigned long>(context.Ebp), static_cast<unsigned long>(context.Esp),
        static_cast<unsigned long>(context.Eip));
}

BOOL WINAPI PreservedStackWalk(
    DWORD machine, HANDLE process, HANDLE thread, LPSTACKFRAME frame, LPVOID context,
    PREAD_PROCESS_MEMORY_ROUTINE readMemory,
    PFUNCTION_TABLE_ACCESS_ROUTINE functionTable,
    PGET_MODULE_BASE_ROUTINE moduleBase,
    PTRANSLATE_ADDRESS_ROUTINE translate) {
    ExceptionFrame* active = g_exceptionFrame;
    if (active && context == &active->original) context = &active->walk;
    return g_stackWalk(machine, process, thread, frame, context,
                       readMemory, functionTable, moduleBase, translate);
}

LONG WINAPI PreserveExceptionContext(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
        return g_exceptionWriter(pointers);
    const DWORD error = GetLastError();
    ExceptionFrame frame{};
    frame.original = *pointers->ContextRecord;
    frame.walk = frame.original;
    frame.record = *pointers->ExceptionRecord;
    frame.pointers.ExceptionRecord = &frame.record;
    frame.pointers.ContextRecord = &frame.original;
    frame.previous = g_exceptionFrame;
    g_exceptionFrame = &frame;
    InterlockedIncrement(&g_preservedExceptions);
    LogOriginalException(frame);
    SetLastError(error);
    const LONG result = g_exceptionWriter(&frame.pointers);
    g_exceptionFrame = frame.previous;
    return result;
}

bool CorrectBounds(uint8_t* site) {
    constexpr uint8_t before[] = {0x7F,0x11,0x83,0xFA,0x06,0x7E,0x0C};
    constexpr uint8_t after[] = {0x7E,0x05,0x83,0xFA,0x06,0x7E,0x0C};
    if (!std::memcmp(site, after, sizeof(after))) return true;
    if (std::memcmp(site, before, sizeof(before))) return false;
    DWORD protection = 0;
    if (!VirtualProtect(site, 2, PAGE_EXECUTE_READWRITE, &protection)) return false;
    std::memcpy(site, after, 2);
    FlushInstructionCache(GetCurrentProcess(), site, 2);
    DWORD ignored = 0;
    if (!VirtualProtect(site, 2, protection, &ignored)) {
        std::memcpy(site, before, 2);
        FlushInstructionCache(GetCurrentProcess(), site, 2);
        VirtualProtect(site, 2, protection, &ignored);
        return false;
    }
    return true;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoNativeDataGuardGetStatusV1(
    SdgoNativeDataGuardStatusV1* out) {
    if (!out || out->size < sizeof(*out)) return 0;
    out->version = 1;
    out->funnel_bounds_corrected = g_boundsCorrected ? 1u : 0u;
    out->missing_names = static_cast<uint32_t>(InterlockedCompareExchange(&g_missingNames, 0, 0));
    out->missing_transform_metadata = static_cast<uint32_t>(InterlockedCompareExchange(&g_missingTransforms, 0, 0));
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoNativeDataGuardGetStatusV2(
    SdgoNativeDataGuardStatusV2* out) {
    if (!out || out->size < sizeof(*out)) return 0;
    out->version = 2;
    out->funnel_bounds_corrected = g_boundsCorrected ? 1u : 0u;
    out->missing_names = static_cast<uint32_t>(InterlockedCompareExchange(&g_missingNames, 0, 0));
    out->missing_transform_metadata =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_missingTransforms, 0, 0));
    out->missing_information_records =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_missingInformation, 0, 0));
    out->preserved_exception_contexts =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_preservedExceptions, 0, 0));
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoNativeDataGuardGetStatusV3(
    SdgoNativeDataGuardStatusV3* out) {
    if (!out || out->size < sizeof(*out)) return 0;
    out->version = 3;
    out->funnel_bounds_corrected = g_boundsCorrected ? 1u : 0u;
    out->missing_names = static_cast<uint32_t>(InterlockedCompareExchange(&g_missingNames, 0, 0));
    out->missing_transform_metadata =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_missingTransforms, 0, 0));
    out->missing_information_records =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_missingInformation, 0, 0));
    out->preserved_exception_contexts =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_preservedExceptions, 0, 0));
    out->deferred_message_copies =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_deferredMessageCopies, 0, 0));
    out->deferred_message_replacements =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_deferredMessageReplacements, 0, 0));
    out->deferred_message_copy_failures =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_deferredMessageCopyFailures, 0, 0));
    out->deferred_message_completions =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_deferredMessageCompletions, 0, 0));
    out->outstanding_deferred_messages =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_outstandingDeferredMessages, 0, 0));
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl SdgoNativeDataGuardGetStatusV4(
    SdgoNativeDataGuardStatusV4* out) {
    if (!out || out->size < sizeof(*out)) return 0;
    const uint32_t size = out->size;
    SdgoNativeDataGuardStatusV3 previous{};
    previous.size = sizeof(previous);
    SdgoNativeDataGuardGetStatusV3(&previous);
    std::memcpy(out, &previous, sizeof(previous));
    out->size = size;
    out->version = 4;
    out->missing_base_information_records =
        static_cast<uint32_t>(InterlockedCompareExchange(&g_missingBaseInformation, 0, 0));
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->size < offsetof(SdgoApi, HookImport) + sizeof(api->HookImport) ||
        !api->Symbol || !api->HookCode || !api->HookSite || !api->HookImport || !api->Log) return 0;
    g_api = api; g_mod = mod;
    auto symbol = [&](const char* name) { return api->Symbol(mod, name); };
    g_grid = reinterpret_cast<GridData>(symbol("grid.data"));
    g_find = reinterpret_cast<GridFind>(symbol("grid.find"));
    g_unitSkip = symbol("native_data_guard.unit_finalize");
    auto* information = static_cast<uint8_t*>(symbol("native_data_guard.information_denominator"));
    auto* bounds = static_cast<uint8_t*>(symbol("native_data_guard.funnel_bounds"));
    g_deferredContinue = symbol("native_data_guard.unit_indexing_defer_continue");
    for (size_t i = 0; i < g_nameCallers.size(); ++i) {
        const auto address = reinterpret_cast<uintptr_t>(symbol(i ?
            "native_data_guard.name_call_bracketed" : "native_data_guard.name_call_plain"));
        if (!address) return 0;
        g_nameCallers[i] = address + 5;
    }
    if (!g_grid || !g_find || !g_unitSkip || !information || !bounds || !g_deferredContinue ||
        !symbol("native_data_guard.exception_writer") ||
        !symbol("native_data_guard.information_refresh") ||
        !symbol("native_data_guard.unit_indexing_copy") ||
        !symbol("native_data_guard.unit_indexing_destructor")) return 0;
    if (!api->HookCode(mod, "native_data_guard.name_lookup", reinterpret_cast<void*>(&FindName),
                       reinterpret_cast<void**>(&g_name)) ||
        !api->HookCode(mod, "native_data_guard.information_refresh",
                       reinterpret_cast<void*>(&RefreshInformation),
                       reinterpret_cast<void**>(&g_informationRefresh)) ||
        !api->HookSite(mod, "native_data_guard.unit_transform", reinterpret_cast<void*>(&UnitTransformSite),
                       &g_unitContinue) ||
        !api->HookSite(mod, "native_data_guard.information_denominator",
                       reinterpret_cast<void*>(&InformationReadSite), &g_informationContinue) ||
        !api->HookSite(mod, "native_data_guard.unit_indexing_defer",
                       reinterpret_cast<void*>(&UnitIndexingDeferredSite), &g_deferredTrampoline) ||
        !api->HookSite(mod, "native_data_guard.unit_indexing_complete",
                       reinterpret_cast<void*>(&UnitIndexingCompletionSite), &g_completionTrampoline) ||
        !api->HookCode(mod, "native_data_guard.unit_indexing_copy",
                       reinterpret_cast<void*>(&CopyUnitIndexingService),
                       reinterpret_cast<void**>(&g_unitIndexingCopy)) ||
        !api->HookCode(mod, "native_data_guard.unit_indexing_destructor",
                       reinterpret_cast<void*>(&DestroyUnitIndexingService),
                       reinterpret_cast<void**>(&g_unitIndexingDestroy)) ||
        !api->HookCode(mod, "native_data_guard.exception_writer",
                       reinterpret_cast<void*>(&PreserveExceptionContext),
                       reinterpret_cast<void**>(&g_exceptionWriter)) ||
        !api->HookImport(mod, "DBGHELP.dll", "StackWalk",
                         reinterpret_cast<void*>(&PreservedStackWalk),
                         reinterpret_cast<void**>(&g_stackWalk))) return 0;
    if (!CorrectBounds(bounds)) return 0;
    g_boundsCorrected = true;
    api->Log(mod, SDGO_LOG_INFO,
        "native data guards active: bounded reads, owned deferred messages, and original exception contexts");
    return 1;
}
