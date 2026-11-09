#pragma once
#include <array>
#include <cstdint>

namespace room_random_client {
struct SkillPartSnapshot {
    uint32_t owner=0;
    uint64_t unit=0;
    uint32_t part=0;
};

// Consumption belongs to an inventory instance, not to the current model.
// Unknown identities and genuine removal on the same instance stay native.
inline bool DifferentSkillPartInstance(const SkillPartSnapshot& before,
                                       uint32_t owner,int32_t nativePart,
                                       uint64_t afterUnit,uint32_t afterPart) {
    return before.owner && before.owner==owner && before.unit && afterUnit &&
        before.unit!=afterUnit && nativePart>0 &&
        before.part==static_cast<uint32_t>(nativePart) && afterPart==0;
}

class SkillPartReads {
    struct Entry {void* reader=nullptr;SkillPartSnapshot snapshot;};
    std::array<Entry,32> entries_{};
public:
    void Begin(void* reader,const SkillPartSnapshot& snapshot) {
        if(!reader)return;
        for(auto& entry:entries_)if(entry.reader==reader) {entry={};break;}
        if(!snapshot.owner || !snapshot.unit)return;
        for(auto& entry:entries_)if(!entry.reader) {entry={reader,snapshot};return;}
    }
    SkillPartSnapshot Take(void* reader) {
        for(auto& entry:entries_)if(entry.reader==reader) {
            const auto snapshot=entry.snapshot;entry={};return snapshot;
        }
        return {};
    }
};
}
