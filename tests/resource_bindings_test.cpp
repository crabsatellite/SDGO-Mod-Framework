#include "sdgo/resource_bindings.h"
#include "sdgo/script_registration.h"
#include "native_lua_image.h"
#include "../core/hooks.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

using namespace sdgo::resources;
namespace {
int failures = 0;
void Check(bool condition, const char* reason) { std::printf("%s %s\n", condition ? "PASS" : "FAIL", reason); if (!condition) ++failures; }
struct Proof final : Authority {
    std::map<std::uint32_t, std::pair<std::string, std::string>> weapons;
    std::map<std::string, std::string> resources;
    std::set<std::uint32_t> originals;
    bool drift = false; mutable int reads = 0;
    std::string revision() const override { return drift && reads++ ? "changed" : "fixture-publication-v1"; }
    bool originalWeapon(std::uint32_t id) const override { return originals.count(id) != 0; }
    bool allocatedWeapon(const std::string& owner, std::uint32_t id, const std::string& sha) const override { const auto i = weapons.find(id); return i != weapons.end() && i->second.first == owner && i->second.second == sha; }
    bool publishedResource(const std::string& path, const std::string& sha) const override { const auto i = resources.find(path); return i != resources.end() && i->second == sha; }
};
Candidate Make(const std::string& owner, std::uint32_t id, Proof& proof, bool fixed = true) {
    Candidate c; c.owner = owner; c.revision = "fixture-v1";
    Weapon weapon; weapon.id = id; weapon.record.resize(1871); std::memcpy(weapon.record.data(), &id, 4); if (!fixed) weapon.record[348] = 'm'; weapon.sha256 = sdgo::scripts::Sha256(weapon.record);
    c.weapons.push_back(weapon); proof.weapons[id] = {owner, weapon.sha256};
    if (fixed) { Resource r{"fxrs/" + owner + "_tail.zfx", sdgo::scripts::Sha256({1, 2, 3})}; c.resources.push_back(r); c.fixedEffects.push_back({id, r.path}); proof.resources[r.path] = r.sha256; }
    return c;
}
void PolicyTests() {
    Proof proof; auto a = Make("alpha", 600001, proof); auto b = Make("beta", 600002, proof); std::string error;
    auto set = Bindings::Build({a, b}, proof, error); Check(set && set->fixedEffectCount() == 2, "two authenticated owners prepare one immutable binding set");
    const wchar_t* original = L"fxrs\\0121.zfx"; auto r = set->resolve(12, nullptr, 0, original); Check(r.resolution == Resolution::Original && r.path == original, "unowned native weapon retains its exact original argument");
    r = set->resolve(600001, a.weapons[0].record.data(), 1871, original); Check(r.resolution == Resolution::Redirected && std::wstring(r.path) == L"fxrs/alpha_tail.zfx", "allocated weapon and exact record select the owned resource"); const auto* stable = r.path;
    auto other = set->resolve(600002, b.weapons[0].record.data(), 1871, original); Check(std::wstring(other.path) == L"fxrs/beta_tail.zfx" && std::wstring(stable) == L"fxrs/alpha_tail.zfx", "separate owners retain stable distinct resource identities");
    auto record = a.weapons[0].record; record[284] ^= 1; r = set->resolve(600001, record.data(), 1871, original); Check(r.resolution == Resolution::Fatal, "runtime record mismatch cannot borrow original or another owner resource");
    r = set->resolve(600001, a.weapons[0].record.data(), 1871, L"fxrs/other.zfx"); Check(r.resolution == Resolution::Fatal, "unexpected owned call argument is refused");
    proof.originals.insert(600001); Check(!Bindings::Build({a}, proof, error), "a baseline ID cannot be claimed as a private allocation"); proof.originals.clear();
    auto missing = proof; missing.weapons.clear(); Check(!Bindings::Build({a}, missing, error), "self-claimed owner without allocation authority is refused");
    missing = proof; missing.resources.clear(); Check(!Bindings::Build({a}, missing, error), "unpublished target resource blocks preparation");
    auto noAlias = a; noAlias.fixedEffects.clear(); Check(!Bindings::Build({noAlias}, proof, error), "empty-model native fallback requires an explicit owned effect");
    auto explicitModel = Make("gamma", 600003, proof, false); Check(Bindings::Build({explicitModel}, proof, error) != nullptr, "explicit native model does not invent a fallback effect");
    auto wrongBranch = a; wrongBranch.weapons[0].record[1769] = 1; wrongBranch.weapons[0].sha256 = sdgo::scripts::Sha256(wrongBranch.weapons[0].record); proof.weapons[600001].second = wrongBranch.weapons[0].sha256; Check(!Bindings::Build({wrongBranch}, proof, error), "split initializer branch cannot declare the standard fallback"); proof.weapons[600001].second = a.weapons[0].sha256;
    auto shared = b; shared.weapons = a.weapons; shared.fixedEffects[0].weapon = 600001; Check(!Bindings::Build({a, shared}, proof, error), "cross-owner ID reuse is refused before activation");
    auto changed = proof; changed.drift = true; Check(!Bindings::Build({a}, changed, error), "allocation/publication revision must remain stable during preparation");
}

struct Missile { std::uint8_t bytes[0xa00] = {}; };
struct Registers { std::uint32_t edi, esi, ebp, savedEsp, ebx, edx, ecx, eax, flags; };
Registers seen = {};
const wchar_t* seenPath = nullptr;
int captures = 0;
alignas(16) std::uint8_t beforeFx[512] = {}, seenFx[512] = {}, initialFx[512] = {};
alignas(16) const std::uint32_t xmmPattern[4] = {0x12345678, 0x90abcdef, 0x11223344, 0x55667788};
std::uint32_t beforeFlags = 0;
void* slice = reinterpret_cast<void*>(0x650268);
NativeLuaImage* image = nullptr;

__declspec(naked) void CaptureString() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { mov edx, [eax] }
    __asm { mov dword ptr [seen], edx }
    __asm { mov edx, [eax + 4] }
    __asm { mov dword ptr [seen + 4], edx }
    __asm { mov edx, [eax + 8] }
    __asm { mov dword ptr [seen + 8], edx }
    __asm { mov edx, [eax + 12] }
    __asm { mov dword ptr [seen + 12], edx }
    __asm { mov edx, [edx + 8] }
    __asm { mov [seenPath], edx }
    __asm { mov edx, [eax + 16] }
    __asm { mov dword ptr [seen + 16], edx }
    __asm { mov edx, [eax + 20] }
    __asm { mov dword ptr [seen + 20], edx }
    __asm { mov edx, [eax + 24] }
    __asm { mov dword ptr [seen + 24], edx }
    __asm { mov edx, [eax + 28] }
    __asm { mov dword ptr [seen + 28], edx }
    __asm { mov edx, [eax + 32] }
    __asm { mov dword ptr [seen + 32], edx }
    __asm { fxsave [seenFx] }
    __asm { inc dword ptr [captures] }
    __asm { popad }
    __asm { popfd }
    __asm { ret 4 }
}
__declspec(naked) void SliceStop() {
    __asm { cld }
    __asm { fxrstor [initialFx] }
    __asm { pop edi }
    __asm { pop esi }
    __asm { pop ebx }
    __asm { mov esp, ebp }
    __asm { pop ebp }
    __asm { ret 8 }
}
__declspec(naked) void __fastcall Replay(Missile*, void*, std::uint32_t, void*) {
    __asm { push ebp }
    __asm { mov ebp, esp }
    __asm { sub esp, 0x80 }
    __asm { push ebx }
    __asm { push esi }
    __asm { push edi }
    __asm { fxsave [initialFx] }
    __asm { fldpi }
    __asm { movdqu xmm0, [xmmPattern] }
    __asm { mov ebx, ecx }
    __asm { mov eax, [ebp + 0xc] }
    __asm { mov esi, 0xffffffff }
    __asm { mov edi, 0x1122aabb }
    __asm { mov edx, 0x7788ccdd }
    __asm { std }
    __asm { stc }
    __asm { fxsave [beforeFx] }
    __asm { pushfd }
    __asm { pop dword ptr [beforeFlags] }
    __asm { jmp dword ptr [slice] }
}
void* Symbol(SdgoMod*, const char* name) {
    if (std::string(name) == "missile.resource.constructor") return image->at<void*>(0x64ff20);
    if (std::string(name) == "missile.resource.fixed_effect_site") return image->at<void*>(0x650279);
    return nullptr;
}
int Hook(SdgoMod*, const char* name, void* detour, void** original) { std::string error; return sdgo::hooks::HookCode("resource-test", Symbol(nullptr, name), detour, original, error, true) ? 1 : 0; }
void PatchJump(std::uint32_t address, void* target) {
    auto* code = image->at<std::uint8_t*>(address); const std::uint8_t expected[] = {0x8d,0x4d,0xd4,0x51,0x8b,0x8b,0xf8,0x09,0,0};
    if (std::memcmp(code, expected, sizeof(expected)) != 0) throw std::runtime_error("bounded source-stop instruction sequence differs");
    DWORD prior = 0; if (!VirtualProtect(code, sizeof(expected), PAGE_EXECUTE_READWRITE, &prior)) throw std::runtime_error("bounded source-stop protection failed");
    const std::int32_t delta = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(target) - reinterpret_cast<std::intptr_t>(code + 5)); code[0] = 0xe9; std::memcpy(code + 1, &delta, 4); std::memset(code + 5, 0x90, sizeof(expected) - 5); VirtualProtect(code, sizeof(expected), prior, &prior); FlushInstructionCache(GetCurrentProcess(), code, sizeof(expected));
}
void Close(void*, const char* why) { std::printf("PASS fatal owned context requests candidate closure: %s\n", why); Check(captures == 0, "fatal binding never resumes the original string call"); std::fflush(stdout); if (captures != 0) ExitProcess(0xe17d1002); }

int NativeReplay(const char* path, bool fatal) {
    NativeLuaImage mapped(path, true); image = &mapped; std::string error; if (!sdgo::hooks::Initialize(error)) throw std::runtime_error(error);
    Proof proof; auto a = Make("alpha", 600001, proof); auto b = Make("beta", 600002, proof); auto set = Bindings::Build({a, b}, proof, error); if (!set) throw std::runtime_error(error);
    SdgoApi api = {}; api.size = sizeof(api); api.Symbol = Symbol; api.HookSite = Hook;
    auto* fingerprintByte = image->at<std::uint8_t*>(0x64ff25); DWORD protection = 0; VirtualProtect(fingerprintByte, 1, PAGE_EXECUTE_READWRITE, &protection); const auto saved = *fingerprintByte; *fingerprintByte ^= 1;
    Check(!InstallFixedEffectHook(&api, reinterpret_cast<SdgoMod*>(1), set, Close, nullptr, error) && error.find("fingerprint") != std::string::npos, "changed constructor bytes are rejected before any hook is installed"); *fingerprintByte = saved; VirtualProtect(fingerprintByte, 1, protection, &protection);
    sdgo::hooks::BeginBatch(); Check(InstallFixedEffectHook(&api, reinterpret_cast<SdgoMod*>(1), set, Close, nullptr, error), "whole original constructor hash and verified CALL site accept the hook");
    Check(!InstallFixedEffectHook(&api, reinterpret_cast<SdgoMod*>(1), set, Close, nullptr, error), "an already owned fixed-effect site cannot be rebound live");
    void* originalString = nullptr; if (!sdgo::hooks::HookCode("capture-string", image->at<void*>(0x4027e0), reinterpret_cast<void*>(&CaptureString), &originalString, error, true)) throw std::runtime_error(error);
    std::size_t enabled = 0; if (!sdgo::hooks::EndBatch(&enabled, error)) throw std::runtime_error(error);
    Check(enabled == 2, "real core/MinHook queues both isolated fixture hooks");
    // Stop immediately after the authentic string-construction CALL. The native
    // effect loader/rendering/lifetime are deliberately outside this replay.
    PatchJump(0x65027e, reinterpret_cast<void*>(&SliceStop));
    Missile missile; auto record = a.weapons[0].record; const auto* pointer = record.data(); std::memcpy(missile.bytes + 0x9c8, &pointer, 4); void* effect = reinterpret_cast<void*>(0x24681357);
    if (fatal) { record[284] ^= 1; captures = 0; Replay(&missile, nullptr, 600001, effect); throw std::runtime_error("fatal constructor unexpectedly resumed"); }
    SetLastError(0x13572468); Replay(&missile, nullptr, 600001, effect); const auto lastError = GetLastError();
    Check(captures == 1 && seenPath && std::wstring(seenPath) == L"fxrs/alpha_tail.zfx", "actual original constructor instructions consume the owned string argument");
    Check(seen.ebx == reinterpret_cast<std::uintptr_t>(&missile) && seen.eax == reinterpret_cast<std::uintptr_t>(effect) && seen.esi == 0xffffffff && seen.edi == 0x1122aabb && seen.edx == 0x7788ccdd && seen.ecx == seen.ebp - 0x2c, "register context survives the instruction hook");
    Check((seen.flags & 0xcd5) == (beforeFlags & 0xcd5) && lastError == 0x13572468, "arithmetic/direction flags and Win32 last-error survive the hook");
    Check(std::memcmp(beforeFx, seenFx, 6) == 0 && std::memcmp(beforeFx + 24, seenFx + 24, 8) == 0 && std::memcmp(beforeFx + 32, seenFx + 32, 256) == 0, "x87 control/data, MXCSR and XMM state survive the hook");
    void* retained = nullptr; std::memcpy(&retained, missile.bytes + 0x9f8, 4); Check(retained == effect, "original instruction retains its existing effect object without a second controller");
    Replay(&missile, nullptr, 12, effect); Check(std::wstring(seenPath) == L"fxrs\\0121.zfx", "unowned original weapon stays on the original path");
    record = b.weapons[0].record; pointer = record.data(); std::memcpy(missile.bytes + 0x9c8, &pointer, 4); Replay(&missile, nullptr, 600002, effect); Check(std::wstring(seenPath) == L"fxrs/beta_tail.zfx", "same fixed source CALL supports a second independent owner");
    return failures ? 1 : 0;
}

int Child(const char* path, bool fatal) {
    wchar_t executable[32768] = {}; GetModuleFileNameW(nullptr, executable, 32768);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0); if (!n) throw std::runtime_error("native input path encoding"); std::vector<wchar_t> wide(static_cast<std::size_t>(n)); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), n);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --source-child \"" + wide.data() + L"\"" + (fatal ? L" --fatal" : L"");
    STARTUPINFOW start = {}; start.cb = sizeof(start); start.dwFlags = STARTF_USESTDHANDLES; start.hStdInput = GetStdHandle(STD_INPUT_HANDLE); start.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); start.hStdError = GetStdHandle(STD_ERROR_HANDLE); PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &start, &process)) throw std::runtime_error("owned resource replay child unavailable");
    DWORD result = 1;
    try {
        void* block = VirtualAllocEx(process.hProcess, reinterpret_cast<void*>(0x400000), 0xc97000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); SIZE_T written = 0;
        if (block != reinterpret_cast<void*>(0x400000) || !WriteProcessMemory(process.hProcess, block, NativeLuaImage::kReservationCookie, sizeof(NativeLuaImage::kReservationCookie), &written) || written != sizeof(NativeLuaImage::kReservationCookie)) throw std::runtime_error("owned resource replay range unavailable");
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1) || WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &result)) throw std::runtime_error("owned resource replay did not complete");
    } catch (...) { TerminateProcess(process.hProcess, 124); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hThread); CloseHandle(process.hProcess); throw; }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (fatal) { Check(result == 0xe17d1001, "fatal partially constructed candidate exits before native continuation"); return failures ? 1 : 0; }
    return static_cast<int>(result);
}
}
int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--source-child") return NativeReplay(argv[2], argc > 3);
        if (argc >= 3 && std::string(argv[1]) == "--native-client") return Child(argv[2], argc > 3);
        PolicyTests();
    } catch (const std::exception& e) { std::printf("FAIL %s\n", e.what()); ++failures; }
    return failures ? 1 : 0;
}
