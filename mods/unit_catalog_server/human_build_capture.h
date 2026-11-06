#pragma once
#include "sdgo/sdgo_api.h"
#include "sdgo/human_unit_capture.h"
#include <memory>
#include <string>

namespace unit_catalog_server {
class HumanBuildCapture {
public:
    HumanBuildCapture();
    ~HumanBuildCapture();
    bool Prepare(const SdgoApi*,SdgoMod*,std::shared_ptr<const sdgo::unit_forms::GraphBindings>,std::string& error);
    // External registration must be the final fallible Mod load action.
    bool Attach();
    bool Healthy() const;
    sdgo::unit_forms::HumanUnitCaptureStatus Status() const;
    bool Inspect(sdgo::unit_forms::HumanUnitCaptureSnapshot&) const;
    bool Snapshot(const void*,const void*,std::uint32_t,std::uint32_t,std::uint64_t,
                  sdgo::unit_forms::HumanUnitCaptureSnapshot&) const;
    // Recheck under the original native room lock immediately before any use
    // which could change an actor. This is still not native actor permission.
    bool Current(const sdgo::unit_forms::HumanUnitCaptureSnapshot&) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace unit_catalog_server
