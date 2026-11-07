#pragma once

void* cameraSetHook = nullptr;
void* cameraUpdateHook = nullptr;
std::array<uint8_t, 0x4444> cameraProfile{};
bool cameraProfileAbsent = false;
unsigned nativeCameraSets = 0;
unsigned nativeCameraUpdates = 0;

void* __cdecl CameraData() {
    return cameraProfileAbsent ? nullptr : cameraProfile.data();
}
int32_t __fastcall CameraIndex(void* self, void*) {
    int32_t value = 0;
    std::memcpy(&value, static_cast<const uint8_t*>(self) + 0x4440, sizeof(value));
    return value;
}
void __fastcall OriginalCameraSet(void* self, void*, int32_t index) {
    ++nativeCameraSets;
    Put(self, 0x4440, index);
    SetLastError(0xCA01);
}
void __fastcall OriginalCameraUpdate(void*, void*) {
    ++nativeCameraUpdates;
    SetLastError(0xCA02);
}
void* CameraSymbol(const char* key) {
    if (!std::strcmp(key, "native_data_guard.presentation_data"))
        return reinterpret_cast<void*>(&CameraData);
    if (!std::strcmp(key, "native_data_guard.camera_index"))
        return reinterpret_cast<void*>(&CameraIndex);
    if (!std::strcmp(key, "native_data_guard.camera_set_index"))
        return reinterpret_cast<void*>(&OriginalCameraSet);
    if (!std::strcmp(key, "native_data_guard.camera_update"))
        return reinterpret_cast<void*>(&OriginalCameraUpdate);
    return nullptr;
}
bool CameraHook(const char* key, void* detour, void** original) {
    if (!std::strcmp(key, "native_data_guard.camera_set_index")) {
        cameraSetHook = detour;
        *original = reinterpret_cast<void*>(&OriginalCameraSet);
        return true;
    }
    if (!std::strcmp(key, "native_data_guard.camera_update")) {
        cameraUpdateHook = detour;
        *original = reinterpret_cast<void*>(&OriginalCameraUpdate);
        return true;
    }
    return false;
}

void ExerciseCameraGuards(HMODULE dll) {
    Check(cameraSetHook && cameraUpdateHook, "both production camera guards are installed");
    using Set = void(__fastcall*)(void*, void*, int32_t);
    using Update = void(__fastcall*)(void*, void*);
    auto set = reinterpret_cast<Set>(cameraSetHook);
    auto update = reinterpret_cast<Update>(cameraUpdateHook);
    Put(cameraProfile.data(), 0x4440, int32_t{9});
    const auto savedProfile = cameraProfile;
    for (int32_t index : {INT32_MIN, -1, 30, INT32_MAX}) {
        SetLastError(0xCACA);
        set(cameraProfile.data(), nullptr, index);
        Check(cameraProfile == savedProfile && !nativeCameraSets && GetLastError() == 0xCACA,
              "out-of-range camera selection preserves all native state and thread error");
    }
    for (int32_t index : {0, 14, 29}) {
        const auto before = nativeCameraSets;
        set(cameraProfile.data(), nullptr, index);
        auto expected = savedProfile;
        Put(expected.data(), 0x4440, index);
        Check(cameraProfile == expected && nativeCameraSets == before + 1 && GetLastError() == 0xCA01,
              "valid boundary/interior camera samples retain exactly one original setter call");
    }
    std::array<uint8_t, 0x206A> scene{};
    const auto emptyScene = scene;
    SetLastError(0xCACA);
    update(scene.data(), nullptr);
    Check(scene == emptyScene && !nativeCameraUpdates && GetLastError() == 0xCACA,
          "unloaded room model leaves its camera untouched until a later frame");
    Put(scene.data(), 0x2066, reinterpret_cast<void*>(uintptr_t{0x1234}));
    const auto loadedScene = scene;
    for (int32_t index : {INT32_MIN, -1, 30, INT32_MAX}) {
        Put(cameraProfile.data(), 0x4440, index);
        update(scene.data(), nullptr);
        Check(scene == loadedScene && !nativeCameraUpdates,
              "invalid camera data never reaches unchecked native table indexing");
    }
    Put(cameraProfile.data(), 0x4440, int32_t{29});
    cameraProfileAbsent = true;
    update(scene.data(), nullptr);
    Check(!nativeCameraUpdates, "absent presentation data cannot enter the camera update");
    cameraProfileAbsent = false;
    update(scene.data(), nullptr);
    Check(nativeCameraUpdates == 1 && scene == loadedScene && GetLastError() == 0xCA02,
          "a later ready frame resumes the genuine camera update exactly once");
    using Status = int(__cdecl*)(SdgoNativeDataGuardStatusV5*);
    auto status = reinterpret_cast<Status>(GetProcAddress(dll, "SdgoNativeDataGuardGetStatusV5"));
    SdgoNativeDataGuardStatusV5 value{};
    value.size = sizeof(value);
    Check(status && status(&value) && value.version == 5 &&
          value.rejected_camera_indices == 4 && value.skipped_camera_updates == 6,
          "V5 observes actual camera rejections and later frame recovery");
    value.size = sizeof(SdgoNativeDataGuardStatusV4);
    Check(!status(&value) && !status(nullptr), "V5 rejects earlier output sizes and null storage");
}
