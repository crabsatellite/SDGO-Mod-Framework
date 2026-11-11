#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include "difficulty.h"
#include "sdgo/sdgo_api.h"

namespace battle_ai::ace {
constexpr uint32_t kAiPilotBit = 0x80000000u;
constexpr uint32_t kAcePilotBit = 0x40000000u;
constexpr uint32_t kAceDifficultyMask = 0x30000000u;
constexpr uint32_t kAceMetadataMask = kAcePilotBit | kAceDifficultyMask;

struct Policy {
    int chancePercent = 5;
    Difficulty promoted = Difficulty::Medium;
    int hpPercent = 100;
    int attackPercent = 100;
};
struct Settings {
    bool enabled = true;
    Policy byDifficulty[3] = {
        {5, Difficulty::Medium, 100, 100},
        {5, Difficulty::Hard, 100, 100},
        {5, Difficulty::Hard, 120, 100},
    };
};
struct Outcome {
    bool ace = false;
    Difficulty base = Difficulty::Medium;
    Difficulty effective = Difficulty::Medium;
    int hpPercent = 100;
    int attackPercent = 100;
};
inline int Index(Difficulty value) {
    const int raw = static_cast<int>(value);
    return raw >= 0 && raw <= 2 ? raw : 1;
}
inline Difficulty ParseDifficulty(const wchar_t* text, Difficulty fallback) {
    if (!text) return fallback;
    std::wstring value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    });
    if (value == L"easy") return Difficulty::Easy;
    if (value == L"normal" || value == L"medium") return Difficulty::Medium;
    if (value == L"hard") return Difficulty::Hard;
    if (value == L"same") return fallback;
    return fallback;
}
inline int Bounded(int value, int fallback, int minimum, int maximum) {
    return value < minimum || value > maximum ? fallback : value;
}
inline std::wstring SiblingPath(const SdgoApi* api, SdgoMod* mod,
                                const wchar_t* directory, const wchar_t* file) {
    if (!api || !api->ModDirectory) return {};
    std::wstring path = api->ModDirectory(mod);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    path += directory;
    path += L"\\";
    path += file;
    return path;
}
inline bool ServerProviderActive() {
    using ActiveFn = int(__cdecl*)();
    HMODULE module = GetModuleHandleW(L"ace_ai_server.dll");
    if (!module) return false;
    auto active = reinterpret_cast<ActiveFn>(GetProcAddress(module, "SdgoAceAiActiveV1"));
    if (!active) return false;
    __try { return active() == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline Settings LoadSettings(const SdgoApi* api, SdgoMod* mod) {
    Settings settings;
    settings.enabled = ServerProviderActive();
    if (!settings.enabled) return settings;
    const std::wstring path = SiblingPath(api, mod, L"ace_ai_server", L"config.ini");
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return settings;
    static const wchar_t* suffix[3] = {L"Easy", L"Medium", L"Hard"};
    static const int defaultHp[3] = {100, 100, 120};
    static const wchar_t* defaultPromotion[3] = {L"Medium", L"Hard", L"Hard"};
    for (int i = 0; i < 3; ++i) {
        wchar_t key[48]{};
        swprintf_s(key, L"AceChance%s", suffix[i]);
        settings.byDifficulty[i].chancePercent = Bounded(
            GetPrivateProfileIntW(L"settings", key, 5, path.c_str()), 5, 0, 100);
        swprintf_s(key, L"AceHpPercent%s", suffix[i]);
        settings.byDifficulty[i].hpPercent = Bounded(
            GetPrivateProfileIntW(L"settings", key, defaultHp[i], path.c_str()),
            defaultHp[i], 1, 1000);
        swprintf_s(key, L"AceAttackPercent%s", suffix[i]);
        settings.byDifficulty[i].attackPercent = Bounded(
            GetPrivateProfileIntW(L"settings", key, 100, path.c_str()), 100, 1, 1000);
        swprintf_s(key, L"AceDifficulty%s", suffix[i]);
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"settings", key, defaultPromotion[i], value,
                                 static_cast<DWORD>(sizeof(value) / sizeof(value[0])), path.c_str());
        settings.byDifficulty[i].promoted =
            ParseDifficulty(value, static_cast<Difficulty>(i));
    }
    return settings;
}
inline Outcome Select(const Settings& settings, Difficulty base, uint32_t roll) {
    const Policy& policy = settings.byDifficulty[Index(base)];
    Outcome out{};
    out.base = base;
    out.effective = base;
    if (!settings.enabled || policy.chancePercent <= 0) return out;
    out.ace = policy.chancePercent >= 100 ||
              roll % 100u < static_cast<uint32_t>(policy.chancePercent);
    if (out.ace) {
        out.effective = policy.promoted;
        out.hpPercent = policy.hpPercent;
        out.attackPercent = policy.attackPercent;
    }
    return out;
}
inline bool EncodePilotId(uint32_t nativePilotId, const Outcome& outcome,
                          uint32_t* encodedPilotId) {
    if (!encodedPilotId) return false;
    *encodedPilotId = nativePilotId;
    if (!outcome.ace) return true;
    if ((nativePilotId & kAiPilotBit) == 0 || (nativePilotId & kAceMetadataMask) != 0)
        return false;
    *encodedPilotId = nativePilotId | kAcePilotBit |
        (static_cast<uint32_t>(Index(outcome.effective)) << 28);
    return true;
}
inline Outcome DecodePilotId(uint32_t pilotId, Difficulty base) {
    Outcome out{};
    out.base = base;
    out.effective = base;
    if ((pilotId & (kAiPilotBit | kAcePilotBit)) != (kAiPilotBit | kAcePilotBit))
        return out;
    const uint32_t raw = (pilotId & kAceDifficultyMask) >> 28;
    if (raw > 2u) return out;
    out.ace = true;
    out.effective = static_cast<Difficulty>(raw);
    return out;
}
inline uint32_t Scale(uint32_t value, int percent) {
    if (!value || percent == 100) return value;
    const uint64_t scaled =
        (static_cast<uint64_t>(value) * static_cast<uint32_t>(percent) + 50u) / 100u;
    return scaled > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(scaled);
}
} // namespace battle_ai::ace
