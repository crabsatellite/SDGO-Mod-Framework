#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include "sdgo/ace_ai_observer.h"

namespace sdgo_game_test_ace {

inline std::string Utf8(const wchar_t* text, size_t capacity) {
    if (!text || !capacity) return {};
    const size_t length = wcsnlen_s(text, capacity);
    if (!length) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string output(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(
            CP_UTF8, 0, text, static_cast<int>(length), output.data(), size,
            nullptr, nullptr))
        return {};
    return output;
}

inline std::string Quote(const std::string& text) {
    std::string output = "\"";
    for (const unsigned char value : text) {
        switch (value) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (value < 0x20) {
                char escaped[7]{};
                sprintf_s(escaped, "\\u%04x", value);
                output += escaped;
            } else output += static_cast<char>(value);
        }
    }
    return output + '"';
}

inline void Number(std::string& json, const char* name, uint64_t value) {
    json += ",\"" + std::string(name) + "\":" + std::to_string(value);
}
inline void Signed(std::string& json, const char* name, int64_t value) {
    json += ",\"" + std::string(name) + "\":" + std::to_string(value);
}

inline std::string ClientUnit(const SdgoAceAiClientUnitV1& unit) {
    std::string json = "{\"pilot_id\":" + std::to_string(unit.pilot_id);
    Number(json, "native_pilot_id", unit.native_pilot_id);
    Number(json, "object_id", unit.object_id);
    Number(json, "npc_id", unit.npc_id);
    Number(json, "machine_id", unit.machine_id);
    Number(json, "generation", unit.generation);
    Number(json, "current_hp", unit.current_hp);
    Number(json, "max_hp", unit.max_hp);
    Signed(json, "slot", unit.slot);
    Signed(json, "form", unit.form);
    Number(json, "base_difficulty", unit.base_difficulty);
    Number(json, "effective_difficulty", unit.effective_difficulty);
    Number(json, "ace", unit.ace);
    Number(json, "team", unit.team);
    Number(json, "colour", unit.colour);
    Number(json, "active", unit.active);
    Number(json, "name_matches", unit.name_matches);
    Number(json, "slot_name_valid", unit.slot_name_valid);
    json += ",\"expected_name\":" + Quote(Utf8(unit.expected_name, _countof(unit.expected_name)));
    json += ",\"slot_name\":" + Quote(Utf8(unit.slot_name, _countof(unit.slot_name))) + '}';
    return json;
}

inline void ClientUnits(std::string& json, const SdgoAceAiClientUnitV1* units, uint32_t count) {
    json += ",\"units\":[";
    for (uint32_t i = 0; i < count && i < 12; ++i) {
        if (i) json += ',';
        json += ClientUnit(units[i]);
    }
    json += ']';
}

inline std::string PvpClient(const SdgoAceAiPvpClientSnapshotV1& snapshot) {
    std::string json = "{\"kind\":\"client\",\"scope\":\"pvp\"";
    Number(json, "battle_sequence", snapshot.battle_sequence);
    Number(json, "unit_count", snapshot.unit_count);
    ClientUnits(json, snapshot.units, snapshot.unit_count);
    return "ace-v1 " + json + '}';
}

inline std::string MissionClient(const SdgoAceAiMissionClientSnapshotV1& snapshot) {
    std::string json = "{\"kind\":\"client\",\"scope\":\"mission\"";
    Number(json, "battle_generation", snapshot.battle_generation);
    Number(json, "unit_count", snapshot.unit_count);
    ClientUnits(json, snapshot.units, snapshot.unit_count);
    return "ace-v1 " + json + '}';
}

inline std::string Announcement(const char* scope, const SdgoAceAiAnnouncementSnapshotV1& snapshot) {
    std::string json = "{\"kind\":\"announcement\",\"scope\":" + Quote(scope);
    Number(json, "sequence", snapshot.sequence);
    Number(json, "source", snapshot.source);
    Number(json, "colour", snapshot.colour);
    Number(json, "feed_count_before", snapshot.feed_count_before);
    Number(json, "feed_count_after", snapshot.feed_count_after);
    Number(json, "successful_count", snapshot.successful_count);
    json += ",\"text\":" + Quote(Utf8(snapshot.text, _countof(snapshot.text))) + '}';
    return "ace-v1 " + json;
}

inline std::string Playback(const SdgoAceAiMissionClientSnapshotV1& snapshot) {
    std::string json = "{\"kind\":\"playback\",\"scope\":\"mission\"";
    Number(json, "playback_sequence", snapshot.playback_sequence);
    Number(json, "playback_object_id", snapshot.playback_object_id);
    Number(json, "playback_generation", snapshot.playback_generation);
    Number(json, "playback_pilot_id", snapshot.playback_pilot_id);
    Number(json, "playback_machine_id", snapshot.playback_machine_id);
    Signed(json, "playback_slot", snapshot.playback_slot);
    Number(json, "playback_colour", snapshot.playback_colour);
    Number(json, "playback_native_result", snapshot.playback_native_result);
    Number(json, "playback_name_refreshed", snapshot.playback_name_refreshed);
    return "ace-v1 " + json + '}';
}

inline std::string ServerUnit(const SdgoAceAiServerUnitV1& unit) {
    std::string json = "{\"pilot_id\":" + std::to_string(unit.pilot_id);
    Number(json, "native_pilot_id", unit.native_pilot_id);
    Number(json, "npc_id", unit.npc_id);
    Number(json, "machine_id", unit.machine_id);
    Number(json, "generation", unit.generation);
    Number(json, "current_hp", unit.current_hp);
    Number(json, "max_hp", unit.max_hp);
    Number(json, "original_max_hp", unit.original_max_hp);
    Number(json, "hp_percent", unit.hp_percent);
    Number(json, "attack_percent", unit.attack_percent);
    Signed(json, "slot", unit.slot);
    Signed(json, "form", unit.form);
    Number(json, "base_difficulty", unit.base_difficulty);
    Number(json, "effective_difficulty", unit.effective_difficulty);
    Number(json, "ace", unit.ace);
    Number(json, "team", unit.team);
    Number(json, "colour", unit.colour);
    Number(json, "active", unit.active);
    return json + '}';
}

inline std::string Damage(const SdgoAceAiDamageDecisionV1& damage) {
    std::string json = "{\"sequence\":" + std::to_string(damage.sequence);
    Number(json, "room_cookie", damage.room_cookie);
    Number(json, "generation", damage.generation);
    Number(json, "attacker_pilot_id", damage.attacker_pilot_id);
    Number(json, "native_pilot_id", damage.native_pilot_id);
    Number(json, "attack_type", damage.attack_type);
    Number(json, "proposed_damage", damage.proposed_damage);
    Number(json, "adjusted_damage", damage.adjusted_damage);
    Number(json, "native_result", damage.native_result);
    Number(json, "attack_percent", damage.attack_percent);
    Number(json, "source", damage.source);
    Number(json, "ace", damage.ace);
    Number(json, "policy_matched", damage.policy_matched);
    return json + '}';
}

inline std::string Server(const char* scope, const SdgoAceAiServerSnapshotV1& snapshot) {
    std::string json = "{\"kind\":\"server\",\"scope\":" + Quote(scope);
    Number(json, "room_cookie", snapshot.room_cookie);
    Number(json, "mode", snapshot.mode);
    Number(json, "generation", snapshot.generation);
    Number(json, "unit_count", snapshot.unit_count);
    json += ",\"units\":[";
    for (uint32_t i = 0; i < snapshot.unit_count && i < 12; ++i) {
        if (i) json += ',';
        json += ServerUnit(snapshot.units[i]);
    }
    json += "],\"damage\":" + Damage(snapshot.damage) + '}';
    return "ace-v1 " + json;
}

} // namespace sdgo_game_test_ace
