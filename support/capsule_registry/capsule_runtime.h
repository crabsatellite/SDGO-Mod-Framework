#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "sdgo/acquisition_sources.h"
#include "../native_resource_overlay/native_resource_overlay.h"
#include "acquisition_hints.h"
#include "capsule_acquisition_hints.h"

namespace capsule_catalog
{
    using NativeFileInfo = sdgo::resource_overlay::NativeFileInfo;
    using OverlayEntry = sdgo::resource_overlay::OverlayEntry;

    struct Bundle
    {
        std::wstring zpkPath;
        std::string catalogRevision;
        std::string indexHash;
        std::string archiveHash;
        uint32_t chunkSize = 0;
        uint32_t fixedCount = 0;
        uint32_t coinCount = 0;
        uint32_t customCount = 0;
        uint32_t candidateCount = 0;
        uint32_t activityCount = 0;
        AcquisitionHints acquisition;
        CapsuleAcquisitionHints capsuleAcquisition;
        std::unordered_map<std::wstring, OverlayEntry> entries;

        const OverlayEntry* Find(const std::wstring& nativePath) const;
    };

    bool IsOverlayGdtPath(const std::wstring& path);

    // Consumes a tracked stream marker exactly once. A mismatched or missing
    // native path still removes the marker so pointer reuse cannot authorize a
    // later checksum call.
    bool ConsumeOverlayGdtChecksum(
        std::unordered_map<void*, std::wstring>& trackedStreams,
        void* stream,
        const std::wstring* nativePath);

    // Optional acquisition-display contribution. This does not depend on the
    // display consumer being installed; callers resolve the DLL export only
    // when both modules are present.
    uint32_t EmitAcquisitionSources(
        const Bundle& bundle,
        const SdgoAcquisitionRequestV1* request,
        const SdgoAcquisitionSinkV1* sink);

    // Loads a complete compiler output bundle from runtime.ini. Every declared
    // GDT and both archive files are hashed before any result is published.
    // On failure, out is left unchanged so the caller can use the stock pack.
    bool LoadBundle(const std::wstring& modDirectory, Bundle& out, std::string& error);
}
