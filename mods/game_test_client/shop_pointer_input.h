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
        HMODULE zoa = GetModuleHandleW(L"zoa_input.dll");
        if (!zoa || !BindZoaExports(zoa)) return false;

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
                      "shop pointer virtual input ready: zoa_input=%08X/%08X",
                      ZoaTimeDateStamp, ZoaSizeOfImage);
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
    static constexpr uint32_t DeviceVtableSlots = 32;
    static constexpr uint32_t GetDeviceStateSlot = 9;

    static constexpr const char* SingletonExport =
        "?m_pSingleton@?$CSingleton@VCInput@@@@0PAVCInput@@A";
    static constexpr const char* GetMouseExport =
        "?GetMouse@CInput@@QAEPAVCMouse@@XZ";
    static constexpr const char* LazyGetInstanceExport =
        "?GetInstancePtr@?$CSingleton@VCInput@@@@SAPAVCInput@@XZ";

    using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
    using GetMouseFn = void*(__thiscall*)(void*);
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

    bool BindZoaExports(HMODULE module) {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + static_cast<size_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt->FileHeader.TimeDateStamp != ZoaTimeDateStamp ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.SizeOfImage != ZoaSizeOfImage) {
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
        if (std::memcmp(reinterpret_cast<const void*>(getMouse), ExpectedGetMouse,
                        sizeof(ExpectedGetMouse)) != 0) {
            return false;
        }

        singletonSlot_ = reinterpret_cast<void* volatile*>(singleton);
        getMouse_ = reinterpret_cast<GetMouseFn>(getMouse);
        zoaRange_ = {reinterpret_cast<uintptr_t>(base),
                     reinterpret_cast<uintptr_t>(base) + ZoaSizeOfImage};
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
        void* mouse = input && getMouse_ ? getMouse_(input) : nullptr;
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

        if (!self->zoaRange_.Contains(_ReturnAddress()) ||
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
    GetMouseFn getMouse_ = nullptr;
    ModuleRange zoaRange_{};

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
