#pragma once
#include <cstdint>
#define SDGO_NEWCOMER_PROTECTION_RECENT_EXPORT "SdgoNewcomerProtectionRecentV1"
struct SdgoNewcomerProtectionReceiptV1 {
    std::uint32_t size,version,sequence,account,unit,deck,mode;
    std::uint32_t maximum_before,maximum_after,current_before,current_after;
    std::uint16_t attack_before[2],attack_after[2];
    std::uint32_t special_before[2],special_after[2];
};
typedef std::uint32_t(__cdecl* SdgoNewcomerProtectionRecentV1Fn)(SdgoNewcomerProtectionReceiptV1*,std::uint32_t);
#define SDGO_NEWCOMER_PROTECTION_HITS_EXPORT "SdgoNewcomerProtectionHitsV1"
struct SdgoNewcomerProtectionHitV1 {
    std::uint32_t size,version,sequence,account,victim,mode,weapon;
    std::uint32_t proposed_before,proposed_after,cap_before,cap_after;
};
typedef std::uint32_t(__cdecl* SdgoNewcomerProtectionHitsV1Fn)(SdgoNewcomerProtectionHitV1*,std::uint32_t);
#define SDGO_NEWCOMER_PROTECTION_STATUS_EXPORT "SdgoNewcomerProtectionGetStatusV1"
struct SdgoNewcomerProtectionStatusV1 {
    std::uint32_t size,version,hit_calls,human_calls,roster_calls,ordinary_roster_calls,membership_rejected,last_account,last_type;
};
typedef int(__cdecl* SdgoNewcomerProtectionStatusV1Fn)(SdgoNewcomerProtectionStatusV1*);
