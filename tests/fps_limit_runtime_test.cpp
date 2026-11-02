// Real core + production FPS Mod + exact native forced-60 instruction fixture.
// This never starts the game or reads a real player's config.xml.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/sdgo_api.h"

namespace {
    constexpr size_t kAppBytes = 0x100;
    constexpr size_t kFpsOffset = 0x80;
    constexpr BYTE kNativeSite[] = {
        0xC7, 0x87, 0x80, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00, // mov dword ptr [edi+80h],60
        0xC3                                                              // ret
    };
    constexpr DWORD kExpectedError = 0x5A160F5;

    __declspec(align(16)) BYTE g_code[32];
    __declspec(align(16)) BYTE g_beforeFx[512], g_afterFx[512];
    __declspec(align(16)) BYTE g_xmm[8][16];
    BYTE g_beforeApp[kAppBytes];
    BYTE g_app[kAppBytes];
    void* g_site = g_code;
    DWORD g_beforeFlags = 0, g_afterFlags = 0;
    DWORD g_afterEax = 0, g_afterEbx = 0, g_afterEcx = 0, g_afterEdx = 0;
    DWORD g_afterEsi = 0, g_afterEdi = 0, g_afterEbp = 0, g_expectedEbp = 0;
    int g_failures = 0;
    int g_expected = 60;

    void Check(bool value, const char* message)
    {
        printf("%s %s\n", value ? "ok  " : "FAIL", message);
        if (!value) ++g_failures;
    }

    std::wstring Root()
    {
        wchar_t path[32768] = {};
        GetModuleFileNameW(nullptr, path, 32768);
        std::wstring result = path;
        return result.substr(0, result.find_last_of(L"\\/"));
    }

    void Write(const std::wstring& path, const std::string& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        Check(file != INVALID_HANDLE_VALUE &&
              WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size(), "write isolated fixture data");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    void __stdcall Inspect()
    {
        const DWORD error = GetLastError();
        int observed = 0;
        memcpy(&observed, g_app + kFpsOffset, sizeof(observed));
        Check(observed == g_expected, "configured cap survives the native forced-60 site");

        bool guards = true;
        for (size_t i = 0; i < kAppBytes; ++i) {
            if (i >= kFpsOffset && i < kFpsOffset + sizeof(int)) continue;
            if (g_app[i] != g_beforeApp[i]) guards = false;
        }
        Check(guards, "only CApplication updatefps is changed");
        Check(g_afterEax == 0x11111111 && g_afterEbx == 0x22222222 &&
              g_afterEcx == 0x33333333 && g_afterEdx == 0x44444444 &&
              g_afterEsi == 0x66666666 &&
              g_afterEdi == reinterpret_cast<uintptr_t>(g_app) &&
              g_afterEbp == g_expectedEbp,
              "site detour preserves general registers");
        Check((g_afterFlags & 0xCD5) == (g_beforeFlags & 0xCD5),
              "site detour preserves arithmetic, direction and carry flags");
        Check(!memcmp(g_beforeFx, g_afterFx, sizeof(g_beforeFx)),
              "site detour preserves x87, MXCSR and all SIMD state");
        Check(error == kExpectedError, "site detour preserves Win32 last-error state");
    }

    __declspec(naked) void __cdecl Invoke(BYTE* app)
    {
        __asm { push ebp }
        __asm { mov ebp, esp }
        __asm { push ebx }
        __asm { push esi }
        __asm { push edi }
        __asm { fninit }
        __asm { fld1 }
        __asm { fldpi }
        __asm { movdqu xmm0, [g_xmm] }
        __asm { movdqu xmm1, [g_xmm + 16] }
        __asm { movdqu xmm2, [g_xmm + 32] }
        __asm { movdqu xmm3, [g_xmm + 48] }
        __asm { movdqu xmm4, [g_xmm + 64] }
        __asm { movdqu xmm5, [g_xmm + 80] }
        __asm { movdqu xmm6, [g_xmm + 96] }
        __asm { movdqu xmm7, [g_xmm + 112] }
        __asm { fxsave [g_beforeFx] }
        __asm { mov eax, 0x11111111 }
        __asm { mov ebx, 0x22222222 }
        __asm { mov ecx, 0x33333333 }
        __asm { mov edx, 0x44444444 }
        __asm { mov esi, 0x66666666 }
        __asm { mov edi, [ebp + 8] }
        __asm { mov [g_expectedEbp], ebp }
        __asm { stc }
        __asm { std }
        __asm { pushfd }
        __asm { pop [g_beforeFlags] }
        __asm { call dword ptr [g_site] }
        __asm { fxsave [g_afterFx] }
        __asm { pushfd }
        __asm { pop [g_afterFlags] }
        __asm { mov [g_afterEax], eax }
        __asm { mov [g_afterEbx], ebx }
        __asm { mov [g_afterEcx], ecx }
        __asm { mov [g_afterEdx], edx }
        __asm { mov [g_afterEsi], esi }
        __asm { mov [g_afterEdi], edi }
        __asm { mov [g_afterEbp], ebp }
        __asm { cld }
        __asm { fninit }
        __asm { call Inspect }
        __asm { lea esp, [ebp - 12] }
        __asm { pop edi }
        __asm { pop esi }
        __asm { pop ebx }
        __asm { mov esp, ebp }
        __asm { pop ebp }
        __asm { ret }
    }

    int Requested(const std::wstring& mode)
    {
        if (mode == L"160") return 160;
        if (mode == L"120") return 120;
        if (mode == L"60") return 60;
        if (mode == L"disabled" || mode == L"badbytes" ||
            mode == L"wrongkind") return 160;
        if (mode == L"min") return 1;
        if (mode == L"max") return 1000;
        if (mode == L"zero") return 0;
        if (mode == L"over") return 1001;
        if (mode == L"negative") return -1;
        return 160;
    }

    bool ModShouldLoad(const std::wstring& mode)
    {
        return mode != L"disabled" && mode != L"badbytes" && mode != L"wrongkind";
    }
}

int wmain(int argc, wchar_t** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring mode = argc > 1 ? argv[1] : L"160";
    const int requested = Requested(mode);
    const bool active = ModShouldLoad(mode);
    g_expected = active && requested >= 1 && requested <= 1000 ? requested : 60;

    for (unsigned i = 0; i < sizeof(g_xmm); ++i)
        reinterpret_cast<BYTE*>(g_xmm)[i] = static_cast<BYTE>(i ^ 0xA5);
    memset(g_beforeFx, 0, sizeof(g_beforeFx));
    memset(g_afterFx, 0, sizeof(g_afterFx));
    memset(g_app, 0xA5, sizeof(g_app));
    memcpy(g_app + kFpsOffset, &requested, sizeof(requested));
    memcpy(g_beforeApp, g_app, sizeof(g_app));

    DWORD old = 0;
    Check(VirtualProtect(g_code, sizeof(g_code), PAGE_EXECUTE_READWRITE, &old) != 0,
          "reserve owned executable site fixture");
    memset(g_code, 0xCC, sizeof(g_code));
    memcpy(g_code, kNativeSite, sizeof(kNativeSite));
    FlushInstructionCache(GetCurrentProcess(), g_code, sizeof(g_code));

    const auto root = Root();
    auto* host = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<BYTE*>(host);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t key[160] = {};
    swprintf_s(key, L"fps_limit_runtime_test-%08X-%08X",
               nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    char symbols[512] = {};
    sprintf_s(symbols,
              "[host]\n"
              "timestamp=0x%08X\n"
              "size_of_image=0x%08X\n"
              "[fps.config.force60]\n"
              "kind=%s\n"
              "rva=0x%08X\n"
              "expect=%s\n",
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
              mode == L"wrongkind" ? "code" : "site",
              static_cast<unsigned>(g_code - base),
              mode == L"badbytes"
                  ? "CC CC CC CC CC CC CC CC CC CC"
                  : "C7 87 80 00 00 00 3C 00 00 00");
    CreateDirectoryW((root + L"\\sdgo\\symbols").c_str(), nullptr);
    Write(root + L"\\sdgo\\symbols\\" + key + L".ini", symbols);

    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto init = core ? reinterpret_cast<SdgoCoreInitFn>(
                           GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    Check(init != nullptr, "load the real core");
    if (!init) return 2;
    SdgoBoot boot = {sizeof(boot), "fps_limit_test", host};
    Check(init(&boot) != 0, "initialize real core and configured FPS Mod");

    SetLastError(kExpectedError);
    Invoke(g_app);
    puts(g_failures ? "FPS LIMIT RUNTIME CHECKS FAILED"
                    : "ALL FPS LIMIT RUNTIME CHECKS PASSED");
    return g_failures ? 1 : 0;
}
