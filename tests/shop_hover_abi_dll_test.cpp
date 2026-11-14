// Execute the production shop-localization DLL through both native hover hooks.
// The supported client calls each hook as thiscall(owner, argument, options)
// and the callee removes both stack arguments.
#include <windows.h>

#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "sdgo/sdgo_api.h"
#include "sdgo/shop_labels.h"

namespace {

static_assert(sizeof(void*) == 4, "shop hover ABI fixture requires the x86 build");

using HoverHook = void(__fastcall*)(void*, void*, std::uint32_t, void*);

constexpr std::uint32_t kEbxSeed = 0x31415926u;
constexpr std::uint32_t kEsiSeed = 0x27182818u;
constexpr std::uint32_t kEdiSeed = 0x16180339u;
constexpr std::uint32_t kObserverEcxClobber = 0xA9194BB5u;
constexpr DWORD kOriginalLastError = 0x000006BAu;

unsigned g_checks = 0;
std::wstring g_modDirectory;
HoverHook g_hover = nullptr;
HoverHook g_hoverAlt = nullptr;
unsigned g_hoverHooks = 0;
unsigned g_hookCount = 0;

void* g_expectedOwner = nullptr;
void* g_expectedOptions = nullptr;
std::uint32_t g_expectedArgument = 0;
void* g_seenOwner = nullptr;
void* g_seenOptions = nullptr;
std::uint32_t g_seenArgument = 0;
unsigned g_originalCalls = 0;

std::uintptr_t g_stackBefore = 0;
std::uintptr_t g_stackAfter = 0;
std::uint32_t g_afterEbx = 0;
std::uint32_t g_afterEsi = 0;
std::uint32_t g_afterEdi = 0;
std::uint32_t g_beforeEbp = 0;
std::uint32_t g_afterEbp = 0;
std::uint16_t g_controlBefore = 0;
std::uint16_t g_controlAfter = 0;
std::uint32_t g_mxcsrBefore = 0;
std::uint32_t g_mxcsrAfter = 0;
DWORD g_afterLastError = ERROR_SUCCESS;
volatile LONG g_observerFaults = 0;

void Check(bool value, const char* message) {
    ++g_checks;
    if (!value) throw std::runtime_error(message);
    std::printf("PASS %s\n", message);
}

void __cdecl GenericOriginal() {}

void __fastcall NativeHover(
    void* owner, void*, std::uint32_t argument, void* options) {
    ++g_originalCalls;
    g_seenOwner = owner;
    g_seenArgument = argument;
    g_seenOptions = options;
    SetLastError(kOriginalLastError);
}

// Call the captured detour exactly as the supported executable does: owner in
// ECX, two stack arguments, and an otherwise volatile EDX. Record the stack,
// nonvolatile integer registers and FP control state immediately after return.
__declspec(naked) void __stdcall InvokeHover(
    void* /*detour*/, void* /*owner*/, std::uint32_t /*argument*/,
    void* /*options*/) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov ebx, 31415926h
        mov esi, 27182818h
        mov edi, 16180339h
        mov eax, ebp
        mov dword ptr [g_beforeEbp], eax
        fnstcw word ptr [g_controlBefore]
        stmxcsr dword ptr [g_mxcsrBefore]
        mov eax, esp
        mov dword ptr [g_stackBefore], eax
        push dword ptr [ebp+14h]
        push dword ptr [ebp+10h]
        mov ecx, dword ptr [ebp+0Ch]
        mov edx, 5A17C0DEh
        call dword ptr [ebp+08h]
        mov dword ptr [g_stackAfter], esp
        mov dword ptr [g_afterEbx], ebx
        mov dword ptr [g_afterEsi], esi
        mov dword ptr [g_afterEdi], edi
        mov eax, ebp
        mov dword ptr [g_afterEbp], eax
        fnstcw word ptr [g_controlAfter]
        stmxcsr dword ptr [g_mxcsrAfter]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret 10h
    }
}

LONG CALLBACK ObserverFaultHandler(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* record = pointers->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        record->NumberParameters < 2 || record->ExceptionInformation[1] >= 0x10000u)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_observerFaults);
    // ECX is volatile across the observation. The repaired hook must reload the
    // native owner explicitly before entering its original thiscall target.
    pointers->ContextRecord->Ecx = kObserverEcxClobber;
    return EXCEPTION_CONTINUE_SEARCH;
}

void* Symbol(SdgoMod*, const char*) {
    // The focused fixture never invokes the other native symbols. Non-null
    // verified addresses are sufficient to let the actual DLL install the same
    // hook set used by a custom-product display profile.
    return reinterpret_cast<void*>(&GenericOriginal);
}

int HookCode(
    SdgoMod*, const char* name, void* detour, void** original) {
    if (!name || !detour || !original) return 0;
    if (std::strcmp(name, "shop_localization.hover") == 0) {
        g_hover = reinterpret_cast<HoverHook>(detour);
        *original = reinterpret_cast<void*>(&NativeHover);
        ++g_hoverHooks;
    } else if (std::strcmp(name, "shop_localization.hover_alt") == 0) {
        g_hoverAlt = reinterpret_cast<HoverHook>(detour);
        *original = reinterpret_cast<void*>(&NativeHover);
        ++g_hoverHooks;
    } else {
        *original = reinterpret_cast<void*>(&GenericOriginal);
    }
    ++g_hookCount;
    return 1;
}

int SettingInt(SdgoMod*, const char*, int fallback) {
    return fallback;
}

const wchar_t* ModDirectory(SdgoMod*) {
    return g_modDirectory.c_str();
}

void __cdecl Log(SdgoMod*, int, const char*, ...) {}

void WriteFixtureFiles(const std::filesystem::path& root) {
    const auto localization = root / L"mods" / L"shop_localization";
    const auto labels = root / L"mods" / L"shop_labels";
    std::filesystem::create_directories(localization);
    std::filesystem::create_directories(labels);
    g_modDirectory = localization.wstring();

    std::ofstream dictionary(localization / L"zh-CN.ini", std::ios::binary);
    dictionary << "[entry:unit]\nKind=name\nSource=GUNDAM\n"
                  "Text=\xE9\xAB\x98\xE8\xBE\xBE\n";
    dictionary.close();

    const std::wstring profile =
        L"\xFEFF[shop]\r\nRevision=hover-abi-fixture\r\n"
        L"[product:1039001]\r\nName=\x7F8E\x7AF9\x5170\x526F\x5B98\r\n"
        L"TemplateId=1030001\r\nCategory=1219047\r\nPrice=100\r\n"
        L"Payment=1\r\nValidity=1\r\n";
    std::ofstream display(labels / L"display.ini", std::ios::binary);
    display.write(
        reinterpret_cast<const char*>(profile.data()),
        static_cast<std::streamsize>(profile.size() * sizeof(wchar_t)));
}

void ResetInvocation(void* owner, std::uint32_t argument, void* options) {
    g_expectedOwner = owner;
    g_expectedArgument = argument;
    g_expectedOptions = options;
    g_seenOwner = nullptr;
    g_seenArgument = 0;
    g_seenOptions = nullptr;
    g_originalCalls = 0;
    g_stackBefore = 0;
    g_stackAfter = 0;
    g_afterEbx = 0;
    g_afterEsi = 0;
    g_afterEdi = 0;
    g_beforeEbp = 0;
    g_afterEbp = 0;
    g_controlBefore = 0;
    g_controlAfter = 0;
    g_mxcsrBefore = 0;
    g_mxcsrAfter = 0;
    g_afterLastError = ERROR_SUCCESS;
}

void VerifyInvocation(const char* path) {
    Check(g_originalCalls == 1, (std::string(path) + " original executes once").c_str());
    Check(
        g_seenOwner == g_expectedOwner,
        (std::string(path) + " restores the native owner in ECX").c_str());
    Check(
        g_seenArgument == g_expectedArgument &&
            g_seenOptions == g_expectedOptions,
        (std::string(path) + " forwards both native stack arguments").c_str());
    Check(
        g_stackAfter == g_stackBefore,
        (std::string(path) + " removes exactly eight argument bytes").c_str());
    Check(
        g_afterEbx == kEbxSeed && g_afterEsi == kEsiSeed &&
            g_afterEdi == kEdiSeed && g_afterEbp == g_beforeEbp,
        (std::string(path) + " preserves x86 nonvolatile registers").c_str());
    Check(
        g_controlAfter == g_controlBefore && g_mxcsrAfter == g_mxcsrBefore,
        (std::string(path) + " preserves x87 and SSE control state").c_str());
    Check(
        g_afterLastError == kOriginalLastError,
        (std::string(path) + " preserves the original native LastError result").c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path root = argv[2];
    try {
        WriteFixtureFiles(root);

        const HMODULE dll = LoadLibraryW(argv[1]);
        const auto load = dll ? reinterpret_cast<SdgoModLoadFn>(
                                    GetProcAddress(dll, "SdgoModLoad"))
                              : nullptr;
        Check(load != nullptr, "actual shop localization DLL export loaded");

        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Log = &Log;
        api.Symbol = &Symbol;
        api.HookCode = &HookCode;
        api.SettingInt = &SettingInt;
        api.ModDirectory = &ModDirectory;
        Check(load(&api, nullptr) != 0, "actual DLL initializes with custom product display enabled");
        Check(
            g_hoverHooks == 2 && g_hover && g_hoverAlt,
            "actual DLL installs both native product hover hooks");
        Check(g_hookCount == 16, "custom display profile installs the complete production hook set");

        std::array<std::uint8_t, 0x80> firstOwner{};
        sdgo::shop::Option option{};
        option.productId = 1039001;
        option.category = 1219047;
        option.price = 100;
        option.payment = 1;
        option.validity = 1;
        std::array<std::uintptr_t, 2> options{
            reinterpret_cast<std::uintptr_t>(&option),
            reinterpret_cast<std::uintptr_t>(&option + 1)};
        ResetInvocation(firstOwner.data(), 0x13572468u, options.data());
        SetLastError(ERROR_SUCCESS);
        InvokeHover(
            reinterpret_cast<void*>(g_hover), firstOwner.data(),
            g_expectedArgument, options.data());
        g_afterLastError = GetLastError();
        VerifyInvocation("primary hover");

        std::array<std::uint8_t, 0x80> secondOwner{};
        auto* const faultingOptions = reinterpret_cast<void*>(1u);
        ResetInvocation(secondOwner.data(), 0x24681357u, faultingOptions);
        g_observerFaults = 0;
        void* const handler = AddVectoredExceptionHandler(1, &ObserverFaultHandler);
        Check(handler != nullptr, "observer fault instrumentation installed");
        SetLastError(ERROR_SUCCESS);
        InvokeHover(
            reinterpret_cast<void*>(g_hoverAlt), secondOwner.data(),
            g_expectedArgument, faultingOptions);
        g_afterLastError = GetLastError();
        Check(
            RemoveVectoredExceptionHandler(handler) != 0,
            "observer fault instrumentation removed");
        Check(
            g_observerFaults == 1,
            "alternate hover contains the invalid options observation fault");
        VerifyInvocation("alternate hover after observer ECX clobber");

        Check(FreeLibrary(dll) != 0, "actual shop localization DLL unloaded");
        std::printf("SHOP HOVER ACTUAL DLL ABI PASS %u checks\n", g_checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
