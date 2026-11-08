// Over-custom (OC) six-stat enhancement: wire formats shared by the World
// server Mod, the Battle server Mod and the client Mod.
//
// Two carriers exist:
//   1. The owned-unit record (_UnitData, 0x99 bytes) carries the stat deltas
//      that the player's allocation yields. The original OC columns lived in
//      the reserved gap after UnitKill (offsets 0x15..0x2D); that gap is zero
//      in the current servers and clients, so the World Mod writes the payload
//      there before the record leaves the server. Every native copy of the
//      record (lobby pages, active-unit replies, room pilots) keeps it intact.
//   2. Lobby requests (query, allocate, challenge, reset) ride the friend-gift
//      extension command range. See friend_unit_gift_protocol.h.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/friend_unit_gift_protocol.h"

namespace sdgo::oc_custom {

inline constexpr std::size_t kUnitRecordBytes = 0x99;
inline constexpr std::size_t kCarrierOffset = 0x15;
inline constexpr std::size_t kCarrierBytes = 25;
inline constexpr std::size_t kCustomSkillOffset = 138; // four u16 UnitSkill slots
inline constexpr std::size_t kCustomSkillCount = 4;
inline constexpr std::uint8_t kCarrierVersion = 1;
// Bytes 'O','C' at the start of the reserved gap.
inline constexpr std::uint8_t kCarrierMagic0 = 0x4F;
inline constexpr std::uint8_t kCarrierMagic1 = 0x43;

enum Stat : std::uint8_t {
    stat_speed = 0,
    stat_hp = 1,
    stat_attack = 2,
    stat_defense = 3,
    stat_agility = 4,
    stat_special = 5,
    stat_count = 6,
};

// tbl_Custom caps of the original rule set (constants in the 2009 procedures).
inline constexpr std::array<std::int32_t, stat_count> kStatCaps = {
    100, 23800, 169, 169, 100, 7950
};

enum CarrierFlag : std::uint8_t {
    carrier_oc_mode = 1u << 0, // the server applies OC and ignores custom skills
    carrier_profile = 1u << 1, // an allocation profile exists for this unit
    // Room systems ("room" mode): the record keeps its custom skills and the
    // room decides. A room whose title carries the !oc command plays OC (the
    // custom skills are ignored in its battles); every other room plays the
    // current custom-skill system. See oc_room_system.h.
    carrier_room_rule = 1u << 2,
};

// ModeReply.flags
inline constexpr std::uint32_t kModeFlagSchemaReady = 1u << 0;
inline constexpr std::uint32_t kModeFlagRoomRule = 1u << 1;

struct Carrier {
    std::uint8_t flags = 0;
    std::uint8_t oc_level = 0;
    std::uint8_t normal_points = 0;
    std::uint8_t over_points = 0;
    std::array<std::int16_t, stat_count> delta{};
    std::uint32_t revision = 0;
};

namespace detail {
inline std::uint8_t Checksum(const std::uint8_t* carrier) noexcept {
    std::uint8_t value = 0x5A;
    for (std::size_t index = 0; index < kCarrierBytes - 2; ++index)
        value = static_cast<std::uint8_t>(value ^ carrier[index]);
    return value;
}
} // namespace detail

// record points at the first byte of the 0x99-byte owned-unit record.
inline void WriteCarrier(std::uint8_t* record, const Carrier& carrier) noexcept {
    using friend_unit_gift::wire::Write16;
    using friend_unit_gift::wire::Write32;
    auto* out = record + kCarrierOffset;
    std::memset(out, 0, kCarrierBytes);
    out[0] = kCarrierMagic0;
    out[1] = kCarrierMagic1;
    out[2] = kCarrierVersion;
    out[3] = carrier.flags;
    out[4] = carrier.oc_level;
    out[5] = carrier.normal_points;
    out[6] = carrier.over_points;
    for (std::size_t index = 0; index < stat_count; ++index)
        Write16(out + 7 + index * 2,
                static_cast<std::uint16_t>(carrier.delta[index]));
    Write32(out + 19, carrier.revision);
    out[23] = detail::Checksum(out);
    out[24] = 0;
}

inline bool ReadCarrier(const std::uint8_t* record, Carrier* carrier) noexcept {
    using friend_unit_gift::wire::Read16;
    using friend_unit_gift::wire::Read32;
    if (!record || !carrier) return false;
    const auto* in = record + kCarrierOffset;
    if (in[0] != kCarrierMagic0 || in[1] != kCarrierMagic1 ||
        in[2] != kCarrierVersion || in[24] != 0 ||
        in[23] != detail::Checksum(in))
        return false;
    Carrier decoded;
    decoded.flags = in[3];
    decoded.oc_level = in[4];
    decoded.normal_points = in[5];
    decoded.over_points = in[6];
    for (std::size_t index = 0; index < stat_count; ++index)
        decoded.delta[index] =
            static_cast<std::int16_t>(Read16(in + 7 + index * 2));
    decoded.revision = Read32(in + 19);
    *carrier = decoded;
    return true;
}

inline void ClearCarrier(std::uint8_t* record) noexcept {
    std::memset(record + kCarrierOffset, 0, kCarrierBytes);
}

inline void ClearCustomSkills(std::uint8_t* record) noexcept {
    std::memset(record + kCustomSkillOffset, 0, kCustomSkillCount * 2);
}

// ---------------------------------------------------------------------------
// Lobby commands (friend-gift extension range 0x1000..0x10FF).

enum class Command : std::uint16_t {
    mode = 0x1000,
    get_profile = 0x1001,
    set_normal = 0x1002,
    set_over = 0x1003,
    upgrade = 0x1004,
    reset_cost = 0x1005,
    reset = 0x1006,
    challenge_items = 0x1007, // OC items the account can use in the next challenge
    owned_items = 0x1008,     // every OC item the account holds (the OC tab's ITEM box)
};

inline constexpr std::uint16_t kFirstCommand = 0x1000;
inline constexpr std::uint16_t kLastCommand = 0x1008;

inline friend_unit_gift::Command Wrap(Command command) noexcept {
    return static_cast<friend_unit_gift::Command>(
        static_cast<std::uint16_t>(command));
}

// Every reply payload starts with the procedure's return value (0 = ok) so a
// business failure travels with WireStatus::success.
inline constexpr std::size_t kReplyHeaderBytes = 4;

struct ModeReply {
    std::uint8_t enabled = 0;
    std::uint8_t oc_mode = 0;
    std::uint16_t schema_version = 0;
    std::uint32_t flags = 0;
};
inline constexpr std::size_t kModeReplyBytes = kReplyHeaderBytes + 8;

struct Profile {
    std::uint64_t unit_uid = 0;
    std::uint32_t unit_number = 0;
    std::uint8_t oc_level = 0;
    std::uint8_t normal_max_lv = 0;
    std::uint8_t state = 0;
    std::uint8_t extra_bonus = 0;
    std::uint32_t revision = 0;
    std::uint8_t normal_points = 0;
    std::array<std::uint8_t, stat_count> normal{};
    std::uint8_t over_points = 0;
    std::array<std::uint8_t, stat_count> over{};
    std::uint8_t native_level = 0;
    std::uint8_t grade = 0;
    std::uint32_t lap_exp = 0;
    std::uint32_t battles = 0;
    std::uint32_t custom_number = 0;
    std::array<std::int32_t, stat_count> base{};
    std::array<std::int32_t, stat_count> normal_stat{};
    std::array<std::int32_t, stat_count> over_stat{};
    std::uint8_t normal_budget = 0;
    std::uint8_t over_budget = 0;
    std::uint8_t next_level = 0;
    std::uint8_t next_give_point = 0;
    std::int32_t next_probability = 0;
    std::int32_t next_battle_cnt = 0;
    std::int32_t level_max_exp = 0;
};
inline constexpr std::size_t kProfileBytes = 136;

inline void WriteProfile(std::uint8_t* out, const Profile& profile) noexcept {
    using friend_unit_gift::wire::Write32;
    using friend_unit_gift::wire::Write64;
    std::memset(out, 0, kProfileBytes);
    Write64(out + 0, profile.unit_uid);
    Write32(out + 8, profile.unit_number);
    out[12] = profile.oc_level;
    out[13] = profile.normal_max_lv;
    out[14] = profile.state;
    out[15] = profile.extra_bonus;
    Write32(out + 16, profile.revision);
    out[20] = profile.normal_points;
    for (std::size_t i = 0; i < stat_count; ++i) out[21 + i] = profile.normal[i];
    out[27] = profile.over_points;
    for (std::size_t i = 0; i < stat_count; ++i) out[28 + i] = profile.over[i];
    out[34] = profile.native_level;
    out[35] = profile.grade;
    Write32(out + 36, profile.lap_exp);
    Write32(out + 40, profile.battles);
    Write32(out + 44, profile.custom_number);
    for (std::size_t i = 0; i < stat_count; ++i) {
        Write32(out + 48 + i * 4, static_cast<std::uint32_t>(profile.base[i]));
        Write32(out + 72 + i * 4, static_cast<std::uint32_t>(profile.normal_stat[i]));
        Write32(out + 96 + i * 4, static_cast<std::uint32_t>(profile.over_stat[i]));
    }
    out[120] = profile.normal_budget;
    out[121] = profile.over_budget;
    out[122] = profile.next_level;
    out[123] = profile.next_give_point;
    Write32(out + 124, static_cast<std::uint32_t>(profile.next_probability));
    Write32(out + 128, static_cast<std::uint32_t>(profile.next_battle_cnt));
    Write32(out + 132, static_cast<std::uint32_t>(profile.level_max_exp));
}

inline void ReadProfile(const std::uint8_t* in, Profile* profile) noexcept {
    using friend_unit_gift::wire::Read32;
    using friend_unit_gift::wire::Read64;
    Profile decoded;
    decoded.unit_uid = Read64(in + 0);
    decoded.unit_number = Read32(in + 8);
    decoded.oc_level = in[12];
    decoded.normal_max_lv = in[13];
    decoded.state = in[14];
    decoded.extra_bonus = in[15];
    decoded.revision = Read32(in + 16);
    decoded.normal_points = in[20];
    for (std::size_t i = 0; i < stat_count; ++i) decoded.normal[i] = in[21 + i];
    decoded.over_points = in[27];
    for (std::size_t i = 0; i < stat_count; ++i) decoded.over[i] = in[28 + i];
    decoded.native_level = in[34];
    decoded.grade = in[35];
    decoded.lap_exp = Read32(in + 36);
    decoded.battles = Read32(in + 40);
    decoded.custom_number = Read32(in + 44);
    for (std::size_t i = 0; i < stat_count; ++i) {
        decoded.base[i] = static_cast<std::int32_t>(Read32(in + 48 + i * 4));
        decoded.normal_stat[i] = static_cast<std::int32_t>(Read32(in + 72 + i * 4));
        decoded.over_stat[i] = static_cast<std::int32_t>(Read32(in + 96 + i * 4));
    }
    decoded.normal_budget = in[120];
    decoded.over_budget = in[121];
    decoded.next_level = in[122];
    decoded.next_give_point = in[123];
    decoded.next_probability = static_cast<std::int32_t>(Read32(in + 124));
    decoded.next_battle_cnt = static_cast<std::int32_t>(Read32(in + 128));
    decoded.level_max_exp = static_cast<std::int32_t>(Read32(in + 132));
    *profile = decoded;
}

// Total of one stat after allocation, as the original rules compute it.
inline std::int32_t StatTotal(const Profile& profile, std::size_t stat) noexcept {
    return profile.base[stat] +
           profile.normal[stat] * profile.normal_stat[stat] +
           profile.over[stat] * profile.over_stat[stat];
}

// Requests ------------------------------------------------------------------

struct UnitRequest { // get_profile
    std::uint64_t unit_uid = 0;
};
inline constexpr std::size_t kUnitRequestBytes = 8;

struct SetRequest { // set_normal / set_over: absolute allocation vector
    std::uint64_t unit_uid = 0;
    std::array<std::uint8_t, stat_count> values{};
    std::uint32_t revision = 0;
};
inline constexpr std::size_t kSetRequestBytes = 20;

struct UpgradeRequest { // upgrade: optional challenge items
    std::uint64_t unit_uid = 0;
    std::array<std::int32_t, 3> item{};
    std::array<std::int16_t, 3> count{};
};
inline constexpr std::size_t kUpgradeRequestBytes = 28;

struct WorkRequest { // reset_cost / reset: work 1 normal, 2 over
    std::uint64_t unit_uid = 0;
    std::uint8_t work = 0;
};
inline constexpr std::size_t kWorkRequestBytes = 12;

inline void WriteUnitRequest(std::uint8_t* out, const UnitRequest& request) noexcept {
    friend_unit_gift::wire::Write64(out, request.unit_uid);
}
inline bool ReadUnitRequest(const std::uint8_t* in, std::size_t size, UnitRequest* request) noexcept {
    if (size != kUnitRequestBytes) return false;
    request->unit_uid = friend_unit_gift::wire::Read64(in);
    return request->unit_uid != 0;
}

inline void WriteSetRequest(std::uint8_t* out, const SetRequest& request) noexcept {
    std::memset(out, 0, kSetRequestBytes);
    friend_unit_gift::wire::Write64(out, request.unit_uid);
    for (std::size_t i = 0; i < stat_count; ++i) out[8 + i] = request.values[i];
    friend_unit_gift::wire::Write32(out + 16, request.revision);
}
inline bool ReadSetRequest(const std::uint8_t* in, std::size_t size, SetRequest* request) noexcept {
    if (size != kSetRequestBytes || in[14] != 0 || in[15] != 0) return false;
    request->unit_uid = friend_unit_gift::wire::Read64(in);
    for (std::size_t i = 0; i < stat_count; ++i) request->values[i] = in[8 + i];
    request->revision = friend_unit_gift::wire::Read32(in + 16);
    return request->unit_uid != 0;
}

inline void WriteUpgradeRequest(std::uint8_t* out, const UpgradeRequest& request) noexcept {
    std::memset(out, 0, kUpgradeRequestBytes);
    friend_unit_gift::wire::Write64(out, request.unit_uid);
    for (std::size_t i = 0; i < 3; ++i) {
        friend_unit_gift::wire::Write32(out + 8 + i * 4, static_cast<std::uint32_t>(request.item[i]));
        friend_unit_gift::wire::Write16(out + 20 + i * 2, static_cast<std::uint16_t>(request.count[i]));
    }
}
inline bool ReadUpgradeRequest(const std::uint8_t* in, std::size_t size, UpgradeRequest* request) noexcept {
    if (size != kUpgradeRequestBytes || in[26] != 0 || in[27] != 0) return false;
    request->unit_uid = friend_unit_gift::wire::Read64(in);
    for (std::size_t i = 0; i < 3; ++i) {
        request->item[i] = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in + 8 + i * 4));
        request->count[i] = static_cast<std::int16_t>(friend_unit_gift::wire::Read16(in + 20 + i * 2));
        if (request->item[i] < 0 || request->count[i] < 0) return false;
    }
    return request->unit_uid != 0;
}

inline void WriteWorkRequest(std::uint8_t* out, const WorkRequest& request) noexcept {
    std::memset(out, 0, kWorkRequestBytes);
    friend_unit_gift::wire::Write64(out, request.unit_uid);
    out[8] = request.work;
}
inline bool ReadWorkRequest(const std::uint8_t* in, std::size_t size, WorkRequest* request) noexcept {
    if (size != kWorkRequestBytes || in[9] != 0 || in[10] != 0 || in[11] != 0) return false;
    request->unit_uid = friend_unit_gift::wire::Read64(in);
    request->work = in[8];
    return request->unit_uid != 0 && (request->work == 1 || request->work == 2);
}

// Replies -------------------------------------------------------------------

struct SetReply {
    std::int32_t ret_val = 0;
    std::uint8_t remaining = 0;
    Profile profile{};
};
inline constexpr std::size_t kSetReplyBytes = kReplyHeaderBytes + 4 + kProfileBytes;

struct UpgradeReply {
    std::int32_t ret_val = 0;
    std::uint8_t success = 0;
    std::uint8_t lv_prevention = 0;
    std::uint8_t now_lv = 0;
    std::uint8_t next_lv = 0;
    std::uint8_t oc_level = 0;
    std::uint8_t over_points = 0;
    std::array<std::int16_t, 3> item_left{};
    Profile profile{};
};
inline constexpr std::size_t kUpgradeReplyBytes = kReplyHeaderBytes + 12 + kProfileBytes;

struct ResetCostReply {
    std::int32_t ret_val = 0;
    std::uint8_t unit_rank = 0;
    std::int32_t reset_gp = 0;
};
inline constexpr std::size_t kResetCostReplyBytes = kReplyHeaderBytes + 8;

struct ResetReply {
    std::int32_t ret_val = 0;
    std::uint8_t normal_points = 0;
    std::uint8_t over_points = 0;
    std::int32_t charged_gp = 0;
    Profile profile{};
};
inline constexpr std::size_t kResetReplyBytes = kReplyHeaderBytes + 8 + kProfileBytes;

inline constexpr std::size_t kProfileReplyBytes = kReplyHeaderBytes + kProfileBytes;

inline void WriteModeReply(std::uint8_t* out, std::int32_t ret_val, const ModeReply& reply) noexcept {
    using friend_unit_gift::wire::Write16;
    using friend_unit_gift::wire::Write32;
    std::memset(out, 0, kModeReplyBytes);
    Write32(out, static_cast<std::uint32_t>(ret_val));
    out[4] = reply.enabled;
    out[5] = reply.oc_mode;
    Write16(out + 6, reply.schema_version);
    Write32(out + 8, reply.flags);
}
inline bool ReadModeReply(const std::uint8_t* in, std::size_t size, std::int32_t* ret_val, ModeReply* reply) noexcept {
    if (size != kModeReplyBytes) return false;
    *ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    reply->enabled = in[4];
    reply->oc_mode = in[5];
    reply->schema_version = friend_unit_gift::wire::Read16(in + 6);
    reply->flags = friend_unit_gift::wire::Read32(in + 8);
    return true;
}

inline void WriteProfileReply(std::uint8_t* out, std::int32_t ret_val, const Profile& profile) noexcept {
    friend_unit_gift::wire::Write32(out, static_cast<std::uint32_t>(ret_val));
    WriteProfile(out + kReplyHeaderBytes, profile);
}
inline bool ReadProfileReply(const std::uint8_t* in, std::size_t size, std::int32_t* ret_val, Profile* profile) noexcept {
    if (size != kProfileReplyBytes) return false;
    *ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    ReadProfile(in + kReplyHeaderBytes, profile);
    return true;
}

inline void WriteSetReply(std::uint8_t* out, const SetReply& reply) noexcept {
    std::memset(out, 0, kSetReplyBytes);
    friend_unit_gift::wire::Write32(out, static_cast<std::uint32_t>(reply.ret_val));
    out[4] = reply.remaining;
    WriteProfile(out + 8, reply.profile);
}
inline bool ReadSetReply(const std::uint8_t* in, std::size_t size, SetReply* reply) noexcept {
    if (size != kSetReplyBytes) return false;
    reply->ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    reply->remaining = in[4];
    ReadProfile(in + 8, &reply->profile);
    return true;
}

inline void WriteUpgradeReply(std::uint8_t* out, const UpgradeReply& reply) noexcept {
    std::memset(out, 0, kUpgradeReplyBytes);
    friend_unit_gift::wire::Write32(out, static_cast<std::uint32_t>(reply.ret_val));
    out[4] = reply.success;
    out[5] = reply.lv_prevention;
    out[6] = reply.now_lv;
    out[7] = reply.next_lv;
    out[8] = reply.oc_level;
    out[9] = reply.over_points;
    for (std::size_t i = 0; i < 3; ++i)
        friend_unit_gift::wire::Write16(out + 10 + i * 2, static_cast<std::uint16_t>(reply.item_left[i]));
    WriteProfile(out + 16, reply.profile);
}
inline bool ReadUpgradeReply(const std::uint8_t* in, std::size_t size, UpgradeReply* reply) noexcept {
    if (size != kUpgradeReplyBytes) return false;
    reply->ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    reply->success = in[4];
    reply->lv_prevention = in[5];
    reply->now_lv = in[6];
    reply->next_lv = in[7];
    reply->oc_level = in[8];
    reply->over_points = in[9];
    for (std::size_t i = 0; i < 3; ++i)
        reply->item_left[i] = static_cast<std::int16_t>(friend_unit_gift::wire::Read16(in + 10 + i * 2));
    ReadProfile(in + 16, &reply->profile);
    return true;
}

inline void WriteResetCostReply(std::uint8_t* out, const ResetCostReply& reply) noexcept {
    std::memset(out, 0, kResetCostReplyBytes);
    friend_unit_gift::wire::Write32(out, static_cast<std::uint32_t>(reply.ret_val));
    out[4] = reply.unit_rank;
    friend_unit_gift::wire::Write32(out + 8, static_cast<std::uint32_t>(reply.reset_gp));
}
inline bool ReadResetCostReply(const std::uint8_t* in, std::size_t size, ResetCostReply* reply) noexcept {
    if (size != kResetCostReplyBytes) return false;
    reply->ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    reply->unit_rank = in[4];
    reply->reset_gp = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in + 8));
    return true;
}

inline void WriteResetReply(std::uint8_t* out, const ResetReply& reply) noexcept {
    std::memset(out, 0, kResetReplyBytes);
    friend_unit_gift::wire::Write32(out, static_cast<std::uint32_t>(reply.ret_val));
    out[4] = reply.normal_points;
    out[5] = reply.over_points;
    friend_unit_gift::wire::Write32(out + 8, static_cast<std::uint32_t>(reply.charged_gp));
    WriteProfile(out + 12, reply.profile);
}
inline bool ReadResetReply(const std::uint8_t* in, std::size_t size, ResetReply* reply) noexcept {
    if (size != kResetReplyBytes) return false;
    reply->ret_val = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in));
    reply->normal_points = in[4];
    reply->over_points = in[5];
    reply->charged_gp = static_cast<std::int32_t>(friend_unit_gift::wire::Read32(in + 8));
    ReadProfile(in + 12, &reply->profile);
    return true;
}

// challenge_items: the account's OC challenge items (tbl_EtcItems with a
// success bonus or level protection) usable at the unit's current level, as
// listed in the original OC challenge window (ten check boxes).
inline constexpr std::size_t kChallengeItemMax = 10;
struct ChallengeItem {
    std::int32_t item = 0;
    std::int16_t count = 0;
    std::int16_t probability_plus = 0;
    std::uint8_t lv_prevention = 0;
    std::uint8_t min_level = 0;
    std::uint8_t max_level = 0;
};
inline constexpr std::size_t kChallengeItemBytes = 12;
struct ChallengeItemsReply {
    std::int32_t ret_val = 0;
    std::uint8_t count = 0;
    std::array<ChallengeItem, kChallengeItemMax> items{};
};
inline constexpr std::size_t kChallengeItemsReplyBytes = kReplyHeaderBytes + 4 + kChallengeItemMax * kChallengeItemBytes;

inline void WriteChallengeItemsReply(std::uint8_t* out, const ChallengeItemsReply& reply) noexcept {
    using friend_unit_gift::wire::Write16;
    using friend_unit_gift::wire::Write32;
    std::memset(out, 0, kChallengeItemsReplyBytes);
    Write32(out, static_cast<std::uint32_t>(reply.ret_val));
    out[4] = reply.count > kChallengeItemMax ? static_cast<std::uint8_t>(kChallengeItemMax) : reply.count;
    for (std::size_t i = 0; i < out[4]; ++i) {
        auto* entry = out + 8 + i * kChallengeItemBytes;
        Write32(entry, static_cast<std::uint32_t>(reply.items[i].item));
        Write16(entry + 4, static_cast<std::uint16_t>(reply.items[i].count));
        Write16(entry + 6, static_cast<std::uint16_t>(reply.items[i].probability_plus));
        entry[8] = reply.items[i].lv_prevention;
        entry[9] = reply.items[i].min_level;
        entry[10] = reply.items[i].max_level;
    }
}
inline bool ReadChallengeItemsReply(const std::uint8_t* in, std::size_t size, ChallengeItemsReply* reply) noexcept {
    using friend_unit_gift::wire::Read16;
    using friend_unit_gift::wire::Read32;
    if (size != kChallengeItemsReplyBytes || in[4] > kChallengeItemMax) return false;
    reply->ret_val = static_cast<std::int32_t>(Read32(in));
    reply->count = in[4];
    for (std::size_t i = 0; i < reply->count; ++i) {
        const auto* entry = in + 8 + i * kChallengeItemBytes;
        reply->items[i].item = static_cast<std::int32_t>(Read32(entry));
        reply->items[i].count = static_cast<std::int16_t>(Read16(entry + 4));
        reply->items[i].probability_plus = static_cast<std::int16_t>(Read16(entry + 6));
        reply->items[i].lv_prevention = entry[8];
        reply->items[i].min_level = entry[9];
        reply->items[i].max_level = entry[10];
        if (reply->items[i].item <= 0 || reply->items[i].count <= 0) return false;
    }
    return true;
}

// Return values that the lobby shows (the original procedures' codes).
enum ResetCode : std::int32_t {
    reset_ok = 0,
    reset_insufficient_gp = 100, // added by the Mod: the GP charge failed
};

static_assert(kProfileReplyBytes <= friend_unit_gift::kMaximumExtensionPayloadBytes,
              "profile reply exceeds the extension payload");
static_assert(kUpgradeReplyBytes <= friend_unit_gift::kMaximumExtensionPayloadBytes,
              "upgrade reply exceeds the extension payload");
static_assert(kChallengeItemsReplyBytes <= friend_unit_gift::kMaximumExtensionPayloadBytes,
              "challenge item reply exceeds the extension payload");

} // namespace sdgo::oc_custom
