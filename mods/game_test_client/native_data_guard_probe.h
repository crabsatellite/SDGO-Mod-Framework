#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "sdgo/game_test.h"
#include "sdgo/native_data_guard_observer.h"
#include "sdgo/sdgo_api.h"

// Runs only inside the explicitly enabled native GameTest observer. The owned
// objects exercise genuine host functions without editing a live unit or grid.
class NativeDataGuardProbe {
    using Status = int(__cdecl*)(SdgoNativeDataGuardStatusV1*);
    using StatusV3 = int(__cdecl*)(SdgoNativeDataGuardStatusV3*);
    using Funnel = void*(__thiscall*)(void*, void*, int, int);
    using Format = void*(__thiscall*)(void*, void*, uint32_t, uint32_t, uint8_t);
    struct NativeString {
        wchar_t text[8]{};
        uint32_t length = 0, capacity = 7, reserved = 0;
    };
    static_assert(sizeof(NativeString) == 28);
    bool done_ = false;

    template<class T> static void Put(uint8_t* data, size_t offset, T value) {
        std::memcpy(data + offset, &value, sizeof(value));
    }
    static bool Invoke(Funnel funnel, Format format, Status status, StatusV3 statusV3,
                       const SdgoGameTestContextV1* context) {
        __try { return Exercise(funnel, format, status, statusV3, context); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            context->Assert(0, "native data guard primitives must not raise a host exception");
            return false;
        }
    }
    static bool Exercise(Funnel funnel, Format format, Status status, StatusV3 statusV3,
                         const SdgoGameTestContextV1* context) {
        SdgoNativeDataGuardStatusV1 before{}; before.size = sizeof(before);
        if (!status(&before) || !before.funnel_bounds_corrected) {
            context->Assert(0, "production native data guard must report the corrected index branch"); return false;
        }
        std::array<uint8_t, 0xE50> object{};
        std::array<uint8_t, 0xE38> owner{};
        std::array<NativeString, 6> nodes{};
        Put(object.data(), 0xB2C, owner.data());
        for (size_t i = 0; i < nodes.size(); ++i) {
            nodes[i].text[0] = L'n'; nodes[i].text[1] = static_cast<wchar_t>(L'1' + i);
            nodes[i].length = 2;
        }
        for (int weapon = 0; weapon < 3; ++weapon) {
            object[0xE48 + weapon] = 1; object[0xE4B + weapon] = 1;
            Put(object.data(), 0xDE8 + weapon * 16, nodes.data());
            Put(object.data(), 0xE18 + weapon * 16, nodes.data());
        }
        unsigned valid = 0, rejected = 0;
        for (int alternate = 0; alternate < 2; ++alternate) {
            owner[0xE34] = static_cast<uint8_t>(alternate);
            for (int weapon = 0; weapon < 3; ++weapon) {
                for (int index = 1; index <= 6; ++index) {
                    NativeString output{};
                    funnel(object.data(), &output, weapon, index);
                    if (output.length != 2 || output.capacity != 7 || output.text[0] != L'n' ||
                        output.text[1] != static_cast<wchar_t>(L'0' + index)) {
                        context->Assert(0, "valid native funnel nodes retain their original text in both banks"); return false;
                    }
                    ++valid;
                }
                for (int index : {-1, 0, 7, 100}) {
                    NativeString output{};
                    funnel(object.data(), &output, weapon, index);
                    if (output.length || output.capacity != 7 || output.text[0]) {
                        context->Assert(0, "invalid native funnel indices return the native empty string"); return false;
                    }
                    ++rejected;
                }
            }
        }
        context->Assert(valid == 36 && rejected == 24,
            "genuine host funnel function preserves 36 valid cases and safely rejects 24 invalid cases");
        for (uint8_t style : {uint8_t{0}, uint8_t{1}}) {
            NativeString output{};
            format(nullptr, &output, 0xFFFFFFFEu, 2, style);
            if (!output.length || output.length > 7 || output.capacity != 7 || !std::wcsstr(output.text, L"未命名")) {
                context->Assert(0, "genuine host name formatter must survive unavailable data in both verified styles"); return false;
            }
        }
        SdgoNativeDataGuardStatusV1 after{}; after.size = sizeof(after);
        const bool observed = status(&after) && after.missing_names >= before.missing_names + 2;
        context->Assert(observed, "production observer records both real missing-name protections");
        SdgoNativeDataGuardStatusV3 lifecycle{}; lifecycle.size = sizeof(lifecycle);
        const bool lifecycleReady = statusV3(&lifecycle) &&
            lifecycle.deferred_message_copy_failures == 0;
        context->Assert(lifecycleReady,
            "native lifecycle observer is ready and has no deferred-message copy failure");
        context->Observe("native data guard host primitives accepted; normal authenticated battle flow follows");
        context->Observe("unit-management GameTest may compare V3 copy, completion and outstanding counters");
        return observed && lifecycleReady;
    }
public:
    bool Run(const SdgoApi* api, SdgoMod* mod, const SdgoGameTestContextV1* context) {
        if (done_) return true;
        const auto module = GetModuleHandleW(L"native_data_guard.dll");
        if (!module) return true;
        const auto status = reinterpret_cast<Status>(GetProcAddress(module, "SdgoNativeDataGuardGetStatusV1"));
        const auto statusV3 = reinterpret_cast<StatusV3>(
            GetProcAddress(module, "SdgoNativeDataGuardGetStatusV3"));
        const auto funnel = reinterpret_cast<Funnel>(api->Symbol(mod, "test.native_guard_funnel"));
        const auto format = reinterpret_cast<Format>(api->Symbol(mod, "test.native_guard_name_formatter"));
        if (!status || !statusV3 || !funnel || !format) {
            context->Assert(0, "native guard acceptance requires its observer and verified host bindings"); return false;
        }
        done_ = Invoke(funnel, format, status, statusV3, context);
        return done_;
    }
};
