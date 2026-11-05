#include <windows.h>
#include <intrin.h>
#include <cmath>
#include "sdgo/native_unit_form_composition.h"

namespace sdgo::unit_forms {
namespace {
static_assert(sizeof(void*) == 4, "native form banks require the supported x86 ABI");
constexpr std::size_t kPlayerBytes = 0x1f54;
struct Scratch {
    std::array<std::uint8_t, 32> leading;
    std::array<std::uint8_t, kPlayerBytes> player{};
    std::array<std::uint8_t, 32> trailing;
    Scratch() { leading.fill(0xa7); trailing.fill(0xa7); }
    bool intact() const {
        for (auto v : leading) if (v != 0xa7) return false;
        for (auto v : trailing) if (v != 0xa7) return false;
        return true;
    }
};
void Put32(std::uint8_t* at, std::uint32_t value) { std::memcpy(at, &value, 4); }
bool Ordinary(std::uint8_t mode) {
    return mode == 1 || mode == 3 || mode == 6 || mode == 7 || mode == 9 || mode == 13 || mode == 14;
}
bool Supported(std::uint8_t mode) {
    return Ordinary(mode) || mode == 5 || mode == 8 || mode == 10 || mode == 11 || mode == 12;
}
bool MapReady(const void* registry, const NativeBindings& binding) {
    const auto* map = static_cast<const std::uint8_t*>(registry) + 0x18;
    const auto count = Read32(map + 0x1c);
    return Read32(map + 8) == 4 &&
        Read32(map + 0x14) == reinterpret_cast<std::uintptr_t>(binding.map_hash) &&
        Read32(map + 0x18) == reinterpret_cast<std::uintptr_t>(binding.map_compare) &&
        count > 0 && count <= 1048576 && Read32(map + 0x20) != 0;
}

// Keep SEH in a function with no destructible C++ local objects. All native
// output addresses refer to scratch storage. The audited map callbacks read
// integer keys only; reject changed callbacks/value extents before any lookup.
CompositionResult Execute(const NativeBindings* binding, const CompositionSeed* seed,
                          std::uint32_t identity, Scratch* scratch, std::uint32_t* fault) {
    __try {
        if (identity < 10001 || identity > 30000 || seed->part_deck < 1 || seed->part_deck > 4 ||
            !binding->find || !binding->stats || !binding->weapons || !binding->boost ||
            !binding->deck_boost || !binding->reset || !binding->unit_registry ||
            !binding->weapon_registry || !binding->map_hash || !binding->map_compare)
            return CompositionResult::invalid_input;
        if (!Supported(seed->mode)) return CompositionResult::unsupported_mode;
        // This constructor is for a fresh native bank. Do not retain stale
        // same-ID pointers when a native lookup fails, or fourth-weapon state
        // which the original reset method deliberately does not clear.
        for (auto byte : seed->before.bytes)
            if (byte != 0) return CompositionResult::invalid_input;
        if (!MapReady(binding->unit_registry, *binding) || !MapReady(binding->weapon_registry, *binding) ||
            (Ordinary(seed->mode) && (!binding->mode_singleton || !*binding->mode_singleton)))
            return CompositionResult::registry_not_ready;
        const auto* definition = static_cast<const std::uint8_t*>(binding->find(binding->unit_registry, identity));
        if (!definition || Read32(definition) != identity) return CompositionResult::native_rejected;
        auto* player = scratch->player.data();
        auto* slot = player + 0x340;
        std::memcpy(slot + kStatsOffset, seed->before.stats.data(), kStatsBytes);
        player[0x33c] = 0;
        player[0x1d93] = seed->part_deck;
        std::memcpy(player + 0x1dac + (seed->part_deck - 1) * 0x68, &seed->boost_bonus, 4);
        // These two methods save/forward but do not dereference ECX. Supplying
        // nullptr also prevents an accidental live context from being retained.
        if (!binding->stats(nullptr, slot, seed->serialized_entry.data(), definition, 0,
                            seed->rule, seed->mode, seed->source_rank))
            return CompositionResult::native_rejected;
        const auto boost = (seed->mode == 6 || seed->mode == 7)
            ? binding->deck_boost(player + 0x1d90, seed->part_deck, Read32(definition + 0x14))
            : binding->boost(player + 0x1d90, Read32(definition + 0x14));
        Put32(slot, identity);
        Put32(slot + kBankOffset, identity);
        Put32(slot + kBankOffset + 4, boost);
        Put32(slot + kBankOffset + 8, reinterpret_cast<std::uintptr_t>(definition));
        Put32(slot + kStatsOffset + 8, boost);
        if (!binding->weapons(nullptr, definition, slot + kBankOffset, player) || binding->reset(player) != 1)
            return CompositionResult::native_rejected;
        for (unsigned i = 0; i < 5; ++i) {
            const auto* weapon = *reinterpret_cast<const std::uint8_t* const*>(slot + kBankOffset + 12 + i * 0x94);
            const auto expected = i < 4 ? Read32(definition + 0x49 + i * 4) : 0;
            if ((weapon ? Read32(weapon) : 0) != expected) return CompositionResult::weapon_mismatch;
        }
        float bonus = 0;std::memcpy(&bonus, slot + kStatsOffset + 4, 4);
        if (!std::isfinite(bonus) || !scratch->intact()) return CompositionResult::scratch_corruption;
        // Table rebuild/unload belongs outside this initialization operation.
        if (!MapReady(binding->unit_registry, *binding) || !MapReady(binding->weapon_registry, *binding))
            return CompositionResult::registry_not_ready;
        return CompositionResult::composed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = GetExceptionCode();return CompositionResult::native_fault;
    }
}
} // namespace

CompositionResult ComposeNativeBank(const NativeBindings& binding, const CompositionSeed& seed,
                                    std::uint32_t unit, Bank& output, std::uint32_t& nativeFault) {
    const DWORD error = GetLastError();alignas(16) unsigned char fp[512];_fxsave(fp);
    Scratch scratch;
    std::uint32_t fault = 0;
    const auto result = Execute(&binding, &seed, unit, &scratch, &fault);
    if (result == CompositionResult::composed) {
        Bank composed;
        std::memcpy(composed.bytes.data(), scratch.player.data() + 0x340 + kBankOffset, kBankBytes);
        std::memcpy(composed.stats.data(), scratch.player.data() + 0x340 + kStatsOffset, kStatsBytes);
        output = composed;
    }
    nativeFault = fault;
    _fxrstor(fp);SetLastError(error);
    return result;
}
} // namespace sdgo::unit_forms
