#include <windows.h>
#include <float.h>
#include "sdgo/native_unit_form_composition.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using namespace sdgo::unit_forms;
namespace {
std::array<std::uint8_t, 0x60> unitRegistry{}, weaponRegistry{}, definition{};
std::array<std::array<std::uint8_t, 4>, 4> weaponDefinitions{};
unsigned calls = 0;
bool fault = false, missingWeapon = false;
void* singleton = reinterpret_cast<void*>(1);
void Put(std::uint8_t* p, std::size_t at, std::uint32_t v) { std::memcpy(p + at, &v, 4); }
unsigned Hash(const unsigned* value) { return *value; }
int Compare(const int* a, const int* b) { return *a - *b; }
void* __fastcall Find(void* registry, void*, std::uint32_t id) {
    ++calls;assert(registry == unitRegistry.data());
    if (fault) RaiseException(0xe0420001, 0, 0, nullptr);
    return id == 15002 ? definition.data() : nullptr;
}
unsigned __fastcall Stats(void* unused, void*, void* raw, const void* serialized, const void* unit,
                          unsigned bank, std::uint8_t rule, std::uint8_t mode, std::uint8_t rank) {
    assert(!unused && serialized && unit == definition.data() && bank == 0 && rule == 2 && mode != 0 && rank == 7);
    auto* slot = static_cast<std::uint8_t*>(raw);
    Put(slot, 4, 99999); // Scratch maxHP must never escape in the output Bank.
    const std::uint16_t attack = 23, defense = 19;const float bonus = 2.5f;
    std::memcpy(slot + 0xe, &attack, 2);std::memcpy(slot + 0x10, &defense, 2);
    std::memcpy(slot + 0x12, &bonus, 4);
    unsigned ignored = 0;_controlfp_s(&ignored, _RC_UP, _MCW_RC);SetLastError(9191);
    return 1;
}
unsigned __fastcall Weapons(void* unused, void*, const void* unit, void* raw, void* player) {
    assert(!unused && unit == definition.data() && player);
    auto* bank = static_cast<std::uint8_t*>(raw);
    for (unsigned i = 0; i < 4; ++i)
        if (!(missingWeapon && i == 2)) // Native miss leaves the pointer untouched.
            Put(bank, 12 + i * 0x94, reinterpret_cast<std::uintptr_t>(weaponDefinitions[i].data()));
    std::memset(bank + 0x25c, 0, 0x94);
    return 1;
}
unsigned __fastcall Boost(void* raw, void*, unsigned base) {
    auto* manager = static_cast<const std::uint8_t*>(raw);
    const auto deck = manager[3];assert(deck >= 1 && deck <= 4);
    std::int32_t bonus = 0;std::memcpy(&bonus, manager + 4 + (deck - 1) * 0x68 + 0x18, 4);
    return static_cast<unsigned>(static_cast<std::int32_t>(base) + bonus);
}
unsigned __fastcall DeckBoost(void* raw, void*, std::uint8_t deck, unsigned base) {
    auto* manager = static_cast<const std::uint8_t*>(raw);assert(deck == manager[3]);
    return Boost(raw, nullptr, base);
}
unsigned __fastcall Reset(void* raw, void*) {
    auto* player = static_cast<std::uint8_t*>(raw);assert(player[0x33c] == 0);
    auto* slot = player + 0x340;assert(Read32(slot) == 15002);
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned weapon = 0; weapon < 3; ++weapon)
            std::memset(slot + kBankOffset + bank * kBankBytes + 16 + weapon * 0x94, 0, 0x90);
    return 1;
}
NativeBindings Bind() {
    NativeBindings result;
    result.find = reinterpret_cast<NativeBindings::Find>(&Find);
    result.stats = reinterpret_cast<NativeBindings::Stats>(&Stats);
    result.weapons = reinterpret_cast<NativeBindings::Weapons>(&Weapons);
    result.boost = reinterpret_cast<NativeBindings::Boost>(&Boost);
    result.deck_boost = reinterpret_cast<NativeBindings::DeckBoost>(&DeckBoost);
    result.reset = reinterpret_cast<NativeBindings::Reset>(&Reset);
    result.unit_registry = unitRegistry.data();result.weapon_registry = weaponRegistry.data();
    result.map_hash = reinterpret_cast<const void*>(&Hash);result.map_compare = reinterpret_cast<const void*>(&Compare);
    result.mode_singleton = &singleton;
    for (auto* object : {unitRegistry.data(), weaponRegistry.data()}) {
        Put(object, 0x20, 4);Put(object, 0x2c, reinterpret_cast<std::uintptr_t>(&Hash));
        Put(object, 0x30, reinterpret_cast<std::uintptr_t>(&Compare));Put(object, 0x34, 101);Put(object, 0x38, 1);
    }
    Put(definition.data(), 0, 15002);Put(definition.data(), 0x14, 70);
    for (unsigned i = 0; i < 4; ++i) { Put(definition.data(), 0x49 + i * 4, 900001 + i);Put(weaponDefinitions[i].data(), 0, 900001 + i); }
    return result;
}
} // namespace

int main() {
    auto binding = Bind();CompositionSeed seed;seed.mode = 1;seed.rule = 2;seed.source_rank = 7;
    seed.part_deck = 3;seed.boost_bonus = 11;
    Bank output;output.bytes.fill(0x51);output.stats.fill(0x61);const auto before = output;
    auto checkFailure = [&](CompositionResult expected, const NativeBindings& selected, const CompositionSeed& input, unsigned id) {
        std::uint32_t exception = 0;SetLastError(7070);unsigned fpBefore = 0, fpAfter = 0;
        _controlfp_s(&fpBefore, 0, 0);
        assert(ComposeNativeBank(selected, input, id, output, exception) == expected);
        assert(output.bytes == before.bytes && output.stats == before.stats && GetLastError() == 7070);
        _controlfp_s(&fpAfter, 0, 0);assert(fpBefore == fpAfter);
        assert((expected == CompositionResult::native_fault) == (exception != 0));
    };
    checkFailure(CompositionResult::invalid_input, binding, seed, 35000);
    auto unsupported = seed;unsupported.mode = 2;
    checkFailure(CompositionResult::unsupported_mode, binding, unsupported, 15002);
    auto stale = seed;
    Put(stale.before.bytes.data(), 12 + 2 * 0x94, reinterpret_cast<std::uintptr_t>(weaponDefinitions[2].data()));
    missingWeapon = true;checkFailure(CompositionResult::invalid_input, binding, stale, 15002);missingWeapon = false;
    stale = seed;stale.before.bytes[16 + 3 * 0x94 + 17] = 9;
    checkFailure(CompositionResult::invalid_input, binding, stale, 15002);
    singleton = nullptr;
    checkFailure(CompositionResult::registry_not_ready, binding, seed, 15002);
    singleton = reinterpret_cast<void*>(1);
    Put(weaponRegistry.data(), 0x30, 0);
    checkFailure(CompositionResult::registry_not_ready, binding, seed, 15002);
    assert(calls == 0);binding = Bind();
    fault = true;checkFailure(CompositionResult::native_fault, binding, seed, 15002);fault = false;
    missingWeapon = true;checkFailure(CompositionResult::weapon_mismatch, binding, seed, 15002);missingWeapon = false;
    for (auto mode : {1, 6, 7, 10}) {
        seed.mode = static_cast<std::uint8_t>(mode);std::uint32_t exception = 0;SetLastError(8080);
        unsigned fpBefore = 0, fpAfter = 0;_controlfp_s(&fpBefore, 0, 0);
        assert(ComposeNativeBank(binding, seed, 15002, output, exception) == CompositionResult::composed);
        assert(output.id() == 15002 && output.has_definition() && Read32(output.bytes.data() + 4) == 81);
        assert(Read32(output.stats.data() + 8) == 81 && exception == 0 && GetLastError() == 8080);
        _controlfp_s(&fpAfter, 0, 0);assert(fpBefore == fpAfter);
        for (unsigned i = 0; i < 4; ++i) {
            assert(Read32(output.bytes.data() + 12 + i * 0x94) == reinterpret_cast<std::uintptr_t>(weaponDefinitions[i].data()));
            for (unsigned n = 0; n < 0x90; ++n) assert(output.bytes[16 + i * 0x94 + n] == 0);
        }
        for (unsigned n = 0x25c; n < kBankBytes; ++n) assert(output.bytes[n] == 0);
    }
    std::puts("native_unit_form_composition_test: PASS (stubbed ABI/isolation and rejection checks; no native gameplay)");
}
