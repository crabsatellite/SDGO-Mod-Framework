#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "capsule_runtime.h"
#include "../support/capsule_registry/capsule_display_number.h"

namespace
{
    struct AcquisitionCapture
    {
        std::unordered_map<std::string, std::wstring> values;
        bool reject = false;
    };

    int __cdecl CaptureAcquisition(void* context, const char* key,
                                   const wchar_t* text, uint32_t chars)
    {
        auto* capture = static_cast<AcquisitionCapture*>(context);
        if (!capture || capture->reject || !key || !text || chars == 0)
            return 0;
        capture->values[key] = std::wstring(text, chars);
        return 1;
    }

    bool CheckAcquisitionContribution(const capsule_catalog::Bundle& bundle)
    {
        uint32_t unit = 7654321;
        if (!bundle.capsuleAcquisition.units.empty())
            unit = bundle.capsuleAcquisition.units.begin()->first;
        else if (!bundle.acquisition.units.empty())
            unit = *bundle.acquisition.units.begin();
        AcquisitionCapture capture;
        SdgoAcquisitionRequestV1 request{sizeof(request), 1, SDGO_ACQUISITION_UNIT, unit};
        SdgoAcquisitionSinkV1 sink{sizeof(sink), 1, &capture, &CaptureAcquisition};
        if (capsule_catalog::EmitAcquisitionSources(bundle, &request, &sink) != SDGO_ACQUISITION_READY)
            return false;
        const auto capsule = bundle.capsuleAcquisition.units.find(unit);
        if (capsule != bundle.capsuleAcquisition.units.end() &&
            capture.values["capsule.catalog"] != capsule->second.display)
            return false;
        if (bundle.acquisition.units.count(unit) != 0 &&
            capture.values["capsule.collection"] != bundle.acquisition.text)
            return false;
        const size_t expected = (capsule != bundle.capsuleAcquisition.units.end() ? 1u : 0u) +
                                (bundle.acquisition.units.count(unit) != 0 ? 1u : 0u);
        if (capture.values.size() != expected)
            return false;
        if (expected != 0) {
            capture.reject = true;
            if (capsule_catalog::EmitAcquisitionSources(bundle, &request, &sink) !=
                SDGO_ACQUISITION_INCOMPLETE)
                return false;
        }
        capture.reject = false;
        request.subject = SDGO_ACQUISITION_BLUEPRINT;
        if (capsule_catalog::EmitAcquisitionSources(bundle, &request, &sink) !=
            SDGO_ACQUISITION_UNAVAILABLE)
            return false;
        request.subject = SDGO_ACQUISITION_UNIT;
        request.version = 2;
        return capsule_catalog::EmitAcquisitionSources(bundle, &request, &sink) ==
               SDGO_ACQUISITION_INCOMPLETE;
    }

    const wchar_t* RejectedModDirectory(SdgoMod*)
    {
        return L"Z:\\sdgo-capsule-provider-rejected-fixture";
    }

    void SilentLog(SdgoMod*, int, const char*, ...) {}

    bool CheckIdleAcquisitionProvider(const wchar_t* path)
    {
        const HMODULE module = LoadLibraryW(path);
        if (!module) return false;
        using Provider = uint32_t (__cdecl*)(const SdgoAcquisitionRequestV1*,
                                              const SdgoAcquisitionSinkV1*);
        const auto provider = reinterpret_cast<Provider>(
            GetProcAddress(module, "SdgoAcquisitionSourcesV1"));
        const auto load = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
        AcquisitionCapture capture;
        SdgoAcquisitionRequestV1 request{sizeof(request), 1, SDGO_ACQUISITION_UNIT, 1};
        SdgoAcquisitionSinkV1 sink{sizeof(sink), 1, &capture, &CaptureAcquisition};
        bool unavailable = provider && load &&
            provider(&request, &sink) == SDGO_ACQUISITION_UNAVAILABLE &&
            capture.values.empty();
        SdgoApi api{};
        api.size = sizeof(api);
        api.version = SDGO_API_VERSION;
        api.Log = &SilentLog;
        api.ModDirectory = &RejectedModDirectory;
        unavailable = unavailable &&
            load(&api, reinterpret_cast<SdgoMod*>(1)) == 0 &&
            provider(&request, &sink) == SDGO_ACQUISITION_UNAVAILABLE &&
            capture.values.empty();
        FreeLibrary(module);
        return unavailable;
    }

    bool CheckChecksumGate()
    {
        std::unordered_map<void*, std::wstring> markers;
        void* first = reinterpret_cast<void*>(1);
        void* second = reinterpret_cast<void*>(2);
        const std::wstring gdt0033 = L"gdrs\\0033.gdt";
        const std::wstring gdt0101 = L"gdrs\\0101.gdt";
        const std::wstring unapproved = L"gdrs\\9999.gdt";

        markers[first] = gdt0033;
        if (!capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0033) ||
            !markers.empty() ||
            capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0033))
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &gdt0101) ||
            !markers.empty())
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, nullptr) ||
            !markers.empty())
            return false;

        markers[first] = unapproved;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &unapproved) ||
            !markers.empty())
            return false;
        const std::wstring ui = L"uirs\\1161.uir";
        markers[first] = ui;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, first, &ui) ||
            !markers.empty())
            return false;

        markers[first] = gdt0033;
        if (capsule_catalog::ConsumeOverlayGdtChecksum(markers, second, &gdt0033) ||
            markers.size() != 1)
            return false;
        return true;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && std::wcscmp(argv[1], L"--idle-provider") == 0) {
        const bool ok = CheckIdleAcquisitionProvider(argv[2]);
        std::printf("IDLE_PROVIDER_UNAVAILABLE %d\n", ok ? 1 : 0);
        return ok ? 0 : 17;
    }
    if (argc < 2 || argc > 3) {
        std::puts("usage: capsule_client_runtime_harness <bundle> [rejected-bundle]");
        return 2;
    }
    capsule_catalog::Bundle bundle;
    std::string error;
    if (!capsule_catalog::LoadBundle(argv[1], bundle, error)) {
        std::printf("FAIL %s\n", error.c_str());
        return 1;
    }
    if (!CheckChecksumGate()) {
        std::puts("CHECKSUM_GATE 0");
        return 4;
    }
    std::puts("CHECKSUM_GATE 1");
    if (!CheckAcquisitionContribution(bundle)) {
        std::puts("ACQUISITION_SOURCES 0");
        return 16;
    }
    std::puts("ACQUISITION_SOURCES 1");
    for (uint32_t raw : {101u, 701u, 2901u, 8401u, 12001u, 15201u, 989901u})
        if (capsule_catalog::DisplayMachineNumber(raw) != raw / 100) return 14;
    for (uint32_t raw : {0u, 1u, 2u, 100u, 700u, 990001u, 1990001u, 0xffffffffu})
        if (capsule_catalog::DisplayMachineNumber(raw) != raw) return 15;
    if(!bundle.acquisition.units.empty()) {
        std::vector<uint8_t> row(614,0),before;
        const uint32_t unit=*bundle.acquisition.units.begin();
        row[5]=1;std::memcpy(row.data()+6,&unit,4);
        const wchar_t existing[]=L"Existing activity source";
        std::memcpy(row.data()+214,existing,sizeof(existing));before=row;
        if(!bundle.acquisition.Apply(row.data(),614) ||
           std::memcmp(row.data(),before.data(),214) ||
           std::memcmp(row.data()+414,before.data()+414,200))return 5;
        const auto once=row;
        if(!bundle.acquisition.Apply(row.data(),614) || row!=once)return 6;
        for(size_t i=0;i<81;++i)std::memcpy(row.data()+214+2*i,L"a",2);
        std::memset(row.data()+214+2*81,0,200-2*81);
        if(!bundle.acquisition.Apply(row.data(),614))return 7;
        const uint32_t other=0;std::memcpy(row.data()+6,&other,4);before=row;
        if(bundle.acquisition.Apply(row.data(),614) || row!=before)return 8;
        std::puts("ACQUISITION 1");
    }
    if(!bundle.capsuleAcquisition.units.empty()) {
        const auto legacy = std::find_if(bundle.capsuleAcquisition.units.begin(),
            bundle.capsuleAcquisition.units.end(), [](const auto& value) {
                return !value.second.full.empty();
            });
        if (legacy != bundle.capsuleAcquisition.units.end()) {
            std::vector<uint8_t> row(614,0), before;
            const uint32_t unit=legacy->first;
            row[5]=1;std::memcpy(row.data()+6,&unit,4);
            const wchar_t existing[]=L"Original shop source";
            std::memcpy(row.data()+214,existing,sizeof(existing));before=row;
            if(!bundle.capsuleAcquisition.Apply(row.data(),614) ||
               std::memcmp(row.data(),before.data(),214) ||
               std::memcmp(row.data()+414,before.data()+414,200))return 10;
            wchar_t explanation[101]{};std::memcpy(explanation,row.data()+214,200);
            const auto& hint=legacy->second;
            const wchar_t* display=bundle.capsuleAcquisition.Expected(unit);
            const wchar_t* legacyExpected=bundle.capsuleAcquisition.LegacyExpected(unit);
            if(!display || std::wcscmp(display,hint.display.c_str()) != 0 ||
               !legacyExpected || std::wcscmp(legacyExpected,hint.full.c_str()) != 0 ||
               !wcsstr(explanation,existing) ||
               (!wcsstr(explanation,hint.full.c_str()) &&
                !wcsstr(explanation,hint.brief.c_str())))return 11;
            const auto once=row;
            if(!bundle.capsuleAcquisition.Apply(row.data(),614) || row!=once)return 12;
            row[5]=0;before=row;
            if(bundle.capsuleAcquisition.Apply(row.data(),614) || row!=before)return 13;
        }
        for (const auto& value : bundle.capsuleAcquisition.units) {
            if (!value.second.full.empty()) continue;
            std::vector<uint8_t> row(614, 0);
            row[5] = 1;
            std::memcpy(row.data() + 6, &value.first, 4);
            const wchar_t existing[] = L"Original shop source";
            std::memcpy(row.data() + 214, existing, sizeof(existing));
            const auto before = row;
            const wchar_t* display=bundle.capsuleAcquisition.Expected(value.first);
            if (!display || std::wcscmp(display,value.second.display.c_str()) != 0 ||
                bundle.capsuleAcquisition.LegacyExpected(value.first) ||
                bundle.capsuleAcquisition.Apply(row.data(), 614) || row != before) return 18;
        }
        std::puts("CAPSULE_ACQUISITION 1");
    }
    std::printf("OK %s %u\n", bundle.catalogRevision.c_str(),
                static_cast<unsigned>(bundle.entries.size()));
    for (const wchar_t* name : {L"gdrs\\0033.gdt", L"gdrs\\0101.gdt",
                                L"gdrs\\0154.gdt", L"gdrs\\0155.gdt",
                                L"uirs\\1161.uir",
                                L"sprs\\60000.spr", L"txrs\\60000.txr"}) {
        std::printf("FIND %ls %d\n", name, bundle.Find(name) != nullptr ? 1 : 0);
    }
    if (argc == 3) {
        const std::string revision = bundle.catalogRevision;
        const size_t entries = bundle.entries.size();
        error.clear();
        if (capsule_catalog::LoadBundle(argv[2], bundle, error)) {
            std::puts("SECOND_UNEXPECTED_OK");
            return 3;
        }
        std::printf("SECOND_FAIL %s\n", error.c_str());
        std::printf("PRESERVED %d\n",
                    bundle.catalogRevision == revision && bundle.entries.size() == entries ? 1 : 0);
    }
    return 0;
}
