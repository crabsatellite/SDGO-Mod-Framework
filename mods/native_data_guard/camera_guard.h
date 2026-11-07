#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"

namespace sdgo::native_camera_guard {

// The native wait-room slider selects 29 - position; the drag calculation
// divides its extent into 30 samples before indexing this same camera table.
constexpr std::int32_t kCameraSamples = 30;
constexpr std::size_t kCameraModel = 0x2066;
using PresentationData = void*(__cdecl*)();
using Index = std::int32_t(__thiscall*)(void*);
using SetIndex = void(__thiscall*)(void*, std::int32_t);
using Update = void(__thiscall*)(void*);

inline const SdgoApi* api_ = nullptr;
inline SdgoMod* mod_ = nullptr;
inline PresentationData data_ = nullptr;
inline Index index_ = nullptr;
inline SetIndex set_ = nullptr;
inline Update update_ = nullptr;
inline volatile LONG rejected_indices_ = 0;
inline volatile LONG skipped_updates_ = 0;

inline bool Valid(std::int32_t index) {
    return index >= 0 && index < kCameraSamples;
}

inline void __fastcall Set(void* self, void*, std::int32_t index) {
    if (Valid(index)) {
        set_(self, index);
        return;
    }
    const DWORD error = GetLastError();
    if (InterlockedIncrement(&rejected_indices_) <= 16) {
        api_->Log(mod_, SDGO_LOG_WARN,
                  "wait-room camera sample %ld is outside 0..29; prior native selection retained",
                  static_cast<long>(index));
    }
    SetLastError(error);
}

inline void __fastcall Tick(void* self, void*) {
    const DWORD error = GetLastError();
    void* data = data_();
    void* model = nullptr;
    if (self) {
        std::memcpy(&model, static_cast<const std::uint8_t*>(self) + kCameraModel,
                    sizeof(model));
    }
    if (!data || !Valid(index_(data)) || !model) {
        if (InterlockedIncrement(&skipped_updates_) <= 16) {
            api_->Log(mod_, SDGO_LOG_WARN,
                      "wait-room camera data is not ready; awaiting a later native frame");
        }
        SetLastError(error);
        return;
    }
    SetLastError(error);
    update_(self);
}

inline bool Attach(const SdgoApi* api, SdgoMod* mod) {
    data_ = reinterpret_cast<PresentationData>(
        api->Symbol(mod, "native_data_guard.presentation_data"));
    index_ = reinterpret_cast<Index>(
        api->Symbol(mod, "native_data_guard.camera_index"));
    if (!data_ || !index_ ||
        !api->Symbol(mod, "native_data_guard.camera_set_index") ||
        !api->Symbol(mod, "native_data_guard.camera_update")) return false;
    api_ = api;
    mod_ = mod;
    return api->HookCode(mod, "native_data_guard.camera_set_index",
                         reinterpret_cast<void*>(&Set), reinterpret_cast<void**>(&set_)) &&
           api->HookCode(mod, "native_data_guard.camera_update",
                         reinterpret_cast<void*>(&Tick), reinterpret_cast<void**>(&update_));
}

inline std::uint32_t RejectedIndices() {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(&rejected_indices_, 0, 0));
}
inline std::uint32_t SkippedUpdates() {
    return static_cast<std::uint32_t>(InterlockedCompareExchange(&skipped_updates_, 0, 0));
}

} // namespace sdgo::native_camera_guard
