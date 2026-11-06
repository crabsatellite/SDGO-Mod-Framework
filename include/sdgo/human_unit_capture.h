#pragma once
#include "sdgo/battle_human_unit_observer.h"
#include "sdgo/native_unit_form_composition.h"
#include "sdgo/unit_form_graph.h"
#include <memory>

namespace sdgo::unit_forms {
struct HumanUnitSeed {
    std::uint32_t root = 0, owner = 0, deck = 0;
    std::uint64_t serial = 0;
    const void* definition = nullptr; // Borrowed from this process/catalog lifetime.
    CompositionSeed composition;
};
struct HumanUnitCaptureSnapshot {
    std::uintptr_t room = 0, player = 0;
    std::uint64_t roomEpoch = 0, generation = 0, invocation = 0;
    HumanUnitSeed unit;
};
struct HumanUnitCaptureStatus {
    bool healthy = false;
    std::uint32_t rooms = 0, players = 0, pending = 0;
    std::uint64_t completed = 0, rejected = 0;
};

// Read-only native preimage capture and process-local lifecycle bookkeeping.
// The callback copies bounded POD into a fixed atomic queue without locks or
// allocation. Snapshot/status calls drain it outside the callback. Overflow,
// missing phases and illegal promotion fail closed. Only reset-to-accepted
// start scopes are supported; in-round rebuild needs a separate native profile.
// No source data is serialized and no native actor is changed. The caller must
// separately check provider status 2, graph.resolveTarget, live actor/UID/root,
// current registry pointers and original sender/alive/room-lock authorization.
class HumanUnitCapture {
public:
    explicit HumanUnitCapture(std::shared_ptr<const GraphBindings> graphs);
    ~HumanUnitCapture();
    HumanUnitCapture(const HumanUnitCapture&) = delete;
    HumanUnitCapture& operator=(const HumanUnitCapture&) = delete;
    void observe(const SdgoBattleHumanUnitEventV1& event) noexcept;
    bool snapshot(const void* room, const void* player, std::uint32_t deck,
                  std::uint32_t root, std::uint64_t serial, HumanUnitCaptureSnapshot& output) const;
    bool current(const HumanUnitCaptureSnapshot&) const; // Token validity only, not current live effects or native actor proof.
    bool inspect(HumanUnitCaptureSnapshot&) const; // Diagnostic representative only; never transform permission.
    HumanUnitCaptureStatus status() const;
    void disable() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sdgo::unit_forms
