#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace battle_ai::ace {
struct UnitProfileInit {
    void* unit = nullptr;
    uint64_t nativeInitSerial = 0;
    uint32_t battleGeneration = 0;
};
struct UnitProfile {
    int difficulty = 1;
    uint32_t callerGeneration = 0;
    uint64_t nativeInitSerial = 0;
    uint32_t battleGeneration = 0;
};

// Binds an external per-unit AI profile to both the current battle and the most
// recent native Init. A late Init clears an older profile, while the common
// Init -> external endpoint -> first Think order preserves the endpoint value.
class UnitProfileRegistry {
    mutable std::mutex mutex_;
    std::unordered_map<void*, uint64_t> initSerials_;
    std::unordered_map<void*, UnitProfile> profiles_;
    uint64_t nextInitSerial_ = 0;
    uint32_t battleGeneration_ = 0;
public:
    void BeginBattle(uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        battleGeneration_ = generation;
        initSerials_.clear();
        profiles_.clear();
    }
    UnitProfileInit NoteNativeInit(void* unit) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (++nextInitSerial_ == 0) ++nextInitSerial_;
        const uint64_t serial = nextInitSerial_;
        initSerials_[unit] = serial;
        return {unit, serial, battleGeneration_};
    }
    bool Apply(void* unit, int difficulty, uint32_t callerGeneration) {
        if (!unit || difficulty < 0 || difficulty > 2 || !callerGeneration) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto current = profiles_.find(unit);
        if (current != profiles_.end() &&
            current->second.battleGeneration == battleGeneration_ &&
            current->second.callerGeneration > callerGeneration)
            return false;
        const auto init = initSerials_.find(unit);
        profiles_[unit] = UnitProfile{
            difficulty,
            callerGeneration,
            init == initSerials_.end() ? 0 : init->second,
            battleGeneration_};
        return true;
    }
    // Reset after one exact native Init. Preserve only an override that was
    // authored after that Init in the same battle generation.
    bool Reset(const UnitProfileInit& event, bool* currentBattle = nullptr) {
        if (currentBattle) *currentBattle = false;
        if (!event.unit) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.battleGeneration != battleGeneration_) return false;
        if (currentBattle) *currentBattle = true;
        const auto found = profiles_.find(event.unit);
        if (found == profiles_.end()) return false;
        const bool preserve = found->second.battleGeneration == event.battleGeneration &&
                              found->second.nativeInitSerial >= event.nativeInitSerial;
        if (!preserve) profiles_.erase(found);
        return preserve;
    }
    int Get(void* unit, int fallback) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = profiles_.find(unit);
        return found == profiles_.end() || found->second.battleGeneration != battleGeneration_
            ? fallback : found->second.difficulty;
    }
    uint32_t BattleGeneration() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return battleGeneration_;
    }
    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return profiles_.size();
    }
};
} // namespace battle_ai::ace
