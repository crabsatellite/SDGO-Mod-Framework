#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sdgo/battle_reward_stats.h"
#include "sdgo/operator_effects.h"
#include "sdgo/operator_reward_filter.h"
#include "sdgo/sdgo_api.h"

#include "../battle_rewards/native_sites.h"
#include "../battle_rewards/reward_filter.h"
#include "../battle_rewards/site_bridge.h"

namespace {
namespace fs = std::filesystem;
using namespace sdgo::battle_rewards;

constexpr uint32_t kModuleVersion = 1;
constexpr char kMissionFinalSymbol[] = "battle.rewards.final.mission";
constexpr ptrdiff_t kBattlePilotOperatorDefinition = 0x50;
constexpr ptrdiff_t kBattlePilotWinSkill = 0x54;
constexpr ptrdiff_t kBattlePilotOtherSkill = 0x58;
constexpr ptrdiff_t kBattlePilotGeneralSkill = 0x5C;
constexpr ptrdiff_t kDefinitionTimedSkills = 0x38;
constexpr ptrdiff_t kTimedStartHour = 0x08;
constexpr ptrdiff_t kTimedStartMinute = 0x09;
constexpr ptrdiff_t kTimedEndHour = 0x0A;
constexpr ptrdiff_t kTimedEndMinute = 0x0B;
constexpr ptrdiff_t kTimedSkill = 0x0C;
constexpr ptrdiff_t kSkillPointAdd = 0x08;
constexpr size_t kMaximumProfiles = 512;
constexpr size_t kMaximumExtensions = 16;
constexpr size_t kMaximumHandlers = 32;
constexpr size_t kReplayCapacity = 4096;

struct ExtensionSpec {
    std::string name;
    std::string payload;
};

struct Profile {
    uint32_t operator_number = 0;
    uint32_t pilot_exp_percent = 0;
    std::vector<ExtensionSpec> extensions;
};

struct ExtensionHandler {
    bool used = false;
    char name[65] = {};
    SdgoOperatorEffectExtensionCallbackV1 callback = nullptr;
    void* user = nullptr;
};

struct SettlementIdentity {
    BYTE* room = nullptr;
    uint32_t room_id = 0;
    uint32_t started = 0;
    uint32_t account = 0;
};

struct ReplayRecord {
    SettlementIdentity identity;
    uint8_t phases = 0;
    bool mission = false;
    uint64_t serial = 0;
};

struct MissionPending {
    bool active = false;
    bool identity_read = false;
    BYTE* frame = nullptr;
    BYTE* room = nullptr;
    BYTE* room_pilot = nullptr;
    BYTE* battle_pilot = nullptr;
    void* operator_definition = nullptr;
    uint32_t account = 0;
    SettlementIdentity identity;
};

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
bool g_trace_settlements = false;
volatile LONG g_trace_count = 0;
std::vector<Profile> g_profiles;
std::string g_revision;
std::wstring g_directory;
std::array<ExtensionHandler, kMaximumHandlers> g_handlers{};
SRWLOCK g_handlerLock = SRWLOCK_INIT;
SRWLOCK g_replayLock = SRWLOCK_INIT;
std::array<ReplayRecord, kReplayCapacity> g_replays{};
size_t g_replayCount = 0;
uint64_t g_replaySerial = 0;
volatile LONG g_contractFailure = 0;
thread_local MissionPending g_missionPending;

template <typename T>
bool SafeRead(const void* base, ptrdiff_t offset, T* value) {
    if (!base || !value) return false;
    const BYTE* address = nullptr;
    if (!native_stats::AddOffset(base, offset, &address)) return false;
    __try {
        std::memcpy(value, address, sizeof(T));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsHex(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

int HexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool ValidUtf8(const std::string& value) {
    size_t position = 0;
    while (position < value.size()) {
        const unsigned char first =
            static_cast<unsigned char>(value[position++]);
        if (first < 0x80) continue;
        unsigned count = 0;
        uint32_t code = 0;
        uint32_t minimum = 0;
        if ((first & 0xE0) == 0xC0) {
            count = 1;
            code = first & 0x1F;
            minimum = 0x80;
        } else if ((first & 0xF0) == 0xE0) {
            count = 2;
            code = first & 0x0F;
            minimum = 0x800;
        } else if ((first & 0xF8) == 0xF0) {
            count = 3;
            code = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (position + count > value.size()) return false;
        for (unsigned i = 0; i < count; ++i) {
            const unsigned char next =
                static_cast<unsigned char>(value[position++]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
    }
    return true;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    bool Extensions(std::vector<ExtensionSpec>* result) {
        if (!result || !ValidUtf8(text_)) return false;
        result->clear();
        Space();
        if (!Take('{')) return false;
        Space();
        if (Take('}')) return End();
        std::set<std::string> names;
        for (;;) {
            std::string name;
            if (!String(&name) || !ValidNamespace(name) ||
                !names.insert(name).second ||
                result->size() >= kMaximumExtensions) {
                return false;
            }
            Space();
            if (!Take(':')) return false;
            Space();
            const size_t start = position_;
            if (!Value(0)) return false;
            const size_t finish = position_;
            result->push_back(
                {std::move(name), text_.substr(start, finish - start)});
            Space();
            if (Take('}')) return End();
            if (!Take(',')) return false;
            Space();
        }
    }

private:
    bool End() {
        Space();
        return position_ == text_.size();
    }

    void Space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool Take(char value) {
        if (position_ >= text_.size() || text_[position_] != value) {
            return false;
        }
        ++position_;
        return true;
    }

    bool String(std::string* decoded = nullptr) {
        if (!Take('"')) return false;
        if (decoded) decoded->clear();
        while (position_ < text_.size()) {
            const unsigned char value =
                static_cast<unsigned char>(text_[position_++]);
            if (value == '"') return true;
            if (value < 0x20) return false;
            if (value != '\\') {
                if (decoded) decoded->push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escape = text_[position_++];
            char translated = 0;
            switch (escape) {
                case '"': translated = '"'; break;
                case '\\': translated = '\\'; break;
                case '/': translated = '/'; break;
                case 'b': translated = '\b'; break;
                case 'f': translated = '\f'; break;
                case 'n': translated = '\n'; break;
                case 'r': translated = '\r'; break;
                case 't': translated = '\t'; break;
                case 'u': {
                    if (position_ + 4 > text_.size()) return false;
                    unsigned code = 0;
                    for (unsigned i = 0; i < 4; ++i) {
                        const int digit = HexValue(text_[position_++]);
                        if (digit < 0) return false;
                        code = code * 16 + static_cast<unsigned>(digit);
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (position_ + 6 > text_.size() ||
                            text_[position_] != '\\' ||
                            text_[position_ + 1] != 'u') {
                            return false;
                        }
                        position_ += 2;
                        unsigned low = 0;
                        for (unsigned i = 0; i < 4; ++i) {
                            const int digit = HexValue(text_[position_++]);
                            if (digit < 0) return false;
                            low = low * 16 + static_cast<unsigned>(digit);
                        }
                        if (low < 0xDC00 || low > 0xDFFF) return false;
                        if (decoded) return false;
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false;
                    }
                    if (decoded) {
                        if (code > 0x7F) return false;
                        translated = static_cast<char>(code);
                    }
                    break;
                }
                default: return false;
            }
            if (decoded) decoded->push_back(translated);
        }
        return false;
    }

    bool Number() {
        const size_t start = position_;
        if (position_ < text_.size() && text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return false;
        if (text_[position_] == '0') {
            ++position_;
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') return false;
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const size_t digits = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) return false;
        }
        if (position_ < text_.size() &&
            (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() &&
                (text_[position_] == '+' || text_[position_] == '-')) {
                ++position_;
            }
            const size_t digits = position_;
            while (position_ < text_.size() &&
                   text_[position_] >= '0' && text_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) return false;
        }
        return position_ > start;
    }

    bool Literal(const char* value) {
        const size_t count = std::strlen(value);
        if (position_ + count > text_.size() ||
            text_.compare(position_, count, value) != 0) {
            return false;
        }
        position_ += count;
        return true;
    }

    bool Value(unsigned depth) {
        if (depth > 32 || position_ >= text_.size()) return false;
        if (text_[position_] == '"') return String();
        if (text_[position_] == '{') {
            ++position_;
            Space();
            if (Take('}')) return true;
            for (;;) {
                if (!String()) return false;
                Space();
                if (!Take(':')) return false;
                Space();
                if (!Value(depth + 1)) return false;
                Space();
                if (Take('}')) return true;
                if (!Take(',')) return false;
                Space();
            }
        }
        if (text_[position_] == '[') {
            ++position_;
            Space();
            if (Take(']')) return true;
            for (;;) {
                if (!Value(depth + 1)) return false;
                Space();
                if (Take(']')) return true;
                if (!Take(',')) return false;
                Space();
            }
        }
        return Number() || Literal("true") || Literal("false") ||
               Literal("null");
    }

    static bool ValidNamespace(const std::string& value) {
        if (value.empty() || value.size() > 64) return false;
        for (unsigned char character : value) {
            const bool valid =
                (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '_' || character == '.' || character == '-';
            if (!valid) return false;
        }
        return true;
    }

    const std::string& text_;
    size_t position_ = 0;
};

bool ReadAscii(const fs::path& path, const wchar_t* section,
               const wchar_t* key, const wchar_t* fallback,
               size_t maximum, std::string* result) {
    if (!result || maximum > 65536) return false;
    std::vector<wchar_t> buffer(maximum + 2);
    const DWORD count = GetPrivateProfileStringW(
        section, key, fallback, buffer.data(),
        static_cast<DWORD>(buffer.size()), path.c_str());
    if (count >= buffer.size() - 1) return false;
    result->clear();
    result->reserve(count);
    for (DWORD i = 0; i < count; ++i) {
        if (buffer[i] > 0x7F) return false;
        result->push_back(static_cast<char>(buffer[i]));
    }
    return true;
}

bool ParseUnsigned(const std::string& text, uint32_t maximum,
                   uint32_t* value) {
    if (!value || text.empty()) return false;
    uint64_t parsed = 0;
    for (char character : text) {
        if (character < '0' || character > '9') return false;
        parsed = parsed * 10 + static_cast<unsigned>(character - '0');
        if (parsed > maximum) return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool DecodeHex(const std::string& text, std::string* bytes) {
    if (!bytes || text.size() % 2 != 0 ||
        text.size() > SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON * 2) {
        return false;
    }
    bytes->clear();
    bytes->reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int high = HexValue(text[i]);
        const int low = HexValue(text[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes->push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool LoadConfiguration(const fs::path& path, std::string* error) {
    auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    std::error_code file_error;
    if (!fs::is_regular_file(path, file_error) || file_error) {
        return fail("effects.ini is unavailable");
    }
    std::string version;
    if (!ReadAscii(path, L"effects", L"Version", L"", 16, &version) ||
        version != "1") {
        return fail("effects.ini Version must be 1");
    }
    std::string revision;
    if (!ReadAscii(path, L"effects", L"Revision", L"", 64, &revision) ||
        revision.size() != 64 ||
        !std::all_of(revision.begin(), revision.end(), IsHex)) {
        return fail("effects.ini Revision must be 64 hexadecimal characters");
    }

    std::vector<wchar_t> sections(65536);
    const DWORD section_count = GetPrivateProfileSectionNamesW(
        sections.data(), static_cast<DWORD>(sections.size()), path.c_str());
    if (section_count == 0 || section_count >= sections.size() - 2) {
        return fail("effects.ini section list is invalid");
    }

    std::vector<Profile> profiles;
    std::set<uint32_t> seen;
    bool effects_section = false;
    for (const wchar_t* section = sections.data(); *section;
         section += std::wcslen(section) + 1) {
        if (!std::wcscmp(section, L"effects")) {
            effects_section = true;
            continue;
        }
        if (std::wcsncmp(section, L"operator:", 9) != 0 ||
            profiles.size() >= kMaximumProfiles) {
            return fail("effects.ini contains an unsupported section");
        }
        wchar_t* finish = nullptr;
        const unsigned long id = std::wcstoul(section + 9, &finish, 10);
        if (!finish || *finish || id < 60001 || id > 60999 ||
            !seen.insert(static_cast<uint32_t>(id)).second) {
            return fail("effects.ini contains an invalid operator number");
        }
        std::string percent_text;
        uint32_t percent = 0;
        if (!ReadAscii(
                path, section, L"PilotExpPercent", L"0", 16,
                &percent_text) ||
            !ParseUnsigned(percent_text, 100, &percent)) {
            return fail("PilotExpPercent must be between 0 and 100");
        }
        std::string extension_hex;
        std::string extension_json;
        std::vector<ExtensionSpec> extensions;
        if (!ReadAscii(
                path, section, L"ExtensionsHex", L"7B7D",
                SDGO_OPERATOR_EFFECTS_MAX_EXTENSION_JSON * 2,
                &extension_hex) ||
            !DecodeHex(extension_hex, &extension_json) ||
            !JsonParser(extension_json).Extensions(&extensions)) {
            return fail("ExtensionsHex must encode a bounded UTF-8 JSON object");
        }
        profiles.push_back(
            {static_cast<uint32_t>(id), percent, std::move(extensions)});
    }
    if (!effects_section) return fail("effects.ini is missing [effects]");
    std::sort(
        profiles.begin(), profiles.end(),
        [](const Profile& left, const Profile& right) {
            return left.operator_number < right.operator_number;
        });
    g_profiles = std::move(profiles);
    g_revision = std::move(revision);
    if (error) error->clear();
    return true;
}

const Profile* FindProfile(uint32_t operator_number) {
    const auto found = std::lower_bound(
        g_profiles.begin(), g_profiles.end(), operator_number,
        [](const Profile& profile, uint32_t value) {
            return profile.operator_number < value;
        });
    return found != g_profiles.end() &&
                   found->operator_number == operator_number
               ? &*found
               : nullptr;
}

bool ReadOperatorIdentity(
    const SdgoOperatorRewardContextV1& context, uint32_t* operator_number,
    const Profile** profile) {
    if (!operator_number || !profile || !context.battle_pilot ||
        !context.operator_definition) {
        return false;
    }
    void* current_definition = nullptr;
    uint32_t number = 0;
    if (!SafeRead(
            context.battle_pilot, kBattlePilotOperatorDefinition,
            &current_definition) ||
        current_definition != context.operator_definition ||
        !SafeRead(context.operator_definition, 0, &number) ||
        number < 60001 || number > 60999) {
        return false;
    }
    const Profile* found = FindProfile(number);
    if (!found) return false;
    *operator_number = number;
    *profile = found;
    return true;
}

bool FindHandler(const std::string& name, ExtensionHandler* result) {
    if (!result) return false;
    bool found = false;
    AcquireSRWLockShared(&g_handlerLock);
    for (const ExtensionHandler& handler : g_handlers) {
        if (handler.used && name == handler.name) {
            *result = handler;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_handlerLock);
    return found;
}

bool ReadPointAdd(const void* skill, int32_t* percent) {
    if (!percent) return false;
    if (!skill) {
        *percent = 0;
        return true;
    }
    return SafeRead(skill, kSkillPointAdd, percent);
}

bool ReadNativeGpPercents(const SdgoOperatorRewardContextV1& context,
                          int32_t* general_percent,
                          int32_t* outcome_percent) {
    if (!general_percent || !outcome_percent ||
        !context.battle_pilot || !context.operator_definition) {
        return false;
    }
    *general_percent = 0;
    *outcome_percent = 0;

    void* outcome_skill = nullptr;
    const ptrdiff_t outcome_offset =
        context.won ? kBattlePilotWinSkill : kBattlePilotOtherSkill;
    if (!SafeRead(
            context.battle_pilot, outcome_offset, &outcome_skill) ||
        !ReadPointAdd(outcome_skill, outcome_percent)) {
        return false;
    }

    void* general_skill = nullptr;
    if (!SafeRead(
            context.battle_pilot, kBattlePilotGeneralSkill,
            &general_skill)) {
        return false;
    }
    if (!general_skill) return true;

    void* selected = general_skill;
    SYSTEMTIME local = {};
    GetLocalTime(&local);
    const uint32_t now =
        static_cast<uint32_t>(local.wHour) * 100u + local.wMinute;
    for (unsigned index = 0; index < 4; ++index) {
        void* timed = nullptr;
        if (!SafeRead(
                context.operator_definition,
                kDefinitionTimedSkills +
                    static_cast<ptrdiff_t>(index * sizeof(void*)),
                &timed)) {
            return false;
        }
        if (!timed) break;
        void* timed_skill = nullptr;
        uint8_t start_hour = 0;
        uint8_t start_minute = 0;
        uint8_t end_hour = 0;
        uint8_t end_minute = 0;
        if (!SafeRead(timed, kTimedSkill, &timed_skill)) return false;
        if (!timed_skill) break;
        if (!SafeRead(timed, kTimedStartHour, &start_hour) ||
            !SafeRead(timed, kTimedStartMinute, &start_minute) ||
            !SafeRead(timed, kTimedEndHour, &end_hour) ||
            !SafeRead(timed, kTimedEndMinute, &end_minute)) {
            return false;
        }
        const uint32_t start =
            static_cast<uint32_t>(start_hour) * 100u + start_minute;
        const uint32_t end =
            static_cast<uint32_t>(end_hour) * 100u + end_minute;
        if (start <= now && now <= end) {
            selected = timed_skill;
            break;
        }
    }
    return ReadPointAdd(selected, general_percent);
}

uint32_t AddPercent(uint32_t base, uint32_t percent) {
    const uint64_t bonus =
        static_cast<uint64_t>(base) * percent / 100u;
    const uint64_t total = static_cast<uint64_t>(base) + bonus;
    return total > UINT32_MAX ? UINT32_MAX
                              : static_cast<uint32_t>(total);
}

uint32_t PreserveNativeGp(uint32_t base, int32_t general_percent,
                          int32_t outcome_percent) {
    const int64_t general =
        static_cast<int64_t>(base) * general_percent / 100;
    const int64_t outcome =
        static_cast<int64_t>(base) * outcome_percent / 100;
    const int64_t total = static_cast<int64_t>(base) + general + outcome;
    if (total <= 0) return 0;
    if (total >= UINT16_MAX) return UINT16_MAX;
    return static_cast<uint32_t>(total);
}

int EvaluateEffects(const SdgoOperatorRewardContextV1* context,
                    SdgoOperatorRewardResultV1* result) {
    if (!context || context->size < sizeof(*context) ||
        context->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        !result || result->size < sizeof(*result) ||
        result->version != SDGO_OPERATOR_REWARD_FILTER_VERSION ||
        !context->frame || !context->room || !context->room_pilot ||
        !context->battle_pilot ||
        (context->writable_mask &
         ~(SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
           SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
           SDGO_OPERATOR_REWARD_FIELD_GP)) != 0) {
        return SDGO_OPERATOR_REWARD_FILTER_REJECT;
    }
    if (context->participant_role !=
            SDGO_OPERATOR_REWARD_ROLE_HUMAN ||
        context->native_eligible != 1 ||
        !context->operator_definition) {
        return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
    }

    uint32_t operator_number = 0;
    const Profile* profile = nullptr;
    if (!ReadOperatorIdentity(
            *context, &operator_number, &profile)) {
        return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
    }

    std::array<ExtensionHandler, kMaximumExtensions> handlers{};
    for (size_t i = 0; i < profile->extensions.size(); ++i) {
        if (!FindHandler(profile->extensions[i].name, &handlers[i])) {
            return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
        }
    }

    uint32_t write_mask = 0;
    uint32_t pilot_exp = context->current_pilot_exp;
    uint32_t unit_exp = context->current_unit_exp;
    uint32_t gp = context->current_gp;
    if (profile->pilot_exp_percent != 0 &&
        (context->writable_mask &
         SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP) != 0) {
        pilot_exp = AddPercent(
            context->current_pilot_exp,
            profile->pilot_exp_percent);
        write_mask |= SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP;
    }

    if (context->gp_policy == SDGO_OPERATOR_REWARD_GP_REPLACE &&
        (context->writable_mask & SDGO_OPERATOR_REWARD_FIELD_GP) != 0) {
        int32_t general_percent = 0;
        int32_t outcome_percent = 0;
        if (!ReadNativeGpPercents(
                *context, &general_percent, &outcome_percent)) {
            return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
        }
        const uint32_t target = PreserveNativeGp(
            context->current_gp, general_percent, outcome_percent);
        if (target != context->current_gp) {
            gp = target;
            write_mask |= SDGO_OPERATOR_REWARD_FIELD_GP;
        }
    }

    for (size_t i = 0; i < profile->extensions.size(); ++i) {
        const ExtensionSpec& extension = profile->extensions[i];
        const ExtensionHandler& handler = handlers[i];
        const SdgoOperatorEffectExtensionContextV1 extension_context = {
            sizeof(SdgoOperatorEffectExtensionContextV1),
            SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION,
            context,
            operator_number,
            extension.name.data(),
            static_cast<uint32_t>(extension.name.size()),
            extension.payload.data(),
            static_cast<uint32_t>(extension.payload.size()),
            context->writable_mask,
            pilot_exp,
            unit_exp,
            gp};
        SdgoOperatorEffectExtensionResultV1 extension_result = {
            sizeof(SdgoOperatorEffectExtensionResultV1),
            SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION,
            0,
            pilot_exp,
            unit_exp,
            gp};
        int status = SDGO_OPERATOR_EFFECT_EXTENSION_REJECT;
        __try {
            status = handler.callback(
                &extension_context, &extension_result, handler.user);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
        }
        if (status == SDGO_OPERATOR_EFFECT_EXTENSION_REJECT ||
            extension_result.size <
                sizeof(SdgoOperatorEffectExtensionResultV1) ||
            extension_result.version !=
                SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION ||
            (extension_result.write_mask &
             ~context->writable_mask) != 0) {
            return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
        }
        if (status == SDGO_OPERATOR_EFFECT_EXTENSION_APPLY) {
            write_mask |= extension_result.write_mask;
            if (extension_result.write_mask &
                SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP) {
                pilot_exp = extension_result.target_pilot_exp;
            }
            if (extension_result.write_mask &
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP) {
                unit_exp = extension_result.target_unit_exp;
            }
            if (extension_result.write_mask &
                SDGO_OPERATOR_REWARD_FIELD_GP) {
                gp = extension_result.target_gp;
            }
        } else if (status != SDGO_OPERATOR_EFFECT_EXTENSION_NO_CHANGE) {
            return SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
        }
    }

    result->write_mask = write_mask;
    result->target_pilot_exp = pilot_exp;
    result->target_unit_exp = unit_exp;
    result->target_gp = gp;
    if (write_mask && g_trace_settlements &&
        InterlockedIncrement(&g_trace_count) <= 16) {
        g_api->Log(g_mod, SDGO_LOG_INFO,
            "operator settlement operator=%u site=%u phase=%u mask=%u "
            "native=%u/%u/%u current=%u/%u/%u target=%u/%u/%u",
            operator_number, context->site, context->phase, write_mask,
            context->native_pilot_exp, context->native_unit_exp, context->native_gp,
            context->current_pilot_exp, context->current_unit_exp, context->current_gp,
            pilot_exp, unit_exp, gp);
    }
    return write_mask ? SDGO_OPERATOR_REWARD_FILTER_APPLY
                      : SDGO_OPERATOR_REWARD_FILTER_NO_CHANGE;
}

int __stdcall SharedFilter(
    const SdgoOperatorRewardContextV1* context,
    SdgoOperatorRewardResultV1* result, void*) {
    return EvaluateEffects(context, result);
}

bool ReadSettlementIdentity(
    const native_stats::NativeFrame& native,
    SettlementIdentity* identity) {
    if (!identity || !native.room || !native.account) return false;
    SettlementIdentity result;
    result.room = native.room;
    result.account = native.account;
    if (!SafeRead(native.room, 0, &result.room_id) ||
        !SafeRead(
            native.room, native_stats::kRoomStartedAtUnix,
            &result.started) ||
        result.started == 0) {
        return false;
    }
    *identity = result;
    return true;
}

bool SameIdentity(const SettlementIdentity& left,
                  const SettlementIdentity& right) {
    return left.room == right.room &&
           left.room_id == right.room_id &&
           left.started == right.started &&
           left.account == right.account;
}

bool ClaimPhase(const SettlementIdentity& identity, uint32_t phase,
                bool mission) {
    const uint8_t phase_bit =
        phase == SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT
            ? static_cast<uint8_t>(1)
            : phase == SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS
                ? static_cast<uint8_t>(2)
                : static_cast<uint8_t>(0);
    if (!phase_bit) return false;
    AcquireSRWLockExclusive(&g_replayLock);
    const uint64_t serial = ++g_replaySerial;
    for (size_t i = 0; i < g_replayCount; ++i) {
        ReplayRecord& record = g_replays[i];
        if (!SameIdentity(record.identity, identity)) continue;
        record.serial = serial;
        if ((record.phases & phase_bit) != 0) {
            ReleaseSRWLockExclusive(&g_replayLock);
            return false;
        }
        record.phases |= phase_bit;
        record.mission = record.mission || mission;
        ReleaseSRWLockExclusive(&g_replayLock);
        return true;
    }
    size_t target = g_replayCount;
    if (target < g_replays.size()) {
        ++g_replayCount;
    } else {
        target = 0;
        for (size_t i = 1; i < g_replays.size(); ++i) {
            const ReplayRecord& candidate = g_replays[i];
            const ReplayRecord& current = g_replays[target];
            const bool candidate_complete =
                !candidate.mission || (candidate.phases & 2) != 0;
            const bool current_complete =
                !current.mission || (current.phases & 2) != 0;
            if ((candidate_complete && !current_complete) ||
                (candidate_complete == current_complete &&
                 candidate.serial < current.serial)) {
                target = i;
            }
        }
    }
    g_replays[target] = {identity, phase_bit, mission, serial};
    ReleaseSRWLockExclusive(&g_replayLock);
    return true;
}

bool ApplyFallbackResult(BYTE* frame, unsigned site,
                         const SdgoOperatorRewardResultV1& result) {
    const uint32_t exp_mask =
        result.write_mask &
        (SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
         SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP);
    if (exp_mask &&
        !WriteNativeExpTargets(
            frame, site, exp_mask, result.target_pilot_exp,
            result.target_unit_exp)) {
        return false;
    }
    return (result.write_mask & SDGO_OPERATOR_REWARD_FIELD_GP) == 0 ||
           WriteNativeGpTarget(frame, site, result.target_gp);
}

void LatchContractFailure(const char* message) {
    InterlockedExchange(&g_contractFailure, 1);
    if (g_api && g_api->Log && message) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "%s", message);
    }
}

bool FallbackPhase(unsigned site, uint32_t phase, BYTE* frame,
                   const native_stats::NativeFrame& native,
                   const SettlementIdentity* identity,
                   uint32_t writable_mask, bool mission) {
    if (!identity || !ClaimPhase(*identity, phase, mission)) return true;
    NativeRewardAmounts amounts;
    void* definition = nullptr;
    if (!ReadNativeRewardAmounts(frame, site, &amounts) ||
        !SafeRead(
            native.battle_pilot, kBattlePilotOperatorDefinition,
            &definition)) {
        return false;
    }
    const SdgoOperatorRewardContextV1 context = {
        sizeof(SdgoOperatorRewardContextV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        site,
        phase,
        writable_mask,
        frame,
        native.room,
        native.room_pilot,
        native.battle_pilot,
        definition,
        native.account,
        static_cast<uint32_t>(SDGO_OPERATOR_REWARD_ROLE_HUMAN),
        static_cast<uint32_t>(native.outcome),
        native.outcome == native_stats::NativeOutcome::WinOrClear
            ? 1u
            : 0u,
        native.native_eligible ? 1u : 0u,
        static_cast<uint32_t>(SDGO_OPERATOR_REWARD_GP_NATIVE),
        amounts.pilot_exp,
        amounts.pilot_event,
        amounts.unit_exp,
        amounts.unit_event,
        amounts.gp,
        amounts.gp_event,
        amounts.pilot_exp,
        amounts.pilot_event,
        amounts.unit_exp,
        amounts.unit_event,
        amounts.gp,
        amounts.gp_event};
    SdgoOperatorRewardResultV1 result = {
        sizeof(SdgoOperatorRewardResultV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        0,
        amounts.pilot_exp,
        amounts.unit_exp,
        amounts.gp};
    const int status = EvaluateEffects(&context, &result);
    return status != SDGO_OPERATOR_REWARD_FILTER_APPLY ||
           ApplyFallbackResult(frame, site, result);
}

void FallbackMissionFinal(BYTE* frame) {
    const MissionPending pending = g_missionPending;
    g_missionPending = {};
    if (!pending.active) return;
    native_stats::NativeFrame native;
    native_stats::CaptureError capture;
    SettlementIdentity identity;
    void* operator_definition = nullptr;
    if (!native_stats::ReadSettlementFrame(
            9, frame, &native, &capture) ||
        pending.frame != frame || pending.room != native.room ||
        pending.room_pilot != native.room_pilot ||
        pending.battle_pilot != native.battle_pilot ||
        pending.account != native.account ||
        !SafeRead(
            native.battle_pilot, kBattlePilotOperatorDefinition,
            &operator_definition) ||
        pending.operator_definition != operator_definition ||
        !ReadSettlementIdentity(native, &identity) ||
        !pending.identity_read ||
        !SameIdentity(pending.identity, identity)) {
        LatchContractFailure(
            "operator effects mission final frame changed identity");
        return;
    }
    if (!FallbackPhase(
            9, SDGO_OPERATOR_REWARD_PHASE_FINAL_TOTALS, frame, native,
            &identity,
            SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                SDGO_OPERATOR_REWARD_FIELD_GP,
            true)) {
        LatchContractFailure(
            "operator effects could not write mission final rewards");
    }
}

void FallbackSettlement(unsigned site, BYTE* frame) {
    if (InterlockedCompareExchange(&g_contractFailure, 0, 0) != 0) {
        if (site == kSettlementSites.size()) g_missionPending = {};
        return;
    }
    if (site == kSettlementSites.size()) {
        FallbackMissionFinal(frame);
        return;
    }
    native_stats::NativeFrame native;
    native_stats::CaptureError capture;
    if (!native_stats::ReadSettlementFrame(
            site, frame, &native, &capture)) {
        LatchContractFailure(
            "operator effects settlement frame contract failed");
        return;
    }
    if (native.kind != native_stats::ParticipantKind::Human) return;
    SettlementIdentity identity;
    const bool identity_read = ReadSettlementIdentity(native, &identity);
    if (site == 9) {
        if (g_missionPending.active) {
            LatchContractFailure(
                "operator effects received nested mission settlement");
            return;
        }
        g_missionPending = {};
        g_missionPending.active = true;
        g_missionPending.identity_read = identity_read;
        g_missionPending.frame = frame;
        g_missionPending.room = native.room;
        g_missionPending.room_pilot = native.room_pilot;
        g_missionPending.battle_pilot = native.battle_pilot;
        if (!SafeRead(
                native.battle_pilot, kBattlePilotOperatorDefinition,
                &g_missionPending.operator_definition)) {
            g_missionPending = {};
            LatchContractFailure(
                "operator effects could not capture mission operator");
            return;
        }
        g_missionPending.account = native.account;
        g_missionPending.identity = identity;
        if (!FallbackPhase(
                site, SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT,
                frame, native, identity_read ? &identity : nullptr,
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP, true)) {
            LatchContractFailure(
                "operator effects could not write mission early rewards");
        }
        return;
    }
    if (!FallbackPhase(
            site, SDGO_OPERATOR_REWARD_PHASE_EARLY_PRE_UNIT,
            frame, native, identity_read ? &identity : nullptr,
            SDGO_OPERATOR_REWARD_FIELD_PILOT_EXP |
                SDGO_OPERATOR_REWARD_FIELD_UNIT_EXP |
                SDGO_OPERATOR_REWARD_FIELD_GP,
            false)) {
        LatchContractFailure(
            "operator effects could not write settlement rewards");
    }
}

void __stdcall FallbackCallback(unsigned site, BYTE* frame) noexcept {
    __try {
        FallbackSettlement(site, frame);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LatchContractFailure(
            "operator effects settlement callback raised an exception");
    }
}

bool AtomicWrite(const fs::path& path, const std::string& bytes) {
    const fs::path temporary =
        path.parent_path() /
        (L"." + path.filename().wstring() + L"." +
         std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool success = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(
            (std::min)(
                bytes.size() - offset,
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        if (!WriteFile(
                file, bytes.data() + offset, request, &written, nullptr) ||
            written == 0) {
            success = false;
            break;
        }
        offset += written;
    }
    if (success) success = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) success = false;
    if (success) {
        success = MoveFileExW(
                      temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING |
                          MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}

bool WriteStatus(const char* mode) {
    std::ostringstream json;
    json << "{\n"
         << "  \"pid\": " << GetCurrentProcessId() << ",\n"
         << "  \"version\": " << kModuleVersion << ",\n"
         << "  \"revision\": \"" << g_revision << "\",\n"
         << "  \"loaded_profiles\": " << g_profiles.size() << ",\n"
         << "  \"native_hook_mode\": \"" << mode << "\"\n"
         << "}\n";
    return AtomicWrite(fs::path(g_directory) / L"status.json", json.str());
}

bool RegisterSharedFilter() {
    HMODULE battle_rewards = GetModuleHandleW(L"battle_rewards.dll");
    if (!battle_rewards) return false;
    const auto register_filter =
        reinterpret_cast<
            SdgoBattleRewardsRegisterOperatorRewardFilterV1Fn>(
            GetProcAddress(
                battle_rewards,
                SDGO_OPERATOR_REWARD_FILTER_EXPORT));
    if (!register_filter) return false;
    const SdgoOperatorRewardFilterSinkV1 sink = {
        sizeof(SdgoOperatorRewardFilterSinkV1),
        SDGO_OPERATOR_REWARD_FILTER_VERSION,
        &SharedFilter,
        nullptr};
    int accepted = 0;
    __try {
        accepted = register_filter(&sink);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return accepted == 1;
}

bool InstallFallbackHooks() {
    bridge::callback = &FallbackCallback;
    for (unsigned i = 0; i < kSettlementSites.size(); ++i) {
        if (!g_api->HookSite(
                g_mod, kSettlementSites[i].symbol,
                bridge::detours[i], bridge::originals[i])) {
            return false;
        }
    }
    return g_api->HookSite(
               g_mod, kMissionFinalSymbol, bridge::detours[10],
               bridge::originals[10]) != 0;
}
}  // namespace

extern "C" __declspec(dllexport) int __cdecl
SdgoOperatorEffectsRegisterExtensionHandlerV1(
    const SdgoOperatorEffectExtensionSinkV1* sink) {
    if (!sink || sink->size < sizeof(*sink) ||
        sink->version != SDGO_OPERATOR_EFFECTS_EXTENSION_VERSION ||
        !sink->extension_namespace || sink->extension_namespace_size == 0 ||
        sink->extension_namespace_size > 64 || !sink->callback) {
        return 0;
    }
    char name[65] = {};
    for (uint32_t i = 0; i < sink->extension_namespace_size; ++i) {
        const unsigned char character = static_cast<unsigned char>(
            sink->extension_namespace[i]);
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '.' || character == '-';
        if (!valid) return 0;
        name[i] = static_cast<char>(character);
    }
    HMODULE callback_owner = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(sink->callback),
            &callback_owner) ||
        !callback_owner) {
        return 0;
    }

    AcquireSRWLockExclusive(&g_handlerLock);
    size_t free_index = g_handlers.size();
    for (size_t i = 0; i < g_handlers.size(); ++i) {
        if (g_handlers[i].used && !std::strcmp(g_handlers[i].name, name)) {
            ReleaseSRWLockExclusive(&g_handlerLock);
            return 0;
        }
        if (!g_handlers[i].used && free_index == g_handlers.size()) {
            free_index = i;
        }
    }
    if (free_index == g_handlers.size()) {
        ReleaseSRWLockExclusive(&g_handlerLock);
        return 0;
    }
    ExtensionHandler& handler = g_handlers[free_index];
    std::memcpy(handler.name, name, sizeof(name));
    handler.callback = sink->callback;
    handler.user = sink->user;
    handler.used = true;
    ReleaseSRWLockExclusive(&g_handlerLock);
    return 1;
}

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size <
            offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) ||
        !api->Log || !api->HookSite || !api->ModDirectory) {
        return 0;
    }
    g_api = api;
    g_mod = mod;
    g_trace_settlements = api->SettingInt &&
        api->SettingInt(mod, "TraceSettlements", 0) != 0;
    std::string error;
    try {
        const wchar_t* directory = api->ModDirectory(mod);
        if (!directory || !*directory) return 0;
        g_directory = directory;
        if (!LoadConfiguration(
                fs::path(g_directory) / L"effects.ini", &error)) {
            api->Log(
                mod, SDGO_LOG_ERROR, "operator effects inactive: %s",
                error.c_str());
            return 0;
        }
    } catch (const std::exception& exception) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "operator effects configuration failed: %s",
            exception.what());
        return 0;
    } catch (...) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "operator effects configuration failed unexpectedly");
        return 0;
    }

    const char* mode = nullptr;
    if (RegisterSharedFilter()) {
        mode = "shared";
    } else if (InstallFallbackHooks()) {
        mode = "fallback";
    } else {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "operator effects could not register or own all settlement hooks");
        return 0;
    }
    bool status_written = false;
    try {
        status_written = WriteStatus(mode);
    } catch (...) {
        status_written = false;
    }
    if (!status_written) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "operator effects could not publish status.json");
    }
    api->Log(
        mod, SDGO_LOG_INFO,
        "operator effects active mode=%s revision=%s profiles=%u",
        mode, g_revision.c_str(),
        static_cast<unsigned>(g_profiles.size()));
    return 1;
}
