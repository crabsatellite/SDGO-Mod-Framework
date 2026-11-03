#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <iterator>

#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

// Isolated visual GameTest input adapter. This supplies a virtual pointer only
// inside the owned test process; it does not claim hardware-mouse evidence.
//
// The native path remains intact:
//   zoa_input!CMouse::Scan -> GetDeviceState/GetCursorPos
//   GOnline WM_MOUSEMOVE -> native UI hit testing -> shop tooltip packets
//
// Install only for the explicit virtual-input test scope. The adapter never
// calls SetCursorPos/SendInput, never selects a foreground window, never writes
// game UI hover fields, and never creates a tooltip packet or rendered pixel.
class ShopPointerInput {
public:
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        if (installed_) return true;
        if (!api || !mod || api->version != SDGO_API_VERSION ||
            api->size < offsetof(SdgoApi, HookSystemExport) + sizeof(api->HookSystemExport) ||
            !api->HookSystemExport || (active_ && active_ != this)) {
            return false;
        }
#if !defined(_M_IX86)
        return false;
#else
        if (!BindInput()) return false;

        api_ = api;
        mod_ = mod;
        active_ = this;
        if (!api->HookSystemExport(
                mod, L"user32.dll", "GetCursorPos", reinterpret_cast<void*>(&GetCursorPosHook),
                reinterpret_cast<void**>(&originalGetCursorPos_)) ||
            !originalGetCursorPos_) {
            active_ = nullptr;
            api_ = nullptr;
            mod_ = nullptr;
            originalGetCursorPos_ = nullptr;
            return false;
        }

        PublishPoint({OffscreenScreenCoordinate, OffscreenScreenCoordinate});
        installed_ = true;
        if (api_->Log) {
            api_->Log(mod_, SDGO_LOG_INFO,
                      "shop pointer virtual input ready: binding=%s image=%08X/%08X",
                      bindingName_, bindingTimeDateStamp_, bindingSizeOfImage_);
        }
        return true;
#endif
    }

    bool MoveToRow(uint32_t row) {
        if (!installed_ || row >= RowCount) return false;

        HWND window = FindOwnedGameWindow();
        if (!window || !EnsureDevicePatched()) return false;

        RECT client{};
        if (!GetClientRect(window, &client)) return false;
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) return false;

        POINT clientPoint{
            MulDiv(DesignLeft + static_cast<int>(row % Columns) * DesignColumnStep,
                   width, DesignWidth),
            MulDiv(DesignTop + static_cast<int>(row / Columns) * DesignRowStep,
                   height, DesignHeight)};
        if (clientPoint.x < 0 || clientPoint.y < 0 ||
            clientPoint.x >= width || clientPoint.y >= height) {
            return false;
        }

        POINT screenPoint = clientPoint;
        if (!ClientToScreen(window, &screenPoint)) return false;
        PublishPoint(screenPoint);

        const LPARAM packed = PackClientPoint(clientPoint.x, clientPoint.y);
        if (!PostMessageW(window, WM_MOUSEMOVE, 0, packed)) {
            SetOffscreen(window);
            return false;
        }
        return true;
    }

    void Clear() {
        if (!installed_) return;
        HWND window = ValidateOwnedWindow(ownedWindow_) ? ownedWindow_ : FindOwnedGameWindow();
        SetOffscreen(window);
    }

    uint32_t PositionReads() const {
        return ReadCounter(positionReads_);
    }

    uint32_t StateReads() const {
        return ReadCounter(stateReads_);
    }

private:
    static constexpr uint32_t RowCount = 8;
    static constexpr uint32_t Columns = 4;
    static constexpr int DesignWidth = 1024;
    static constexpr int DesignHeight = 768;
    static constexpr int DesignLeft = 430;
    static constexpr int DesignTop = 270;
    static constexpr int DesignColumnStep = 157;
    static constexpr int DesignRowStep = 237;
    static constexpr int OffscreenCoordinate = -64;
    static constexpr int OffscreenScreenCoordinate = -32768;

    static constexpr DWORD ZoaTimeDateStamp = 0x4E523963;
    static constexpr DWORD ZoaSizeOfImage = 0x00012000;
    static constexpr DWORD ZoaSingletonRva = 0x0000E330;
    static constexpr DWORD ZoaGetMouseRva = 0x000052F0;
    static constexpr DWORD ZoaLazyGetInstanceRva = 0x00005C50;
    static constexpr DWORD ZoaMousePollRva = 0x00005370;
    static constexpr DWORD ZoaMousePollSize = 0x000000FC;
    static constexpr DWORD ZoaCursorCallRva = 0x000053CB;
    static constexpr DWORD ZoaCursorReturnRva = 0x000053D1;
    static constexpr DWORD ZoaCursorIatRva = 0x000091B8;
    static constexpr DWORD ZoaScreenCallRva = 0x000053D3;
    static constexpr DWORD ZoaScreenIatRva = 0x000091B4;

    static constexpr DWORD GOnlinePreferredBase = 0x00400000;
    static constexpr DWORD GOnlineTimeDateStamp = 0x54BE1568;
    static constexpr DWORD GOnlineSizeOfImage = 0x00C97000;
    static constexpr DWORD GOnlineEntryPointRva = 0x0087AF55;
    static constexpr DWORD GOnlineChecksum = 0x005A78EF;
    static constexpr DWORD GOnlineSingletonRva = 0x00C0DC60;
    static constexpr DWORD GOnlineSingletonResetRva = 0x00847130;
    static constexpr DWORD GOnlineInputConstructorRva = 0x00847140;
    static constexpr DWORD GOnlineInputScanRva = 0x00847180;
    static constexpr DWORD GOnlineInputInitializeRva = 0x008471B0;
    static constexpr DWORD GOnlineDirectInputCallRva = 0x008471CE;
    static constexpr DWORD GOnlineDirectInputThunkRva = 0x0097E540;
    static constexpr DWORD GOnlineDirectInputIatRva = 0x00A2D08C;
    static constexpr DWORD GOnlineMousePollRva = 0x00846680;
    static constexpr DWORD GOnlineMousePollSize = 0x000000F8;
    static constexpr DWORD GOnlineCursorCallRva = 0x008466DE;
    static constexpr DWORD GOnlineCursorReturnRva = 0x008466E4;
    static constexpr DWORD GOnlineCursorIatRva = 0x00A2D8B8;
    static constexpr DWORD GOnlineScreenCallRva = 0x008466E6;
    static constexpr DWORD GOnlineScreenIatRva = 0x00A2D734;

    static constexpr size_t InputMouseOffset = 8;
    static constexpr uint32_t DeviceVtableSlots = 32;
    static constexpr uint32_t GetDeviceStateSlot = 9;

    static constexpr const char* SingletonExport =
        "?m_pSingleton@?$CSingleton@VCInput@@@@0PAVCInput@@A";
    static constexpr const char* GetMouseExport =
        "?GetMouse@CInput@@QAEPAVCMouse@@XZ";
    static constexpr const char* LazyGetInstanceExport =
        "?GetInstancePtr@?$CSingleton@VCInput@@@@SAPAVCInput@@XZ";

    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);

    struct ModuleRange {
        uintptr_t begin = 0;
        uintptr_t end = 0;

        bool Contains(const void* address) const {
            const uintptr_t value = reinterpret_cast<uintptr_t>(address);
            return value >= begin && value < end;
        }
    };

    struct WindowSearch {
        HWND found = nullptr;
        bool ambiguous = false;
    };

    static uint32_t ReadCounter(const volatile LONG& value) {
        return static_cast<uint32_t>(
            InterlockedCompareExchange(const_cast<volatile LONG*>(&value), 0, 0));
    }

    static LPARAM PackClientPoint(int x, int y) {
        const uint32_t low = static_cast<uint16_t>(static_cast<int16_t>(x));
        const uint32_t high =
            static_cast<uint32_t>(static_cast<uint16_t>(static_cast<int16_t>(y))) << 16;
        return static_cast<LPARAM>(low | high);
    }

    template <size_t Size>
    static bool MatchBytes(const BYTE* base, DWORD rva, const BYTE (&expected)[Size]) {
        return std::memcmp(base + rva, expected, Size) == 0;
    }

    static bool IsIndirectCallToIat(const BYTE* base, DWORD callRva, DWORD iatRva) {
        const BYTE* instruction = base + callRva;
        if (instruction[0] != 0xFF || instruction[1] != 0x15) return false;

        DWORD operand = 0;
        std::memcpy(&operand, instruction + 2, sizeof(operand));
        return operand == static_cast<DWORD>(
                              reinterpret_cast<uintptr_t>(base + iatRva));
    }

    static bool IsRelativeCallTo(const BYTE* base, DWORD callRva, DWORD targetRva) {
        const BYTE* instruction = base + callRva;
        if (instruction[0] != 0xE8) return false;

        int32_t displacement = 0;
        std::memcpy(&displacement, instruction + 1, sizeof(displacement));
        return callRva + 5 + displacement == targetRva;
    }

    static bool IsIndirectJumpToIat(const BYTE* base, DWORD jumpRva, DWORD iatRva) {
        const BYTE* instruction = base + jumpRva;
        if (instruction[0] != 0xFF || instruction[1] != 0x25) return false;

        DWORD operand = 0;
        std::memcpy(&operand, instruction + 2, sizeof(operand));
        return operand == static_cast<DWORD>(
                              reinterpret_cast<uintptr_t>(base + iatRva));
    }

    static bool HasGOnlineIdentity(HMODULE module) {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        if (reinterpret_cast<uintptr_t>(base) != GOnlinePreferredBase) return false;

        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew >= 0x1000) {
            return false;
        }
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + static_cast<size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.NumberOfSections != 7 ||
            nt->FileHeader.TimeDateStamp != GOnlineTimeDateStamp ||
            nt->FileHeader.Characteristics != 0x010F ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.ImageBase != GOnlinePreferredBase ||
            nt->OptionalHeader.AddressOfEntryPoint != GOnlineEntryPointRva ||
            nt->OptionalHeader.SizeOfImage != GOnlineSizeOfImage ||
            nt->OptionalHeader.CheckSum != GOnlineChecksum ||
            nt->OptionalHeader.DllCharacteristics != 0x8000) {
            return false;
        }
        return true;
    }

    bool BindInput() {
        HMODULE executable = GetModuleHandleW(nullptr);
        if (executable && HasGOnlineIdentity(executable)) {
            return BindGOnlineInput(executable);
        }

        HMODULE zoa = GetModuleHandleW(L"zoa_input.dll");
        return zoa && BindZoaInput(zoa);
    }

    bool BindGOnlineInput(HMODULE module) {
        if (!HasGOnlineIdentity(module)) return false;
        const auto* base = reinterpret_cast<const BYTE*>(module);

        static constexpr BYTE ExpectedSingletonReset[] = {
            0xC7, 0x05, 0x60, 0xDC, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xC3};
        static constexpr BYTE ExpectedInputConstructor[] = {
            0x56, 0x8B, 0xF1, 0x68, 0x00, 0x01, 0x00, 0x00, 0x89, 0x35,
            0x60, 0xDC, 0x00, 0x01, 0x8D, 0x46, 0x14, 0x6A, 0x00, 0x50,
            0xC7, 0x06, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x46, 0x04, 0x00,
            0x00, 0x00, 0x00, 0xC7, 0x46, 0x08, 0x00, 0x00, 0x00, 0x00,
            0xE8, 0x43, 0x4C, 0x03, 0x00, 0x83, 0xC4, 0x0C, 0xC7, 0x46,
            0x0C, 0x00, 0x00, 0x00, 0x00, 0x8B, 0xC6, 0x5E, 0xC3};
        // The static build has no exported getter to bind. CInput::Scan reads
        // the GetMouse field through the exact "mov ecx,[esi+8]" sequence
        // before the sole mouse poll.
        static constexpr BYTE ExpectedInputScan[] = {
            0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x04, 0x85, 0xC9, 0x74, 0x05,
            0xE8, 0x11, 0xF3, 0xFF, 0xFF, 0x8B, 0x4E, 0x08, 0x85, 0xC9,
            0x74, 0x09, 0x8B, 0x46, 0x10, 0x50, 0xE8, 0xE1, 0xF4, 0xFF,
            0xFF, 0x8B, 0x4E, 0x0C, 0x5E, 0x85, 0xC9, 0x74, 0x05, 0xE9,
            0x84, 0xFD, 0xFF, 0xFF, 0xC3};
        static constexpr BYTE ExpectedInputInitializePrefix[] = {
            0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x0C, 0x56, 0x57, 0x8B, 0x7D,
            0x08, 0x6A, 0x00, 0x8B, 0xF1, 0x56, 0x68, 0xFC, 0x19, 0xF2,
            0x00, 0x68, 0x00, 0x08, 0x00, 0x00, 0x50, 0x89, 0x7E, 0x10};
        // CMouse+0 is the native DirectInput device. Slot 9 receives a 20-byte
        // DIMOUSESTATE2 buffer before the genuine cursor polling path.
        static constexpr BYTE ExpectedMouseStatePoll[] = {
            0x8B, 0x06, 0x8B, 0x08, 0x8D, 0x55, 0xE8, 0x52, 0x6A,
            0x14, 0x50, 0x8B, 0x41, 0x24, 0xFF, 0xD0, 0x85, 0xC0};
        static constexpr BYTE ExpectedMouseCursorPrefix[] = {
            0x8B, 0x46, 0x18, 0x8B, 0x4E, 0x1C, 0x57, 0x8D, 0x7E, 0x18,
            0x57, 0xC6, 0x46, 0x28, 0x00, 0x89, 0x46, 0x20, 0x89, 0x4E,
            0x24};
        static constexpr BYTE ExpectedAfterCursorCall[] = {0x57, 0x53};
        static constexpr BYTE ExpectedMousePollTail[] = {
            0x8B, 0x4D, 0xFC, 0x5E, 0x33, 0xCD, 0x5B, 0xE8, 0x22,
            0x56, 0x03, 0x00, 0x8B, 0xE5, 0x5D, 0xC2, 0x04, 0x00};

        if (!MatchBytes(base, GOnlineSingletonResetRva, ExpectedSingletonReset) ||
            !MatchBytes(base, GOnlineInputConstructorRva, ExpectedInputConstructor) ||
            !MatchBytes(base, GOnlineInputScanRva, ExpectedInputScan) ||
            !MatchBytes(base, GOnlineInputInitializeRva, ExpectedInputInitializePrefix) ||
            !IsRelativeCallTo(base, GOnlineDirectInputCallRva,
                              GOnlineDirectInputThunkRva) ||
            !IsIndirectJumpToIat(base, GOnlineDirectInputThunkRva,
                                 GOnlineDirectInputIatRva) ||
            !MatchBytes(base, GOnlineMousePollRva + 0x17, ExpectedMouseStatePoll) ||
            !MatchBytes(base, GOnlineMousePollRva + 0x49, ExpectedMouseCursorPrefix) ||
            !IsIndirectCallToIat(base, GOnlineCursorCallRva, GOnlineCursorIatRva) ||
            !MatchBytes(base, GOnlineCursorReturnRva, ExpectedAfterCursorCall) ||
            !IsIndirectCallToIat(base, GOnlineScreenCallRva, GOnlineScreenIatRva) ||
            !MatchBytes(base, GOnlineMousePollRva + 0xE6, ExpectedMousePollTail)) {
            return false;
        }

        singletonSlot_ =
            reinterpret_cast<void* volatile*>(const_cast<BYTE*>(base) + GOnlineSingletonRva);
        cursorCallerRange_ = {
            reinterpret_cast<uintptr_t>(base + GOnlineMousePollRva),
            reinterpret_cast<uintptr_t>(base + GOnlineMousePollRva + GOnlineMousePollSize)};
        cursorReturnAddress_ = base + GOnlineCursorReturnRva;
        bindingName_ = "gonline-static";
        bindingTimeDateStamp_ = GOnlineTimeDateStamp;
        bindingSizeOfImage_ = GOnlineSizeOfImage;
        return true;
    }

    bool BindZoaInput(HMODULE module) {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            dos->e_lfanew >= 0x1000) {
            return false;
        }
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + static_cast<size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.NumberOfSections != 4 ||
            nt->FileHeader.TimeDateStamp != ZoaTimeDateStamp ||
            nt->FileHeader.Characteristics != 0x210E ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.ImageBase != 0x10000000 ||
            nt->OptionalHeader.AddressOfEntryPoint != 0x000068BC ||
            nt->OptionalHeader.SizeOfImage != ZoaSizeOfImage ||
            nt->OptionalHeader.CheckSum != 0 ||
            nt->OptionalHeader.DllCharacteristics != 0) {
            return false;
        }

        FARPROC singleton = GetProcAddress(module, SingletonExport);
        FARPROC getMouse = GetProcAddress(module, GetMouseExport);
        FARPROC lazyGetInstance = GetProcAddress(module, LazyGetInstanceExport);
        if (!singleton || !getMouse || !lazyGetInstance ||
            reinterpret_cast<const BYTE*>(singleton) != base + ZoaSingletonRva ||
            reinterpret_cast<const BYTE*>(getMouse) != base + ZoaGetMouseRva ||
            reinterpret_cast<const BYTE*>(lazyGetInstance) != base + ZoaLazyGetInstanceRva) {
            return false;
        }

        static constexpr BYTE ExpectedGetMouse[] = {0x8B, 0x41, 0x08, 0xC3};
        static constexpr BYTE ExpectedMouseStatePoll[] = {
            0x8B, 0x06, 0x8B, 0x08, 0x6A, 0x14, 0x50, 0xFF, 0x51, 0x24,
            0x85, 0xC0};
        static constexpr BYTE ExpectedMouseCursorPrefix[] = {
            0x8B, 0x4E, 0x18, 0x8B, 0x56, 0x1C, 0x57, 0x8D, 0x7E, 0x18,
            0x57, 0xC6, 0x46, 0x28, 0x00, 0x89, 0x4E, 0x20, 0x89, 0x56,
            0x24};
        static constexpr BYTE ExpectedAfterCursorCall[] = {0x57, 0x55};
        static constexpr BYTE ExpectedMousePollTail[] = {
            0x8B, 0x4C, 0x24, 0x1C, 0x5E, 0x5D, 0x33, 0xCC, 0xE8, 0xEB,
            0x12, 0x00, 0x00, 0x83, 0xC4, 0x18, 0xC2, 0x04, 0x00};
        if (std::memcmp(reinterpret_cast<const void*>(getMouse), ExpectedGetMouse,
                        sizeof(ExpectedGetMouse)) != 0 ||
            !MatchBytes(base, ZoaMousePollRva + 0x1B, ExpectedMouseStatePoll) ||
            !MatchBytes(base, ZoaMousePollRva + 0x46, ExpectedMouseCursorPrefix) ||
            !IsIndirectCallToIat(base, ZoaCursorCallRva, ZoaCursorIatRva) ||
            !MatchBytes(base, ZoaCursorReturnRva, ExpectedAfterCursorCall) ||
            !IsIndirectCallToIat(base, ZoaScreenCallRva, ZoaScreenIatRva) ||
            !MatchBytes(base, ZoaMousePollRva + 0xE9, ExpectedMousePollTail)) {
            return false;
        }

        singletonSlot_ = reinterpret_cast<void* volatile*>(singleton);
        cursorCallerRange_ = {
            reinterpret_cast<uintptr_t>(base + ZoaMousePollRva),
            reinterpret_cast<uintptr_t>(base + ZoaMousePollRva + ZoaMousePollSize)};
        cursorReturnAddress_ = base + ZoaCursorReturnRva;
        bindingName_ = "zoa_input";
        bindingTimeDateStamp_ = ZoaTimeDateStamp;
        bindingSizeOfImage_ = ZoaSizeOfImage;
        return true;
    }

    bool EnsureDevicePatched() {
        if (InterlockedCompareExchange(&devicePatched_, 0, 0) != 0) return true;

        AcquireSRWLockExclusive(&deviceLock_);
        if (devicePatched_ != 0) {
            ReleaseSRWLockExclusive(&deviceLock_);
            return true;
        }

        void* input = singletonSlot_
                          ? InterlockedCompareExchangePointer(singletonSlot_, nullptr, nullptr)
                          : nullptr;
        void* mouse =
            input ? *reinterpret_cast<void**>(static_cast<BYTE*>(input) + InputMouseOffset)
                  : nullptr;
        void* device = mouse ? *reinterpret_cast<void**>(mouse) : nullptr;
        void** originalTable = device ? *reinterpret_cast<void***>(device) : nullptr;
        if (!input || !mouse || !device || !originalTable ||
            !originalTable[GetDeviceStateSlot]) {
            ReleaseSRWLockExclusive(&deviceLock_);
            return false;
        }

        for (uint32_t i = 0; i < DeviceVtableSlots; ++i) {
            if (!originalTable[i]) {
                ReleaseSRWLockExclusive(&deviceLock_);
                return false;
            }
            deviceVtable_[i] = originalTable[i];
        }

        device_ = device;
        originalDeviceVtable_ = originalTable;
        originalGetDeviceState_ =
            reinterpret_cast<GetDeviceStateFn>(originalTable[GetDeviceStateSlot]);
        deviceVtable_[GetDeviceStateSlot] = reinterpret_cast<void*>(&GetDeviceStateHook);

        void* previous = InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(device_), deviceVtable_,
            originalDeviceVtable_);
        if (previous != originalDeviceVtable_) {
            device_ = nullptr;
            originalDeviceVtable_ = nullptr;
            originalGetDeviceState_ = nullptr;
            ReleaseSRWLockExclusive(&deviceLock_);
            return false;
        }

        InterlockedExchange(&devicePatched_, 1);
        ReleaseSRWLockExclusive(&deviceLock_);
        if (api_ && api_->Log) {
            api_->Log(mod_, SDGO_LOG_INFO,
                      "shop pointer native mouse adapter patched: device=%p state_slot=%u",
                      device_, GetDeviceStateSlot);
        }
        return true;
    }

    static BOOL CALLBACK EnumOwnedWindow(HWND window, LPARAM parameter) {
        auto* search = reinterpret_cast<WindowSearch*>(parameter);
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId()) return TRUE;

        wchar_t className[128]{};
        if (GetClassNameW(window, className, static_cast<int>(std::size(className))) <= 0) {
            return TRUE;
        }
        if (lstrcmpW(className, L"SDGO.GameTest.GraphicsProxy") == 0 ||
            lstrcmpW(className, L"_GONLINE_") != 0) {
            return TRUE;
        }

        if (search->found && search->found != window) {
            search->ambiguous = true;
        } else {
            search->found = window;
        }
        return TRUE;
    }

    HWND FindOwnedGameWindow() {
        if (ValidateOwnedWindow(ownedWindow_)) return ownedWindow_;

        WindowSearch search{};
        if (!EnumWindows(&EnumOwnedWindow, reinterpret_cast<LPARAM>(&search)) ||
            search.ambiguous || !search.found) {
            return nullptr;
        }
        ownedWindow_ = search.found;
        return ownedWindow_;
    }

    static bool ValidateOwnedWindow(HWND window) {
        if (!window || !IsWindow(window)) return false;
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        if (processId != GetCurrentProcessId()) return false;
        wchar_t className[64]{};
        return GetClassNameW(window, className, static_cast<int>(std::size(className))) > 0 &&
               lstrcmpW(className, L"_GONLINE_") == 0;
    }

    void PublishPoint(POINT point) {
        AcquireSRWLockExclusive(&pointLock_);
        fakePoint_ = point;
        ReleaseSRWLockExclusive(&pointLock_);
        InterlockedExchange(&virtualEnabled_, 1);
    }

    void SetOffscreen(HWND window) {
        POINT screenPoint{OffscreenScreenCoordinate, OffscreenScreenCoordinate};
        if (ValidateOwnedWindow(window)) {
            POINT clientPoint{OffscreenCoordinate, OffscreenCoordinate};
            screenPoint = clientPoint;
            ClientToScreen(window, &screenPoint);
            PublishPoint(screenPoint);
            PostMessageW(window, WM_MOUSEMOVE, 0,
                         PackClientPoint(clientPoint.x, clientPoint.y));
            return;
        }
        PublishPoint(screenPoint);
    }

    static BOOL WINAPI GetCursorPosHook(LPPOINT point) {
        ShopPointerInput* self = active_;
        if (!self || !self->originalGetCursorPos_) return FALSE;

        const void* returnAddress = _ReturnAddress();
        if (!self->cursorCallerRange_.Contains(returnAddress) ||
            returnAddress != self->cursorReturnAddress_ ||
            InterlockedCompareExchange(&self->virtualEnabled_, 0, 0) == 0) {
            return self->originalGetCursorPos_(point);
        }
        if (!point) return FALSE;

        AcquireSRWLockShared(&self->pointLock_);
        *point = self->fakePoint_;
        ReleaseSRWLockShared(&self->pointLock_);
        InterlockedIncrement(&self->positionReads_);
        return TRUE;
    }

    static HRESULT STDMETHODCALLTYPE
    GetDeviceStateHook(void* device, DWORD bytes, LPVOID data) {
        ShopPointerInput* self = active_;
        if (!self || !self->originalGetDeviceState_) return E_FAIL;
        if (device != self->device_ || (bytes != 16 && bytes != 20) || !data ||
            InterlockedCompareExchange(&self->virtualEnabled_, 0, 0) == 0) {
            return self->originalGetDeviceState_(device, bytes, data);
        }

        std::memset(data, 0, bytes);
        InterlockedIncrement(&self->stateReads_);
        return S_OK;
    }

    inline static ShopPointerInput* active_ = nullptr;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    GetCursorPosFn originalGetCursorPos_ = nullptr;
    void* volatile* singletonSlot_ = nullptr;
    ModuleRange cursorCallerRange_{};
    const void* cursorReturnAddress_ = nullptr;
    const char* bindingName_ = nullptr;
    DWORD bindingTimeDateStamp_ = 0;
    DWORD bindingSizeOfImage_ = 0;

    HWND ownedWindow_ = nullptr;
    void* device_ = nullptr;
    void** originalDeviceVtable_ = nullptr;
    GetDeviceStateFn originalGetDeviceState_ = nullptr;
    void* deviceVtable_[DeviceVtableSlots]{};

    bool installed_ = false;
    mutable SRWLOCK pointLock_ = SRWLOCK_INIT;
    mutable SRWLOCK deviceLock_ = SRWLOCK_INIT;
    POINT fakePoint_{OffscreenCoordinate, OffscreenCoordinate};
    volatile LONG virtualEnabled_ = 0;
    volatile LONG devicePatched_ = 0;
    volatile LONG positionReads_ = 0;
    volatile LONG stateReads_ = 0;
};
