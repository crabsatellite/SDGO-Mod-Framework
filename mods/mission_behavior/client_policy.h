#pragma once

#include "sdgo/mission_behavior_profile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sdgo::mission_behavior {

enum class RecordResult : std::uint32_t {
    Ignored = 0,
    Unchanged = 1,
    Applied = 2,
    Restored = 3,
    Conflict = 4,
};

constexpr std::uint32_t kNpcRecordKind = 41;
constexpr std::size_t kNpcRecordBytes = 598;
constexpr std::size_t kNpcIdOffset = 0;
constexpr std::size_t kNpcAiNameOffset = 0x6D;
constexpr std::size_t kNpcAiNameBytes = 100;
constexpr std::size_t kNpcAiNameCharacters =
    kNpcAiNameBytes / sizeof(std::uint16_t);
constexpr const char* kGcoM1BossAi = "MM001_NpcBoss001E";

struct M1Binding {
    std::uint32_t npc_id;
    const char* oc_ai;
};

constexpr std::array<M1Binding, 12> kM1Bindings{{
    {85054, "MM001_NpcBoss001"},
    {85058, "MM001_NpcBoss001"},
    {85062, "MM001_NpcBoss001"},
    {85079, "MM001_NpcBoss002"},
    {85080, "MM001_NpcBoss002"},
    {85081, "MM001_NpcBoss001"},
    {85082, "MM001_NpcBoss001"},
    {85083, "MM001_NpcBoss001"},
    {85084, "MM001_NpcBoss001"},
    {85169, "MM001_NpcBoss001"},
    {85170, "MM001_NpcBoss001"},
    {85171, "MM001_NpcBoss001"},
}};

struct RecordDecision {
    RecordResult result = RecordResult::Ignored;
    std::uint32_t npc_id = 0;
    std::array<char, kNpcAiNameCharacters> before{};
    std::array<char, kNpcAiNameCharacters> after{};
};

struct M1SetPlan {
    bool valid = false;
    std::uint32_t conflict_npc_id = 0;
    std::uint32_t changed = 0;
    std::uint32_t unchanged = 0;
    std::array<RecordDecision, kM1Bindings.size()> records{};
};

inline bool EqualAsciiInsensitive(const char* left, const char* right) {
    if (!left || !right) return false;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return *left == *right;
}

inline bool ParseClientProfile(const char* text, Profile& output) {
    if (EqualAsciiInsensitive(text, "oc")) {
        output = Profile::Oc;
        return true;
    }
    if (EqualAsciiInsensitive(text, "gco")) {
        output = Profile::Gco;
        return true;
    }
    return false;
}

inline std::uint32_t ProfileWireValue(Profile profile) {
    return profile == Profile::Oc ? 1u : 2u;
}

inline const M1Binding* FindM1Binding(std::uint32_t npc_id) {
    for (const auto& binding : kM1Bindings)
        if (binding.npc_id == npc_id) return &binding;
    return nullptr;
}

inline std::uint32_t ReadU32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

inline bool ReadAiName(
    const std::uint8_t* record,
    std::size_t record_bytes,
    std::array<char, kNpcAiNameCharacters>& output) {
    output.fill('\0');
    if (!record || record_bytes != kNpcRecordBytes) return false;
    bool terminated = false;
    for (std::size_t index = 0; index < kNpcAiNameCharacters; ++index) {
        std::uint16_t code = 0;
        std::memcpy(
            &code,
            record + kNpcAiNameOffset + index * sizeof(code),
            sizeof(code));
        if (!code) {
            terminated = true;
            break;
        }
        if (code < 0x20 || code > 0x7E ||
            index + 1 == kNpcAiNameCharacters) {
            output.fill('\0');
            return false;
        }
        output[index] = static_cast<char>(code);
    }
    return terminated;
}

inline bool WriteAiName(
    std::uint8_t* record,
    std::size_t record_bytes,
    const char* value) {
    if (!record || record_bytes != kNpcRecordBytes || !value) return false;
    const std::size_t length = std::strlen(value);
    if (length >= kNpcAiNameCharacters) return false;
    for (std::size_t index = 0; index < length; ++index)
        if (static_cast<unsigned char>(value[index]) < 0x20 ||
            static_cast<unsigned char>(value[index]) > 0x7E)
            return false;
    std::memset(record + kNpcAiNameOffset, 0, kNpcAiNameBytes);
    for (std::size_t index = 0; index < length; ++index) {
        const std::uint16_t code =
            static_cast<unsigned char>(value[index]);
        std::memcpy(
            record + kNpcAiNameOffset + index * sizeof(code),
            &code,
            sizeof(code));
    }
    return true;
}

inline RecordDecision PlanM1Binding(
    const std::uint8_t* record,
    std::size_t record_bytes,
    Profile profile) {
    RecordDecision decision;
    if (!record || record_bytes < sizeof(std::uint32_t))
        return decision;
    decision.npc_id = ReadU32(record + kNpcIdOffset);
    const M1Binding* binding = FindM1Binding(decision.npc_id);
    if (!binding) return decision;
    if (!ReadAiName(record, record_bytes, decision.before)) {
        decision.result = RecordResult::Conflict;
        return decision;
    }

    const char* desired =
        profile == Profile::Oc ? binding->oc_ai : kGcoM1BossAi;
    if (std::strcmp(decision.before.data(), desired) == 0) {
        std::memcpy(
            decision.after.data(),
            decision.before.data(),
            decision.before.size());
        decision.result = RecordResult::Unchanged;
        return decision;
    }

    const char* accepted =
        profile == Profile::Oc ? kGcoM1BossAi : binding->oc_ai;
    if (std::strcmp(decision.before.data(), accepted) != 0) {
        decision.after.fill('\0');
        decision.result = RecordResult::Conflict;
        return decision;
    }
    std::memcpy(
        decision.after.data(),
        desired,
        std::strlen(desired) + 1);
    decision.result =
        profile == Profile::Oc ? RecordResult::Applied
                               : RecordResult::Restored;
    return decision;
}

inline RecordDecision ApplyM1Binding(
    std::uint8_t* record,
    std::size_t record_bytes,
    Profile profile) {
    RecordDecision decision = PlanM1Binding(record, record_bytes, profile);
    if (decision.result != RecordResult::Applied &&
        decision.result != RecordResult::Restored)
        return decision;
    if (!WriteAiName(record, record_bytes, decision.after.data())) {
        decision.after.fill('\0');
        decision.result = RecordResult::Conflict;
        return decision;
    }
    std::array<char, kNpcAiNameCharacters> readback{};
    if (!ReadAiName(record, record_bytes, readback) ||
        std::strcmp(readback.data(), decision.after.data()) != 0) {
        WriteAiName(record, record_bytes, decision.before.data());
        decision.after.fill('\0');
        decision.result = RecordResult::Conflict;
        return decision;
    }
    return decision;
}

inline M1SetPlan PlanM1Set(
    const std::array<const std::uint8_t*, kM1Bindings.size()>& records,
    std::size_t record_bytes,
    Profile profile) {
    M1SetPlan plan;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto decision =
            PlanM1Binding(records[index], record_bytes, profile);
        plan.records[index] = decision;
        if (decision.npc_id != kM1Bindings[index].npc_id ||
            decision.result == RecordResult::Ignored ||
            decision.result == RecordResult::Conflict) {
            plan.conflict_npc_id =
                decision.npc_id ? decision.npc_id
                                : kM1Bindings[index].npc_id;
            return plan;
        }
        if (decision.result == RecordResult::Unchanged)
            ++plan.unchanged;
        else
            ++plan.changed;
    }
    plan.valid = true;
    return plan;
}

inline bool RestoreM1Binding(
    std::uint8_t* record,
    std::size_t record_bytes,
    const RecordDecision& decision) {
    if (!WriteAiName(record, record_bytes, decision.before.data()))
        return false;
    std::array<char, kNpcAiNameCharacters> readback{};
    return ReadAiName(record, record_bytes, readback) &&
        std::strcmp(readback.data(), decision.before.data()) == 0;
}

inline bool ApplyM1Plan(
    std::uint8_t* record,
    std::size_t record_bytes,
    const RecordDecision& decision) {
    if (decision.result == RecordResult::Unchanged) return true;
    if (decision.result != RecordResult::Applied &&
        decision.result != RecordResult::Restored)
        return false;
    if (!WriteAiName(record, record_bytes, decision.after.data())) {
        return false;
    }
    std::array<char, kNpcAiNameCharacters> readback{};
    return ReadAiName(record, record_bytes, readback) &&
        std::strcmp(readback.data(), decision.after.data()) == 0;
}

} // namespace sdgo::mission_behavior
