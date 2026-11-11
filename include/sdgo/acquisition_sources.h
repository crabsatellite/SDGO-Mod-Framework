#pragma once
#include <cstdint>

#define SDGO_ACQUISITION_SOURCES_EXPORT "SdgoAcquisitionSourcesV1"
#define SDGO_ACQUISITION_DISPLAY_EXPORT "SdgoAcquisitionDisplayInstalledV1"
#define SDGO_ACQUISITION_LOCALIZE_EXPORT "SdgoUiLocalizationAcquisitionEventsV1"
#define SDGO_BLUEPRINT_ACQUISITION_STATE_EXPORT "SdgoBlueprintAcquisitionStateV1"
#define SDGO_ACQUISITION_DISPLAY_TEXT_EXPORT "SdgoAcquisitionDisplayTextV1"

enum : std::uint32_t {
    SDGO_ACQUISITION_UNIT = 1,
    SDGO_ACQUISITION_BLUEPRINT = 2,
    SDGO_ACQUISITION_UNAVAILABLE = 0,
    SDGO_ACQUISITION_READY = 1,
    SDGO_ACQUISITION_INCOMPLETE = 2,
};

struct SdgoAcquisitionRequestV1 {
    std::uint32_t size, version, subject, id;
};
struct SdgoAcquisitionSinkV1 {
    std::uint32_t size, version;
    void* context;
    // Synchronous copied text. Keys identify a source within this provider.
    // Do not retain the sink or emit player data, credentials or SQL settings.
    int(__cdecl* Emit)(void*, const char* key, const wchar_t* text, std::uint32_t chars);
};
using SdgoAcquisitionSourcesV1Fn = std::uint32_t(__cdecl*)(
    const SdgoAcquisitionRequestV1*, const SdgoAcquisitionSinkV1*);
using SdgoAcquisitionDisplayInstalledV1Fn = int(__cdecl*)();
using SdgoUiLocalizationAcquisitionEventsV1Fn = void(__cdecl*)(void*);
// 0 unclaimed stock recipe, 1 enabled owner recipe, 2 disabled owner recipe,
// 3 ownership/availability cannot be established for this loaded provider.
using SdgoBlueprintAcquisitionStateV1Fn = std::uint32_t(__cdecl*)(std::uint32_t recipe, std::uint32_t unit);
// Copy text from the visible native CStatic of a FinishedProduct service.
// Returns UTF-16 units excluding NUL; zero means unavailable or insufficient space.
using SdgoAcquisitionDisplayTextV1Fn = std::uint32_t(__cdecl*)(void*, wchar_t*, std::uint32_t);

namespace sdgo::acquisition_sources {
inline bool DisplayInstalled(SdgoAcquisitionDisplayInstalledV1Fn query) noexcept {
    if (!query) return false;
    __try { return query() == 1; }
    __except (1) { return false; }
}
inline std::uint32_t BlueprintState(SdgoBlueprintAcquisitionStateV1Fn query,
                                    std::uint32_t recipe, std::uint32_t unit) noexcept {
    if (!query) return 3;
    __try { const auto state = query(recipe, unit); return state <= 3 ? state : 3; }
    __except (1) { return 3; }
}
} // namespace sdgo::acquisition_sources

// Provider/consumer integration is optional. Resolve the export only from an
// already loaded module, validate this version and fail closed per provider.
// Absence must never prevent that module's own gameplay feature from loading.
