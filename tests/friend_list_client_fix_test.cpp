#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "../mods/friend_list_client_fix/chat_open_policy.h"
#include "../mods/friend_list_client_fix/messenger_layout_policy.h"

extern "C" __declspec(dllimport) int __cdecl SdgoModLoad(const SdgoApi*, SdgoMod*);

namespace
{
    constexpr std::uint32_t kZf = 0x40;
    constexpr std::uint32_t kInjectedFlags = 0x0C01; // CF/DF/OF
    constexpr std::uint32_t kObservedFlags = 0x0FD5; // CF/PF/AF/ZF/SF/TF/IF/DF/OF
    constexpr std::uint32_t kEbx = 0x11223344;
    constexpr std::uint32_t kEcx = 0x55667788;
    constexpr std::uint32_t kEdx = 0x99AABBCC;
    constexpr std::uint32_t kEsi = 0x13579BDF;
    constexpr std::uint32_t kEdi = 0x2468ACE0;
    constexpr std::uint32_t kEbp = 0x0BADF00D;

    struct GateState
    {
        std::uint32_t eax;
        std::uint32_t ebx;
        std::uint32_t ecx;
        std::uint32_t edx;
        std::uint32_t esi;
        std::uint32_t edi;
        std::uint32_t ebp;
        std::uint32_t entryEsp;
        std::uint32_t exitEsp;
    };

    struct LayoutCallState
    {
        std::uint32_t result;
        std::uint32_t ebx;
        std::uint32_t esi;
        std::uint32_t edi;
        std::uint32_t ebp;
        std::uint32_t entryEsp;
        std::uint32_t exitEsp;
    };

    using LayoutCaller =
        void* (__stdcall*)(void*, const wchar_t*, std::uint32_t);
    using NativeAssign =
        void* (__thiscall*)(void*, const wchar_t*, std::size_t);

    struct LayoutCallerFixture
    {
        std::uint8_t* code = nullptr;
        void* callSite = nullptr;
        void* returnAddress = nullptr;
        LayoutCaller invoke = nullptr;
    };

    int g_checks = 0;
    int g_failures = 0;
    bool g_rejectLayout = false;
    bool g_rejectMessengerSymbol = false;
    bool g_rejectServiceHook = false;
    bool g_rejectPopupHook = false;
    bool g_rejectHook = false;
    bool g_layoutRequested = false;
    bool g_gateRequested = false;
    bool g_assignRequested = false;
    bool g_serviceCallRequested = false;
    bool g_popupCallRequested = false;
    bool g_serviceReturnRequested = false;
    bool g_popupReturnRequested = false;
    bool g_originalLayoutRequested = false;
    bool g_serviceHookRequested = false;
    bool g_popupHookRequested = false;
    void* g_gateCode = nullptr;
    void* g_assignCode = nullptr;
    std::uint32_t g_openCalls = 0;
    std::uint32_t g_openFlags = 0;
    std::uint32_t g_blockedFlags = 0;
    GateState g_state = {};
    LayoutCallState g_layoutState = {};
    void* g_currentLayoutCaller = nullptr;
    std::uint32_t g_layoutExpectedFlags = 0;
    std::uint32_t g_assignEntryFlags = 0;
    void* g_assignedSelf = nullptr;
    const wchar_t* g_assignedText = nullptr;
    std::uint32_t g_assignedLength = 0;
    std::uint32_t g_assignCalls = 0;
    std::uint32_t g_otherAssignCalls = 0;
    NativeAssign g_otherOriginalAssign = nullptr;
    wchar_t g_assignedCopy[32] = {};
    wchar_t g_originalLayout[] = L"uirs\\1364.uir";
    LayoutCallerFixture g_serviceCaller = {};
    LayoutCallerFixture g_popupCaller = {};
    LayoutCallerFixture g_exchangeCaller = {};

    void Check(bool value, const char* message)
    {
        ++g_checks;
        if (!value) {
            ++g_failures;
            std::printf("FAIL %s\n", message);
        }
    }

    bool Same(const char* left, const char* right)
    {
        return left && right && std::strcmp(left, right) == 0;
    }

    void FakeLog(SdgoMod*, int, const char*, ...) {}

    void* FakeSymbol(SdgoMod*, const char* name)
    {
        if (Same(name, friend_list_client_fix::kLayoutSymbol)) {
            g_layoutRequested = true;
            return g_rejectLayout ? nullptr : g_gateCode;
        }
        if (Same(name, friend_list_client_fix::kAssignSymbol)) {
            g_assignRequested = true;
            return g_rejectMessengerSymbol ? nullptr : g_assignCode;
        }
        if (Same(name, friend_list_client_fix::kServiceLayoutCallSymbol)) {
            g_serviceCallRequested = true;
            return g_rejectMessengerSymbol
                ? nullptr : g_serviceCaller.callSite;
        }
        if (Same(name, friend_list_client_fix::kPopupLayoutCallSymbol)) {
            g_popupCallRequested = true;
            return g_rejectMessengerSymbol
                ? nullptr : g_popupCaller.callSite;
        }
        if (Same(name, friend_list_client_fix::kServiceLayoutReturnSymbol)) {
            g_serviceReturnRequested = true;
            return g_rejectMessengerSymbol
                ? nullptr : g_serviceCaller.returnAddress;
        }
        if (Same(name, friend_list_client_fix::kPopupLayoutReturnSymbol)) {
            g_popupReturnRequested = true;
            return g_rejectMessengerSymbol
                ? nullptr : g_popupCaller.returnAddress;
        }
        if (Same(name, friend_list_client_fix::kOriginalLayoutSymbol)) {
            g_originalLayoutRequested = true;
            return g_rejectMessengerSymbol ? nullptr : g_originalLayout;
        }
        return nullptr;
    }

    int FakeHookSite(SdgoMod*, const char* name, void* detour, void** original)
    {
        void* site = nullptr;
        bool reject = false;
        if (Same(name, friend_list_client_fix::kServiceLayoutCallSymbol)) {
            g_serviceHookRequested = true;
            site = g_serviceCaller.callSite;
            reject = g_rejectServiceHook;
        } else if (Same(name, friend_list_client_fix::kPopupLayoutCallSymbol)) {
            g_popupHookRequested = true;
            site = g_popupCaller.callSite;
            reject = g_rejectPopupHook;
        } else if (Same(name, friend_list_client_fix::kGateSymbol)) {
            g_gateRequested = true;
            site = g_gateCode;
            reject = g_rejectHook;
        } else {
            return 0;
        }
        if (reject || !site || !detour || !original) return 0;
        if (MH_CreateHook(site, detour, original) != MH_OK) return 0;
        return MH_EnableHook(site) == MH_OK;
    }

    void* __cdecl RecordNativeAssign(
        void* self, const wchar_t* text, std::size_t length)
    {
        g_assignedSelf = self;
        g_assignedText = text;
        g_assignedLength = static_cast<std::uint32_t>(length);
        ++g_assignCalls;
        const std::size_t copyLength =
            (text && length < std::size(g_assignedCopy))
                ? length : (std::size(g_assignedCopy) - 1);
        if (text) {
            std::memcpy(g_assignedCopy, text, copyLength * sizeof(wchar_t));
        }
        g_assignedCopy[copyLength] = L'\0';
        return self;
    }

    __declspec(naked) void* NativeAssignFixture()
    {
        __asm {
            pushfd
            pop dword ptr [g_assignEntryFlags]
            push dword ptr [esp + 8]
            push dword ptr [esp + 8]
            push ecx
            call RecordNativeAssign
            add esp, 0Ch
            ret 8
        }
    }

    void* __fastcall OtherAssignOwner(
        void* self, void*, const wchar_t* text, std::size_t length)
    {
        ++g_otherAssignCalls;
        return g_otherOriginalAssign(self, text, length);
    }

    bool InstallOtherAssignOwner()
    {
        g_otherOriginalAssign = nullptr;
        if (MH_CreateHook(
                g_assignCode, reinterpret_cast<void*>(&OtherAssignOwner),
                reinterpret_cast<void**>(&g_otherOriginalAssign)) != MH_OK) {
            return false;
        }
        return MH_EnableHook(g_assignCode) == MH_OK;
    }

    bool BuildLayoutCaller(LayoutCallerFixture& fixture)
    {
        auto* code = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                         PAGE_EXECUTE_READWRITE));
        if (!code) return false;

        // stdcall fixture(self,text,length):
        //   ECX=self; push length; push text; direct-call native assign; ret 12.
        const std::uint8_t prefix[] = {
            0x8B, 0x4C, 0x24, 0x04,
            0x8B, 0x44, 0x24, 0x0C,
            0x8B, 0x54, 0x24, 0x08,
            0x50,
            0x52,
            0xE8
        };
        std::memcpy(code, prefix, sizeof(prefix));
        const auto next = reinterpret_cast<std::intptr_t>(code + 19);
        const auto destination =
            reinterpret_cast<std::intptr_t>(g_assignCode);
        const auto displacement =
            static_cast<std::int32_t>(destination - next);
        std::memcpy(code + 15, &displacement, sizeof(displacement));
        code[19] = 0xC2;
        code[20] = 0x0C;
        code[21] = 0x00;
        FlushInstructionCache(GetCurrentProcess(), code, 22);
        fixture.code = code;
        fixture.callSite = code + 14;
        fixture.returnAddress = code + 19;
        fixture.invoke = reinterpret_cast<LayoutCaller>(code);
        return true;
    }

    __declspec(naked) void RunLayoutCaller(
        void*, const wchar_t*, std::uint32_t)
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov dword ptr [g_layoutState + 20], esp
            mov ebx, 11223344h
            mov esi, 13579BDFh
            mov edi, 2468ACE0h
            mov ebp, 0BADF00Dh
            pushfd
            or dword ptr [esp], 0C01h
            popfd
            pushfd
            pop dword ptr [g_layoutExpectedFlags]
            mov eax, dword ptr [esp + 28]
            mov ecx, dword ptr [esp + 24]
            mov edx, dword ptr [esp + 20]
            push eax
            push ecx
            push edx
            call dword ptr [g_currentLayoutCaller]
            mov dword ptr [g_layoutState + 0], eax
            mov dword ptr [g_layoutState + 4], ebx
            mov dword ptr [g_layoutState + 8], esi
            mov dword ptr [g_layoutState + 12], edi
            mov dword ptr [g_layoutState + 16], ebp
            mov eax, esp
            mov dword ptr [g_layoutState + 24], eax
            cld
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    void CheckLayoutCall(
        const LayoutCallerFixture& fixture, const wchar_t* requested,
        std::uint32_t length, const wchar_t* expectedPointer,
        const wchar_t* expectedText, const char* behavior,
        bool checkEntryFlags = true)
    {
        std::uint32_t self = 0xA1B2C3D4;
        g_layoutState = {};
        g_assignedSelf = nullptr;
        g_assignedText = nullptr;
        g_assignedLength = 0;
        g_assignCalls = 0;
        g_assignEntryFlags = 0;
        std::memset(g_assignedCopy, 0, sizeof(g_assignedCopy));
        g_currentLayoutCaller = reinterpret_cast<void*>(fixture.invoke);
        RunLayoutCaller(&self, requested, length);

        Check(g_assignCalls == 1 && g_assignedSelf == &self &&
                  g_layoutState.result ==
                      static_cast<std::uint32_t>(
                          reinterpret_cast<std::uintptr_t>(&self)),
              behavior);
        Check((!expectedPointer || g_assignedText == expectedPointer) &&
                  g_assignedLength == length &&
                  std::wcscmp(g_assignedCopy, expectedText) == 0,
              "layout hook forwards the expected pointer, length and text");
        Check(g_layoutState.ebx == kEbx && g_layoutState.esi == kEsi &&
                  g_layoutState.edi == kEdi && g_layoutState.ebp == kEbp,
              "layout detour and trampoline preserve callee-saved registers");
        if (checkEntryFlags) {
            Check(((g_assignEntryFlags ^ g_layoutExpectedFlags) &
                   kObservedFlags) == 0,
                  "layout callsite detour preserves incoming flags");
        }
        Check(g_layoutState.entryEsp == g_layoutState.exitEsp,
              "layout detour, trampoline and native ret 8 keep the stack balanced");
    }

    // Called by the native instruction fixture after its `push eax; mov ecx,esi`.
    // It snapshots flags at the real open-conversation boundary, then emulates
    // thiscall cleanup of the one stack argument.
    __declspec(naked) void OpenConversationFixture()
    {
        __asm {
            pushfd
            pop dword ptr [g_openFlags]
            inc dword ptr [g_openCalls]
            mov eax, 1
            ret 4
        }
    }

    bool BuildGateFixture()
    {
        auto* code = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!code) return false;

        // Same gate instructions as GOnline 0x00964549 through 0x00964552:
        //   je +8; push eax; mov ecx,esi; call OpenConversationFixture
        // Both paths then snapshot flags and return to the test caller.
        const std::uint8_t prefix[] = {0x74, 0x08, 0x50, 0x8B, 0xCE, 0xE8};
        std::memcpy(code, prefix, sizeof(prefix));
        const auto next = reinterpret_cast<std::intptr_t>(code + 10);
        const auto destination = reinterpret_cast<std::intptr_t>(&OpenConversationFixture);
        const auto displacement = static_cast<std::int32_t>(destination - next);
        std::memcpy(code + 6, &displacement, sizeof(displacement));
        code[10] = 0x9C; // pushfd
        code[11] = 0x58; // pop eax
        code[12] = 0xA3; // mov [g_blockedFlags],eax
        const auto flagsAddress =
            static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_blockedFlags));
        std::memcpy(code + 13, &flagsAddress, sizeof(flagsAddress));
        code[17] = 0xC3; // ret
        FlushInstructionCache(GetCurrentProcess(), code, 18);
        g_gateCode = code;
        return true;
    }

    __declspec(naked) std::uint32_t NativeCmpFlags(const std::uint8_t*)
    {
        __asm {
            mov eax, dword ptr [esp + 4]
            cmp dword ptr [eax + 1Ch], 0
            pushfd
            pop eax
            ret
        }
    }

    // Enters the actual hooked instruction fixture with the same live EAX and
    // flags as the client. Captures register and stack effects after the
    // trampoline either opens or skips the native conversation call.
    __declspec(naked) void RunGate(const std::uint8_t*)
    {
        __asm {
            push ebp
            push ebx
            push esi
            push edi
            mov dword ptr [g_state + 28], esp
            mov ebx, 11223344h
            mov ecx, 55667788h
            mov edx, 99AABBCCh
            mov esi, 13579BDFh
            mov edi, 2468ACE0h
            mov ebp, 0BADF00Dh
            mov eax, dword ptr [esp + 20]
            cmp dword ptr [eax + 1Ch], 0
            pushfd
            or dword ptr [esp], 0C01h
            popfd
            call dword ptr [g_gateCode]
            mov dword ptr [g_state + 0], eax
            mov dword ptr [g_state + 4], ebx
            mov dword ptr [g_state + 8], ecx
            mov dword ptr [g_state + 12], edx
            mov dword ptr [g_state + 16], esi
            mov dword ptr [g_state + 20], edi
            mov dword ptr [g_state + 24], ebp
            mov eax, esp
            mov dword ptr [g_state + 32], eax
            cld
            pop edi
            pop esi
            pop ebx
            pop ebp
            ret
        }
    }

    std::array<std::uint8_t, 0x38> Row(std::uint32_t identity, std::uint8_t nameLead,
                                       std::uint8_t relationship, std::uint32_t presence)
    {
        std::array<std::uint8_t, 0x38> row = {};
        std::memcpy(row.data(), &identity, sizeof(identity));
        row[0x0A] = nameLead;
        row[0x1B] = relationship;
        std::memcpy(row.data() + 0x1C, &presence, sizeof(presence));
        return row;
    }

    void CheckGate(const std::array<std::uint8_t, 0x38>& row, bool opens,
                   bool clearsZeroFlag, const char* behavior)
    {
        g_openCalls = 0;
        g_openFlags = 0;
        g_blockedFlags = 0;
        g_state = {};
        const std::uint32_t nativeFlags = NativeCmpFlags(row.data());
        RunGate(row.data());

        Check((g_openCalls != 0) == opens, behavior);
        const std::uint32_t observedFlags = opens ? g_openFlags : g_blockedFlags;
        const std::uint32_t injectedFlags = nativeFlags | kInjectedFlags;
        const std::uint32_t expectedFlags =
            clearsZeroFlag ? injectedFlags & ~kZf : injectedFlags;
        Check(((observedFlags ^ expectedFlags) & kObservedFlags) == 0,
              "detour changes only the intended zero flag");
        Check(g_state.ebx == kEbx && g_state.edx == kEdx &&
                  g_state.esi == kEsi && g_state.edi == kEdi && g_state.ebp == kEbp,
              "detour and trampoline preserve unrelated general registers");
        Check(g_state.ecx == (opens ? kEsi : kEcx),
              "native mov ecx,esi runs only on the open path");
        Check(g_state.entryEsp == g_state.exitEsp,
              "detour, trampoline and native thiscall keep the stack balanced");
    }

    template <std::size_t N>
    bool Match(const std::vector<std::uint8_t>& image, std::size_t offset,
               const std::array<std::uint8_t, N>& expected)
    {
        return offset <= image.size() && N <= image.size() - offset &&
               std::equal(expected.begin(), expected.end(), image.begin() + offset);
    }

    bool NtHeaders(const std::vector<std::uint8_t>& image,
                   const IMAGE_NT_HEADERS32*& nt)
    {
        nt = nullptr;
        if (image.size() < sizeof(IMAGE_DOS_HEADER)) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
        const std::size_t offset = static_cast<std::size_t>(dos->e_lfanew);
        if (offset > image.size() ||
            sizeof(IMAGE_NT_HEADERS32) > image.size() - offset) {
            return false;
        }
        nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image.data() + offset);
        return nt->Signature == IMAGE_NT_SIGNATURE &&
               nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    }

    bool RvaToOffset(const std::vector<std::uint8_t>& image, std::uint32_t rva,
                     std::size_t& offset)
    {
        const IMAGE_NT_HEADERS32* nt = nullptr;
        if (!NtHeaders(image, nt)) return false;
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        const auto sectionOffset =
            reinterpret_cast<const std::uint8_t*>(sections) - image.data();
        if (sectionOffset < 0 ||
            static_cast<std::size_t>(sectionOffset) > image.size() ||
            nt->FileHeader.NumberOfSections >
                (image.size() - static_cast<std::size_t>(sectionOffset)) /
                    sizeof(IMAGE_SECTION_HEADER)) {
            return false;
        }
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const auto& section = sections[i];
            const std::uint32_t span =
                (section.Misc.VirtualSize > section.SizeOfRawData)
                    ? section.Misc.VirtualSize
                    : section.SizeOfRawData;
            if (rva < section.VirtualAddress ||
                rva - section.VirtualAddress >= span) {
                continue;
            }
            const std::uint32_t within = rva - section.VirtualAddress;
            if (within >= section.SizeOfRawData) return false;
            offset = static_cast<std::size_t>(section.PointerToRawData) + within;
            return offset < image.size();
        }
        return false;
    }

    bool DirectCallTargets(const std::vector<std::uint8_t>& image,
                           std::uint32_t returnRva,
                           std::uint32_t targetRva)
    {
        if (returnRva < 5) return false;
        std::size_t offset = 0;
        if (!RvaToOffset(image, returnRva - 5, offset) ||
            offset > image.size() || 5 > image.size() - offset ||
            image[offset] != 0xE8) {
            return false;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, image.data() + offset + 1,
                    sizeof(displacement));
        return static_cast<std::uint32_t>(
                   static_cast<std::int64_t>(returnRva) + displacement) ==
               targetRva;
    }

    void CheckClientImage(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::vector<std::uint8_t> image((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
        Check(input.good() || input.eof(), "supplied client image is readable");

        const IMAGE_NT_HEADERS32* nt = nullptr;
        Check(NtHeaders(image, nt) &&
                  nt->FileHeader.TimeDateStamp == 0x54BE1568 &&
                  nt->OptionalHeader.SizeOfImage == 0x00C97000,
              "fixture is the supported GOnline image");

        constexpr std::array<std::uint8_t, 17> layout = {
            0x83, 0x78, 0x1C, 0x00, 0x74, 0x08, 0x50, 0x8B, 0xCE,
            0xE8, 0x3D, 0xE9, 0xFF, 0xFF, 0x8B, 0x4D, 0xF4
        };
        constexpr std::array<std::uint8_t, 13> gate = {
            0x74, 0x08, 0x50, 0x8B, 0xCE, 0xE8, 0x3D,
            0xE9, 0xFF, 0xFF, 0x8B, 0x4D, 0xF4
        };
        constexpr std::array<std::uint8_t, 24> assign = {
            0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x56, 0x57,
            0x8B, 0xF1, 0x85, 0xC0, 0x74, 0x45, 0x8B, 0x56,
            0x14, 0x83, 0xFA, 0x08, 0x72, 0x02, 0x8B, 0x0E
        };
        constexpr std::array<std::uint8_t, 5> serviceCall = {
            0xE8, 0xDA, 0x8D, 0xAA, 0xFF
        };
        constexpr std::array<std::uint8_t, 5> popupCall = {
            0xE8, 0xCD, 0x07, 0xAA, 0xFF
        };
        constexpr std::array<std::uint8_t, 9> serviceReturn = {
            0x6A, 0x0C, 0x33, 0xD2, 0x68, 0xD4, 0xD6, 0xEA, 0x00
        };
        constexpr std::array<std::uint8_t, 9> popupReturn = {
            0x6A, 0x10, 0x33, 0xD2, 0x68, 0x2C, 0xD9, 0xEA, 0x00
        };
        constexpr std::array<std::uint8_t, 28> originalLayout = {
            0x75, 0x00, 0x69, 0x00, 0x72, 0x00, 0x73, 0x00,
            0x5C, 0x00, 0x31, 0x00, 0x33, 0x00, 0x36, 0x00,
            0x34, 0x00, 0x2E, 0x00, 0x75, 0x00, 0x69, 0x00,
            0x72, 0x00, 0x00, 0x00
        };
        std::size_t layoutOffset = 0;
        std::size_t gateOffset = 0;
        std::size_t assignOffset = 0;
        std::size_t serviceCallOffset = 0;
        std::size_t popupCallOffset = 0;
        std::size_t serviceReturnOffset = 0;
        std::size_t popupReturnOffset = 0;
        std::size_t originalLayoutOffset = 0;
        Check(RvaToOffset(image, friend_list_client_fix::kLayoutRva, layoutOffset) &&
                  Match(image, layoutOffset, layout),
              "exact presence comparison and native chat call are pinned");
        const bool gateMatches =
            RvaToOffset(image, friend_list_client_fix::kGateRva, gateOffset) &&
            Match(image, gateOffset, gate);
        Check(gateMatches,
              "HookSite begins at the exact conditional branch");
        Check(RvaToOffset(image, friend_list_client_fix::kAssignRva,
                          assignOffset) &&
                  Match(image, assignOffset, assign),
              "native wide-string assign ABI and entry are pinned");
        Check(RvaToOffset(
                  image, friend_list_client_fix::kServiceLayoutCallRva,
                  serviceCallOffset) &&
                  Match(image, serviceCallOffset, serviceCall) &&
                  friend_list_client_fix::kServiceLayoutCallRva + 5 ==
                      friend_list_client_fix::kServiceLayoutReturnRva,
              "service layout HookSite is the exact five-byte assign call");
        Check(RvaToOffset(
                  image, friend_list_client_fix::kPopupLayoutCallRva,
                  popupCallOffset) &&
                  Match(image, popupCallOffset, popupCall) &&
                  friend_list_client_fix::kPopupLayoutCallRva + 5 ==
                      friend_list_client_fix::kPopupLayoutReturnRva,
              "popup layout HookSite is the exact five-byte assign call");
        Check(RvaToOffset(
                  image, friend_list_client_fix::kServiceLayoutReturnRva,
                  serviceReturnOffset) &&
                  Match(image, serviceReturnOffset, serviceReturn) &&
                  DirectCallTargets(
                      image,
                      friend_list_client_fix::kServiceLayoutReturnRva,
                      friend_list_client_fix::kAssignRva),
              "messenger service layout assign return and target are pinned");
        Check(RvaToOffset(
                  image, friend_list_client_fix::kPopupLayoutReturnRva,
                  popupReturnOffset) &&
                  Match(image, popupReturnOffset, popupReturn) &&
                  DirectCallTargets(
                      image,
                      friend_list_client_fix::kPopupLayoutReturnRva,
                      friend_list_client_fix::kAssignRva),
              "messenger popup layout assign return and target are pinned");
        const bool originalLayoutMatches =
            RvaToOffset(image, friend_list_client_fix::kOriginalLayoutRva,
                        originalLayoutOffset) &&
            Match(image, originalLayoutOffset, originalLayout);
        Check(originalLayoutMatches,
              "shared original 1364 path pointer and terminator are pinned");
        Check(DirectCallTargets(
                  image, friend_list_client_fix::kExchangeLayoutReturnRva,
                  friend_list_client_fix::kAssignRva),
              "parts-exchange 1364 caller remains a distinct forwarded caller");

        if (gateMatches) {
            image[gateOffset] ^= 1;
            Check(!Match(image, gateOffset, gate),
                  "a one-byte host change fails the exact gate signature");
        }
        if (originalLayoutMatches) {
            image[originalLayoutOffset + 10] ^= 1;
            Check(!Match(image, originalLayoutOffset, originalLayout),
                  "a one-byte resource-path change fails the exact data receipt");
        }
    }

    void ResetRequestReceipts()
    {
        g_layoutRequested = false;
        g_gateRequested = false;
        g_assignRequested = false;
        g_serviceCallRequested = false;
        g_popupCallRequested = false;
        g_serviceReturnRequested = false;
        g_popupReturnRequested = false;
        g_originalLayoutRequested = false;
        g_serviceHookRequested = false;
        g_popupHookRequested = false;
    }

    bool RemoveHook(void* target)
    {
        return target && MH_DisableHook(target) == MH_OK &&
               MH_RemoveHook(target) == MH_OK;
    }

    bool RemoveFriendHooks()
    {
        const bool service = RemoveHook(g_serviceCaller.callSite);
        const bool popup = RemoveHook(g_popupCaller.callSite);
        const bool gate = RemoveHook(g_gateCode);
        return service && popup && gate;
    }

    bool AllFriendReceipts()
    {
        return g_layoutRequested && g_gateRequested && g_assignRequested &&
               g_serviceCallRequested && g_popupCallRequested &&
               g_serviceReturnRequested && g_popupReturnRequested &&
               g_originalLayoutRequested && g_serviceHookRequested &&
               g_popupHookRequested;
    }

    void CheckOtherOwnerCall(
        const LayoutCallerFixture& fixture, const wchar_t* requested,
        std::uint32_t length, const wchar_t* expectedPointer,
        const wchar_t* expectedText, const char* behavior)
    {
        const std::uint32_t before = g_otherAssignCalls;
        CheckLayoutCall(
            fixture, requested, length, expectedPointer, expectedText,
            behavior, false);
        Check(g_otherAssignCalls == before + 1,
              "independent global assign owner still receives the call");
    }
}

int wmain(int argc, wchar_t** argv)
{
    using namespace friend_list_client_fix;

    const FriendRow offline{1001, static_cast<std::uint8_t>('A'), 7, 0};
    const FriendRow online{1001, static_cast<std::uint8_t>('A'), 7, 1};
    const FriendRow inRoom{1001, static_cast<std::uint8_t>('A'), 7, 3};
    const FriendRow category{0, 0, 0, 0};
    const FriendRow pending{1001, static_cast<std::uint8_t>('A'), 2, 0};
    const FriendRow unnamed{1001, 0, 7, 0};
    Check(!OpensNativeConversation(false, &offline),
          "no FriendTree selection exits before the hook");
    Check(!OpensNativeConversation(true, nullptr),
          "a selection without a friend payload is not opened");
    Check(OpensNativeConversation(true, &online) &&
              OpensNativeConversation(true, &inRoom),
          "online and in-room friends retain the original open path");
    Check(OpensNativeConversation(true, &offline),
          "complete accepted offline friend is newly allowed");
    Check(!OpensNativeConversation(true, &category) &&
              !OpensNativeConversation(true, &pending) &&
              !OpensNativeConversation(true, &unnamed),
          "category, pending and unnamed rows remain blocked");

    if (argc > 2) {
        std::puts("FAIL usage: friend_list_client_fix_test [GOnline.exe]");
        return 2;
    }
    Check(BuildGateFixture(), "native gate fixture allocated");
    if (!g_gateCode) return 2;
    g_assignCode = reinterpret_cast<void*>(&NativeAssignFixture);
    Check(BuildLayoutCaller(g_serviceCaller) &&
              BuildLayoutCaller(g_popupCaller) &&
              BuildLayoutCaller(g_exchangeCaller),
          "three direct native assign caller fixtures allocated");
    if (!g_serviceCaller.code || !g_popupCaller.code ||
        !g_exchangeCaller.code) {
        return 2;
    }

    SdgoApi api = {};
    api.version = SDGO_API_VERSION;
    api.size = static_cast<std::uint32_t>(offsetof(SdgoApi, HookSite));
    api.Log = &FakeLog;
    api.Symbol = &FakeSymbol;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0,
          "mod declines an older core before reading HookSite");

    api.size = sizeof(api);
    api.HookSite = &FakeHookSite;
    ResetRequestReceipts();
    g_rejectMessengerSymbol = true;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0 &&
              !g_serviceHookRequested && !g_popupHookRequested &&
              !g_gateRequested,
          "missing messenger layout symbol fails closed before hooking");
    g_rejectMessengerSymbol = false;
    ResetRequestReceipts();
    g_rejectLayout = true;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0 &&
              !g_serviceHookRequested && !g_popupHookRequested &&
              !g_gateRequested,
          "missing or mismatched layout receipt fails closed before hooking");
    g_rejectLayout = false;
    ResetRequestReceipts();
    g_rejectServiceHook = true;
    Check(SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) == 0 &&
              g_serviceHookRequested && !g_popupHookRequested &&
              !g_gateRequested,
          "rejected service callsite ownership fails closed");
    g_rejectServiceHook = false;

    MH_STATUS initialized = MH_Initialize();
    Check(initialized == MH_OK,
          "MinHook initialized for friend-first compatibility fixtures");
    ResetRequestReceipts();
    const bool active = initialized == MH_OK &&
        SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) != 0;
    Check(active && AllFriendReceipts(),
          "production DLL binds all verified symbols and activates three scoped sites");

    if (active) {
        CheckLayoutCall(
            g_serviceCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            nullptr, kFriendLayoutPath,
            "messenger service caller is redirected to the shipped friend layout");
        const wchar_t* replacementPointer = g_assignedText;
        Check(replacementPointer != nullptr &&
                  replacementPointer != g_originalLayout,
              "replacement path is stable storage owned by the production DLL");
        CheckLayoutCall(
            g_popupCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            replacementPointer, kFriendLayoutPath,
            "messenger popup caller is redirected to the same friend layout");
        CheckLayoutCall(
            g_exchangeCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            g_originalLayout, kOriginalLayoutPath,
            "parts-exchange caller keeps the original 1364 layout");

        wchar_t equalButForeign[] = L"uirs\\1364.uir";
        CheckLayoutCall(
            g_serviceCaller, equalButForeign,
            static_cast<std::uint32_t>(kLayoutLength),
            equalButForeign, kOriginalLayoutPath,
            "friend caller with a noncanonical path pointer is forwarded");
        CheckLayoutCall(
            g_popupCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength - 1),
            g_originalLayout, L"uirs\\1364.ui",
            "friend caller with a mismatched length is forwarded");

        CheckGate(Row(1001, 'A', 7, 1), true, false,
                  "online friend still opens through the native branch");
        CheckGate(Row(1001, 'A', 7, 3), true, false,
                  "in-room friend still opens through the native branch");
        CheckGate(Row(1001, 'A', 7, 0), true, true,
                  "accepted offline friend opens after clearing only ZF");
        CheckGate(Row(0, 'A', 7, 0), false, false,
                  "zero-identity category row stays blocked");
        CheckGate(Row(1001, 0, 7, 0), false, false,
                  "blank-name row stays blocked");
        CheckGate(Row(1001, 'A', 2, 0), false, false,
                  "non-accepted relationship stays blocked");

        const bool otherAfter = InstallOtherAssignOwner();
        Check(otherAfter,
              "independent global assign owner installs after the friend Mod");
        if (otherAfter) {
            CheckOtherOwnerCall(
                g_serviceCaller, g_originalLayout,
                static_cast<std::uint32_t>(kLayoutLength),
                nullptr, kFriendLayoutPath,
                "friend redirect still works when global assign is hooked later");
            CheckOtherOwnerCall(
                g_exchangeCaller, g_originalLayout,
                static_cast<std::uint32_t>(kLayoutLength),
                g_originalLayout, kOriginalLayoutPath,
                "unrelated native caller still reaches the later global assign owner");
            Check(RemoveHook(g_assignCode),
                  "later global assign owner hook removed cleanly");
        }
        Check(RemoveFriendHooks(),
              "friend callsite and offline gate hooks removed cleanly");
    }
    if (initialized == MH_OK) {
        Check(MH_Uninitialize() == MH_OK,
              "friend-first MinHook fixture uninitialized cleanly");
    }

    initialized = MH_Initialize();
    Check(initialized == MH_OK,
          "MinHook initialized for global-owner-first compatibility fixtures");
    const bool otherBefore =
        initialized == MH_OK && InstallOtherAssignOwner();
    Check(otherBefore,
          "independent global assign owner installs before the friend Mod");
    ResetRequestReceipts();
    const bool activeAfterOther = otherBefore &&
        SdgoModLoad(&api, reinterpret_cast<SdgoMod*>(1)) != 0;
    Check(activeAfterOther && AllFriendReceipts(),
          "friend Mod activates without claiming the already-hooked global assign");
    if (activeAfterOther) {
        CheckOtherOwnerCall(
            g_serviceCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            nullptr, kFriendLayoutPath,
            "friend redirect works with a pre-existing global assign owner");
        const wchar_t* replacementPointer = g_assignedText;
        CheckOtherOwnerCall(
            g_popupCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            replacementPointer, kFriendLayoutPath,
            "both scoped friend calls reach the pre-existing global owner");
        CheckOtherOwnerCall(
            g_exchangeCaller, g_originalLayout,
            static_cast<std::uint32_t>(kLayoutLength),
            g_originalLayout, kOriginalLayoutPath,
            "unrelated caller remains native with the pre-existing global owner");
        Check(RemoveFriendHooks(),
              "second friend hook set removed cleanly");
    }
    if (otherBefore) {
        Check(RemoveHook(g_assignCode),
              "pre-existing global assign owner hook removed cleanly");
    }
    if (initialized == MH_OK) {
        Check(MH_Uninitialize() == MH_OK,
              "global-owner-first MinHook fixture uninitialized cleanly");
    }

    if (argc == 2) CheckClientImage(argv[1]);

    VirtualFree(g_gateCode, 0, MEM_RELEASE);
    VirtualFree(g_serviceCaller.code, 0, MEM_RELEASE);
    VirtualFree(g_popupCaller.code, 0, MEM_RELEASE);
    VirtualFree(g_exchangeCaller.code, 0, MEM_RELEASE);
    std::printf("%d FRIEND LIST CLIENT CHECKS, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
