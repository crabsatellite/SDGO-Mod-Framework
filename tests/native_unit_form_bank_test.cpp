#include "sdgo/native_unit_form_bank.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>

using namespace sdgo::unit_forms;

void Put(std::uint8_t* bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}

int main() {
    for (unsigned active = 0; active < 2; ++active) {
        Slot original;
        for (std::size_t i = 0; i < original.size(); ++i)
            original[i] = static_cast<std::uint8_t>((i * 31 + 17) & 255);
        Put(original.data(), 0, 15000);
        Put(original.data(), kActiveBankOffset, active);
        Put(original.data(), kBankOffset + active * kBankBytes, 15001);
        Put(original.data(), kBankOffset + active * kBankBytes + 8, 0x10000000);
        Put(original.data(), kBankOffset + (active ^ 1) * kBankBytes, 15000);
        Bank replacement;
        replacement.bytes.fill(0x71);replacement.stats.fill(0x83);
        Put(replacement.bytes.data(), 0, 15002);
        Put(replacement.bytes.data(), 8, 0x20000000);
        Slot staged{};
        const auto preserved = original;
        assert(StageInactive(original, 15000, 15001, replacement, staged) == StageResult::prepared);
        assert(original == preserved);
        for (std::size_t i = 0; i < original.size(); ++i) {
            const auto bankAt = kBankOffset + (active ^ 1) * kBankBytes;
            const auto statsAt = kStatsOffset + (active ^ 1) * kStatsBytes;
            if (i >= bankAt && i < bankAt + kBankBytes)
                assert(staged[i] == replacement.bytes[i - bankAt]);
            else if (i >= statsAt && i < statsAt + kStatsBytes)
                assert(staged[i] == replacement.stats[i - statsAt]);
            else assert(staged[i] == original[i]);
        }
        Bank captured;
        assert(Capture(staged, active ^ 1, captured));
        assert(captured.bytes == replacement.bytes && captured.stats == replacement.stats);
        Slot untouched;untouched.fill(0x42);const auto before = untouched;
        assert(StageInactive(staged, 15000, 15001, replacement, untouched) == StageResult::already_cached);
        assert(untouched == before);
        auto brokenCached = staged;
        Put(brokenCached.data(), kBankOffset + (active ^ 1) * kBankBytes + 8, 0);
        assert(StageInactive(brokenCached, 15000, 15001, replacement, untouched) == StageResult::invalid);
        for (const auto offset : {4, 8, 12, 12 + 4 * 0x94}) {
            auto changed = replacement;changed.bytes[offset] ^= 1;
            assert(StageInactive(staged, 15000, 15001, changed, untouched) == StageResult::invalid);
        }
        auto changedStats = replacement;changedStats.stats[4] ^= 1;
        assert(StageInactive(staged, 15000, 15001, changedStats, untouched) == StageResult::invalid);
        auto used = staged;used[kBankOffset + (active ^ 1) * kBankBytes + 16] ^= 1;
        assert(StageInactive(used, 15000, 15001, replacement, untouched) == StageResult::already_cached);
        assert(untouched == before);
        assert(StageInactive(original, 15000, 15001, replacement, original) == StageResult::invalid);
        assert(original == preserved);
        assert(StageInactive(original, 14999, 15001, replacement, untouched) == StageResult::invalid);
        assert(StageInactive(original, 15000, 15002, replacement, untouched) == StageResult::invalid);
        auto bad = replacement;Put(bad.bytes.data(), 0, 35000);
        assert(StageInactive(original, 15000, 15001, bad, untouched) == StageResult::invalid);
        bad = replacement;Put(bad.bytes.data(), 8, 0);
        assert(StageInactive(original, 15000, 15001, bad, untouched) == StageResult::invalid);
        bad = replacement;Put(bad.bytes.data(), 0, 15001);
        assert(StageInactive(original, 15000, 15001, bad, untouched) == StageResult::invalid);
        auto corrupt = original;Put(corrupt.data(), kActiveBankOffset, 2);
        assert(StageInactive(corrupt, 15000, 15001, replacement, untouched) == StageResult::invalid);
        assert(untouched == before);
        const auto capturedBefore = captured;
        assert(!Capture(staged, 2, captured));
        assert(captured.bytes == capturedBefore.bytes && captured.stats == capturedBefore.stats);

        Bank initialSource{},initialTarget{};
        Put(initialSource.bytes.data(),0,15000);Put(initialSource.bytes.data(),4,3000);
        Put(initialSource.bytes.data(),8,0x30000000);
        for(unsigned weapon=0;weapon<5;++weapon)
            Put(initialSource.bytes.data(),12+weapon*0x94,weapon<4?0x40000000+weapon*0x100:0);
        initialSource.stats.fill(0x19);initialTarget=initialSource;
        Put(initialTarget.bytes.data(),0,15002);Put(initialTarget.bytes.data(),8,0x50000000);
        auto equivalent=original;const auto inactiveAt=kBankOffset+(active^1)*kBankBytes;
        // Preserve nonzero runtime state even in weapon slots 3 and 4, which
        // the original three-weapon reset does not fully initialize.
        Put(equivalent.data(),inactiveAt,initialSource.id());
        Put(equivalent.data(),inactiveAt+8,Read32(initialSource.bytes.data()+8));
        for(unsigned weapon=0;weapon<5;++weapon)
            Put(equivalent.data(),inactiveAt+12+weapon*0x94,Read32(initialSource.bytes.data()+12+weapon*0x94));
        const auto equivalentBefore=equivalent;
        assert(StageEquivalentInactive(equivalent,15000,15001,initialSource,initialTarget,staged)==StageResult::prepared);
        assert(equivalent==equivalentBefore);
        for(std::size_t i=0;i<kSlotBytes;++i)
            if(!((i>=inactiveAt&&i<inactiveAt+4)||(i>=inactiveAt+8&&i<inactiveAt+12)))
                assert(staged[i]==equivalentBefore[i]);
        assert(Read32(staged.data()+inactiveAt)==15002&&Read32(staged.data()+inactiveAt+8)==0x50000000);
        assert(StageEquivalentInactive(staged,15000,15001,initialSource,initialTarget,untouched)==StageResult::already_cached);
        assert(untouched==before);
        for(std::size_t at:{4u,12u,12u+3u*0x94u,12u+4u*0x94u,0x2efu}) {
            auto changed=initialTarget;changed.bytes[at]^=1;
            assert(StageEquivalentInactive(equivalent,15000,15001,initialSource,changed,untouched)==StageResult::invalid);
            assert(untouched==before);
        }
        auto differentStats=initialTarget;differentStats.stats[8]^=1;
        assert(StageEquivalentInactive(equivalent,15000,15001,initialSource,differentStats,untouched)==StageResult::invalid);
        for(unsigned weapon=0;weapon<5;++weapon) {
            auto changed=equivalent;changed[inactiveAt+12+weapon*0x94]^=1;
            assert(StageEquivalentInactive(changed,15000,15001,initialSource,initialTarget,untouched)==StageResult::invalid);
        }
        assert(StageEquivalentInactive(equivalent,15000,15001,initialSource,initialTarget,equivalent)==StageResult::invalid);
        assert(equivalent==equivalentBefore&&untouched==before);
    }
    std::puts("native_unit_form_bank_test: PASS (staging only, no native activation)");
}
