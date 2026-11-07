#pragma once
#include "sdgo/native_unit_form_bank.h"

namespace sdgo::unit_forms {

// Bind only verified symbols from the supported original x86 server image.
// Registry pointers and function pointers are installation/runtime inputs,
// never fields accepted from a content package.
struct NativeBindings {
    using Find = void* (__thiscall*)(void*, std::uint32_t);
    using Stats = unsigned (__thiscall*)(void*, void*, const void*, const void*, unsigned,
                                        std::uint8_t, std::uint8_t, std::uint8_t);
    using Weapons = unsigned (__thiscall*)(void*, const void*, void*, void*);
    using Boost = std::uint32_t (__thiscall*)(void*, std::uint32_t);
    using DeckBoost = std::uint32_t (__thiscall*)(void*, std::uint8_t, std::uint32_t);
    using Reset = unsigned (__thiscall*)(void*);
    Find find = nullptr;
    Stats stats = nullptr;
    Weapons weapons = nullptr;
    Boost boost = nullptr;
    DeckBoost deck_boost = nullptr;
    Reset reset = nullptr;
    void* unit_registry = nullptr;
    void* weapon_registry = nullptr;
    const void* map_hash = nullptr;
    const void* map_compare = nullptr;
    void* const* mode_singleton = nullptr;
};

// Captured by the host at normal initialization. Only scalar equipment state
// and the selected pre-initialization stats are retained; no shallow player,
// inventory manager, linked list, room or owner object is copied.
// before.bytes must be entirely zero: this creates a fresh bank, not a state
// migration or a reconstruction of an already-used weapon bank.
struct CompositionSeed {
    Bank before;
    std::array<std::uint8_t, 0x99> serialized_entry{};
    std::uint8_t rule = 0, mode = 0, source_rank = 0, part_deck = 0;
    std::int32_t boost_bonus = 0;
};

enum class CompositionResult {
    composed, invalid_input, unsupported_mode, registry_not_ready,
    native_rejected, native_fault, weapon_mismatch, scratch_corruption
};

// Produces a borrowed, process-local bank and its stats. Does not authorize a
// graph edge, publish records, change live actors, select a weapon, or commit
// anything. Output stays unchanged on every failure. FP/LastError are retained.
CompositionResult ComposeNativeBank(const NativeBindings&, const CompositionSeed&,
                                    std::uint32_t unit, Bank& output,
                                    std::uint32_t& native_fault);

} // namespace sdgo::unit_forms
