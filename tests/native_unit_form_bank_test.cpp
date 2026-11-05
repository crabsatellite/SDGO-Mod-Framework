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
    }
    std::puts("native_unit_form_bank_test: PASS (staging only, no native activation)");
}
