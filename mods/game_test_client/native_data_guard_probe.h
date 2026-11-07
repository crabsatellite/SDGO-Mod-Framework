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
    using StatusV4 = int(__cdecl*)(SdgoNativeDataGuardStatusV4*);
    using StatusV5 = int(__cdecl*)(SdgoNativeDataGuardStatusV5*);
    using Material = void*(__thiscall*)(void*, int, uint8_t, uint8_t);
    using CameraSet = void(__thiscall*)(void*, int32_t);
    using CameraTick = void(__thiscall*)(void*);
    using Funnel = void*(__thiscall*)(void*, void*, int, int);
    using Format = void*(__thiscall*)(void*, void*, uint32_t, uint32_t, uint8_t);
    using InformationRefresh = void(__thiscall*)(void*);
    struct NativeString {
        wchar_t text[8]{};
        uint32_t length = 0, capacity = 7, reserved = 0;
    };
    static_assert(sizeof(void*) == 4);
    static_assert(sizeof(NativeString) == 28);
    struct CaptionCapture {
        std::array<wchar_t, 256> text{};
        uint32_t length = 0;
        bool called = false, valid = false;
    };
    struct CaptionControl {
        void** table = nullptr;
        CaptionCapture* capture = nullptr;
    };
    static_assert(sizeof(CaptionControl) == 8);
    bool done_ = false;

    static bool ExerciseLoading(const SdgoApi* api, SdgoMod* mod, StatusV5 status,
                                const SdgoGameTestContextV1* context) {
        const auto material = reinterpret_cast<Material>(
            api->Symbol(mod, "test.native_guard_material_lookup"));
        const auto cameraSet = reinterpret_cast<CameraSet>(
            api->Symbol(mod, "test.native_guard_camera_set_index"));
        const auto cameraTick = reinterpret_cast<CameraTick>(
            api->Symbol(mod, "test.native_guard_camera_update"));
        SdgoNativeDataGuardStatusV5 before{}; before.size = sizeof(before);
        if (!material || !cameraSet || !cameraTick || !status(&before)) return false;
        std::array<uint8_t, 0x180> model{};
        const auto emptyModel = model;
        bool valid = material(model.data(), 0, 0, 0) == nullptr &&
                     material(model.data(), 7, 0, 1) == nullptr && model == emptyModel;
        std::array<uint8_t, 0x70> firstMaterial{}, secondMaterial{};
        std::array<void*, 2> slots{firstMaterial.data(), secondMaterial.data()};
        for (size_t offset : {size_t{0x154}, size_t{0x164}}) {
            Put(model.data(), offset, slots.data());
            Put(model.data(), offset + 4, slots.data() + slots.size());
            Put(model.data(), offset + 8, slots.data() + slots.size());
        }
        const auto readyModel = model;
        valid = valid && material(model.data(), -1, 0, 0) == nullptr &&
            material(model.data(), 2, 0, 0) == nullptr &&
            material(model.data(), 9, 0, 1) == nullptr &&
            material(model.data(), 1, 0, 0) == slots[1] &&
            material(model.data(), 0, 0, 1) == slots[0] && model == readyModel;
        std::array<uint8_t, 0x4444> privateSelection{};
        Put(privateSelection.data(), 0x4440, int32_t{9});
        const auto priorSelection = privateSelection;
        for (int32_t index : {INT32_MIN, -1, 30, INT32_MAX}) {
            cameraSet(privateSelection.data(), index);
            valid = valid && privateSelection == priorSelection;
        }
        cameraSet(privateSelection.data(), 29);
        auto expectedSelection = priorSelection;
        Put(expectedSelection.data(), 0x4440, int32_t{29});
        valid = valid && privateSelection == expectedSelection;
        std::array<uint8_t, 0x206A> privateRoom{};
        const auto roomBefore = privateRoom;
        cameraTick(privateRoom.data());
        valid = valid && privateRoom == roomBefore;
        SdgoNativeDataGuardStatusV5 after{}; after.size = sizeof(after);
        valid = valid && status(&after) &&
            after.unavailable_material_slots >= before.unavailable_material_slots + 5 &&
            after.rejected_camera_indices >= before.rejected_camera_indices + 4 &&
            after.skipped_camera_updates >= before.skipped_camera_updates + 1;
        context->Assert(valid,
            "genuine native material and camera entries reject private incomplete data and recover valid inputs");
        if (valid) context->Observe(
            "native loading guards accepted empty/short materials and invalid camera indices without changing live game data");
        return valid;
    }

    static bool InvokeLoading(const SdgoApi* api, SdgoMod* mod, StatusV5 status,
                              const SdgoGameTestContextV1* context) {
        __try { return ExerciseLoading(api, mod, status, context); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            context->Assert(0, "loading guard probes must not enter unchecked native reads");
            return false;
        }
    }

    static bool ExerciseBase(InformationRefresh refresh, StatusV4 status,
                             const SdgoGameTestContextV1* context) {
        SdgoNativeDataGuardStatusV4 before{}; before.size = sizeof(before);
        if (!status(&before)) return false;
        std::array<uint8_t, 0x120> page{};
        Put(page.data(), 0x114, uint32_t{0xFFFFFFFEu});
        for (uint8_t transform : {uint8_t{0}, uint8_t{1}}) {
            page[0x11C] = transform;
            const auto saved = page;
            refresh(page.data());
            if (page != saved) {
                context->Assert(0, "missing base information leaves the private page unchanged");
                return false;
            }
        }
        SdgoNativeDataGuardStatusV4 after{}; after.size = sizeof(after);
        const bool guarded = status(&after) && after.missing_base_information_records >=
            before.missing_base_information_records + 2;
        context->Assert(guarded,
            "genuine whole-page entry safely rejects missing primary records in normal and transform views");
        return guarded;
    }
    static bool InvokeBase(InformationRefresh refresh, StatusV4 status,
                           const SdgoGameTestContextV1* context) {
        __try { return ExerciseBase(refresh, status, context); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            context->Assert(0, "missing base information must not reach native null detail readers");
            return false;
        }
    }

    template<class T> static void Put(uint8_t* data, size_t offset, T value) {
        std::memcpy(data + offset, &value, sizeof(value));
    }
    static void __fastcall CaptureCaption(void* self, void*, const NativeString* value) {
        auto* control = static_cast<CaptionControl*>(self);
        CaptionCapture* capture = control ? control->capture : nullptr;
        if (!capture) return;
        capture->called = true;
        if (!value || value->length >= capture->text.size() ||
            value->capacity < value->length) return;
        const wchar_t* source = value->text;
        if (value->capacity >= 8)
            std::memcpy(&source, value->text, sizeof(source));
        if (!source) return;
        std::wmemcpy(capture->text.data(), source, value->length);
        capture->text[value->length] = L'\0';
        capture->length = value->length;
        capture->valid = true;
    }
    static bool Invoke(Funnel funnel, Format format, InformationRefresh information,
                       Status status, StatusV3 statusV3,
                       const SdgoGameTestContextV1* context) {
        __try { return Exercise(funnel, format, information, status, statusV3, context); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            context->Assert(0, "native data guard primitives must not raise a host exception");
            return false;
        }
    }
    static bool Exercise(Funnel funnel, Format format, InformationRefresh information,
                         Status status, StatusV3 statusV3,
                         const SdgoGameTestContextV1* context) {
        SdgoNativeDataGuardStatusV1 before{}; before.size = sizeof(before);
        if (!status(&before) || !before.funnel_bounds_corrected) {
            context->Assert(0, "production native data guard must report the corrected index branch"); return false;
        }
        SdgoNativeDataGuardStatusV3 lifecycleBefore{};
        lifecycleBefore.size = sizeof(lifecycleBefore);
        if (!statusV3(&lifecycleBefore)) {
            context->Assert(0, "production native data guard must expose its V3 lifecycle observer"); return false;
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
        // The shipped information consumer reads only these page fields. Its
        // tail calls caption vt+0x88 and writes the private gauge through the
        // verified +0x2C -> +0x1A4 -> [0] -> [0] link.
        alignas(16) std::array<uint8_t, 0x11C> informationPage{};
        alignas(16) std::array<uint8_t, 0x1A8> privateUiRoot{};
        alignas(16) std::array<uint8_t, 0x140> privateGauge{};
        std::array<void*, 35> captionTable{};
        CaptionCapture captionCapture{};
        CaptionControl caption{captionTable.data(), &captionCapture};
        captionTable[0x88 / sizeof(void*)] = reinterpret_cast<void*>(&CaptureCaption);
        void* gauge = privateGauge.data();
        void* gaugeHolder = &gauge;
        Put(privateUiRoot.data(), 0x1A4, &gaugeHolder);
        Put(informationPage.data(), 0x2C, privateUiRoot.data());
        Put(informationPage.data(), 0x54, &caption);
        Put(informationPage.data(), 0x74, &caption);
        Put(informationPage.data(), 0x78, uint32_t{0xFFFFFFFEu});
        Put(informationPage.data(), 0x88, uint32_t{2});
        Put(informationPage.data(), 0xFC, uint32_t{37});
        Put(informationPage.data(), 0x118, uint32_t{1});
        const auto pageBefore = informationPage;
        const auto rootBefore = privateUiRoot;
        const auto tableBefore = captionTable;
        const auto captionBefore = caption;
        information(informationPage.data());
        SdgoNativeDataGuardStatusV3 informationAfter{};
        informationAfter.size = sizeof(informationAfter);
        const bool informationObserved = statusV3(&informationAfter) &&
            informationAfter.missing_information_records >=
                lifecycleBefore.missing_information_records + 1;
        std::array<uint8_t, 0x140> expectedGauge{};
        Put(expectedGauge.data(), 0x12E, uint16_t{1});
        Put(expectedGauge.data(), 0x134, uint32_t{1});
        Put(expectedGauge.data(), 0x138, 1.0f);
        Put(expectedGauge.data(), 0x13C, 1.0f);
        const bool defaultGauge =
            !std::memcmp(privateGauge.data(), expectedGauge.data(), privateGauge.size());
        const bool privateInputsPreserved =
            !std::memcmp(informationPage.data(), pageBefore.data(), informationPage.size()) &&
            !std::memcmp(privateUiRoot.data(), rootBefore.data(), privateUiRoot.size()) &&
            !std::memcmp(captionTable.data(), tableBefore.data(),
                         captionTable.size() * sizeof(captionTable[0])) &&
            !std::memcmp(&caption, &captionBefore, sizeof(caption)) &&
            gauge == privateGauge.data() && gaugeHolder == &gauge;
        context->Assert(informationObserved,
            "genuine host information refresh records its missing derived-data guard");
        context->Assert(defaultGauge,
            "genuine host information refresh retains the native default 100 percent gauge");
        context->Assert(captionCapture.called && captionCapture.valid &&
                        captionCapture.length && captionCapture.text[0],
            "genuine host information refresh produces a non-empty caption through its native SetText call");
        context->Assert(privateInputsPreserved,
            "genuine host information refresh leaves the private page, UI links and callback table unchanged");
        SdgoNativeDataGuardStatusV3 lifecycle{}; lifecycle.size = sizeof(lifecycle);
        const bool lifecycleReady = statusV3(&lifecycle) &&
            lifecycle.deferred_message_copy_failures == 0;
        context->Assert(lifecycleReady,
            "native lifecycle observer is ready and has no deferred-message copy failure");
        context->Observe("native data guard host primitives accepted; normal authenticated battle flow follows");
        context->Observe("native information refresh accepted private display receivers and the default gauge");
        context->Observe("unit-management GameTest may compare V3 copy, completion and outstanding counters");
        return observed && informationObserved && defaultGauge &&
            captionCapture.called && captionCapture.valid && captionCapture.length &&
            privateInputsPreserved && lifecycleReady;
    }
public:
    bool Run(const SdgoApi* api, SdgoMod* mod, const SdgoGameTestContextV1* context) {
        if (done_) return true;
        const auto module = GetModuleHandleW(L"native_data_guard.dll");
        if (!module) return true;
        const auto status = reinterpret_cast<Status>(GetProcAddress(module, "SdgoNativeDataGuardGetStatusV1"));
        const auto statusV3 = reinterpret_cast<StatusV3>(
            GetProcAddress(module, "SdgoNativeDataGuardGetStatusV3"));
        const auto statusV4 = reinterpret_cast<StatusV4>(
            GetProcAddress(module, "SdgoNativeDataGuardGetStatusV4"));
        const auto statusV5 = reinterpret_cast<StatusV5>(
            GetProcAddress(module, "SdgoNativeDataGuardGetStatusV5"));
        const auto baseInformation = reinterpret_cast<InformationRefresh>(
            api->Symbol(mod, "test.native_guard_base_information_refresh"));
        const auto funnel = reinterpret_cast<Funnel>(api->Symbol(mod, "test.native_guard_funnel"));
        const auto format = reinterpret_cast<Format>(api->Symbol(mod, "test.native_guard_name_formatter"));
        const auto information = reinterpret_cast<InformationRefresh>(
            api->Symbol(mod, "test.native_guard_information_refresh"));
        if (!status || !statusV3 || !statusV4 || !baseInformation || !funnel || !format || !information) {
            context->Assert(0, "native guard acceptance requires its observer and verified host bindings"); return false;
        }
        done_ = (!statusV5 || InvokeLoading(api, mod, statusV5, context)) &&
            InvokeBase(baseInformation, statusV4, context) &&
            Invoke(funnel, format, information, status, statusV3, context);
        return done_;
    }
};
