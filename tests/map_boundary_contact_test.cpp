#include "sdgo/map_boundary_contact.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
// Native 004D1DD0 admission, after the authenticated room was resolved.
bool LegacyAdmission(std::uint32_t source, std::uint32_t host, std::uint32_t target) {
    return source == target || (source == host && (target & 0x80000000u));
}
}

int main() {
    using namespace sdgo::map_boundary_contact;
    const std::uint32_t samples[] = {1, 101, 102, 65535, 1000000, kIdentityMask};
    for (auto source : samples) {
        const auto own = EncodeRequest(source, source);
        Check(own && ResolveRequest(source, 101, own, source) == source, "own contact identity was lost");
        Check(!LegacyAdmission(source, source, own), "extension could enter an unmodified death handler");
        Check(!LegacyAdmission(source, 101, own), "nonhost extension could enter native death handling");
        Check(!EncodeRequest(source, source == 1 ? 2 : 1), "foreign human cannot be encoded as own contact");
        for (std::uint32_t npc : {kNpc, kNpc | 7u, kNpc | 1000u, kNpc | kAce | 7u}) {
            const auto encoded = EncodeRequest(source, npc);
            Check(encoded != 0, "valid NPC contact was rejected");
            Check(!LegacyAdmission(source, source, encoded), "NPC contact could be misread as native NPC death");
            Check(ResolveRequest(source, source, encoded, npc) == npc, "host NPC identity was lost");
            Check(!ResolveRequest(source, source == 101 ? 102 : 101, encoded, npc), "nonhost acquired NPC damage authority");
            Check(NoticeTarget(npc | kMarker) == npc, "NPC notice lost bit31");
        }
    }
    Check(!EncodeRequest(0, 101) && !EncodeRequest(kMarker, kMarker), "reserved/empty sources cannot send");
    Check(!IsRequest(101, kNpc | kMarker | 7), "request bit31 would activate legacy NPC-death admission");
    Check(!IsNotice(kNpc | kAce | 7u), "ordinary ACE map death must remain native");
    Check(!ResolveRequest(101, 101, 101, 101), "ordinary native death must not become contact damage");
    Check(ResolveRequest(101, 101, EncodeRequest(101, kNpc | 101u), kNpc | 101u) == (kNpc | 101u),
          "full target identity must distinguish NPC and human with the same low ID");
    Check(!ResolveRequest(101, 101, EncodeRequest(101, 101), 102), "mismatched full identity was admitted");
    Check(ShockHealth(1000, 1000, 50) == 950, "five-percent contact cost changed");
    Check(ShockHealth(49, 1000, 50) == 1 && ShockHealth(1, 1000, 50) == 1,
          "boundary feedback must never consume a life or kill a mission target");
    Check(ShockHealth(0, 1000, 50) == 0, "contact cannot revive a dead unit");
    Check(ShockHealth(1001, 1000, 50) == 1001 && ShockHealth(1000, 1000, 0) == 1000,
          "invalid/disabled shock changed HP");
    std::uint8_t health[3]{};
    for (const std::uint32_t maximum : {1u, 31u, 1000u, 87750u, kMaximumHealth}) {
        for (std::uint32_t current : {1u, (maximum + 1) / 2, maximum}) {
            for (std::uint32_t amount : {1u, 50u, 1000u}) {
                const auto result = ShockHealth(current, maximum, amount);
                Check(result >= 1 && result <= current, "shock health is not nonlethal and monotonic");
                Check(EncodeHealth(result, health) && DecodeHealth(health) == result,
                      "fixed-size authority notice truncated HP");
            }
        }
    }
    Check(!EncodeHealth(0, health) && !EncodeHealth(kMaximumHealth + 1, health),
          "unrepresentable HP must not be sent");
    std::puts("boundary contact identity, legacy rejection and nonlethal HP tests passed");
}
