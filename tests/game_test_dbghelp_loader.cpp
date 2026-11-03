// Fixture-only loader for the two native monitor programs, which import
// dbghelp rather than winmm. Reuse the framework's early CRT boot, then forward
// every imported debugging API unchanged to the system DLL. Original EXEs stay
// byte-identical. This is built separately from all distribution stages.
#include "../loader/common/early_boot.h"

namespace {
INIT_ONCE realOnce = INIT_ONCE_STATIC_INIT;
HMODULE realModule = nullptr;
const char* names[] = {"SymCleanup", "SymGetLineFromAddr64", "SymFromAddr",
    "StackWalk", "SymFunctionTableAccess", "SymGetModuleBase", "SymInitialize",
    "SymSetOptions", "EnumerateLoadedModules", "GetTimestampForLoadedLibrary",
    "MiniDumpWriteDump"};
BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    realModule = LoadForwardTarget(L"fixture_chain_dbghelp", L"dbghelp.dll");
    return realModule != nullptr;
}
}

extern "C" FARPROC __cdecl ResolveFixtureDbgHelp(unsigned index) {
    const DWORD previous = GetLastError();
    EarlyBoot();
    InitOnceExecuteOnce(&realOnce, &LoadReal, nullptr, nullptr);
    FARPROC result = realModule && index < sizeof(names) / sizeof(names[0]) ?
        GetProcAddress(realModule, names[index]) : nullptr;
    if (!result) ExitProcess(ERROR_PROC_NOT_FOUND);
    SetLastError(previous);
    return result;
}

#define FORWARD(name, index) \
extern "C" __declspec(naked) void __cdecl Fixture_##name() { \
    __asm { pushfd } \
    __asm { pushad } \
    __asm { push index } \
    __asm { call ResolveFixtureDbgHelp } \
    __asm { add esp, 4 } \
    __asm { mov [esp+28], eax } \
    __asm { popad } \
    __asm { popfd } \
    __asm { jmp eax } \
}
FORWARD(SymCleanup, 0)
FORWARD(SymGetLineFromAddr64, 1)
FORWARD(SymFromAddr, 2)
FORWARD(StackWalk, 3)
FORWARD(SymFunctionTableAccess, 4)
FORWARD(SymGetModuleBase, 5)
FORWARD(SymInitialize, 6)
FORWARD(SymSetOptions, 7)
FORWARD(EnumerateLoadedModules, 8)
FORWARD(GetTimestampForLoadedLibrary, 9)
FORWARD(MiniDumpWriteDump, 10)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        EarlyBootInstall("game_test_dbghelp_fixture");
    }
    return TRUE;
}
