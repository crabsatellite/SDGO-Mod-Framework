#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "sdgo/battle_reward_stats.h"

namespace sdgo::battle_rewards::eligibility_observer {

// Verified only for the team settlement function in BattleServer.exe SHA-256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4.
// 0048F290  mov eax,[ebp-980h]
// 0048F296  cmp dword ptr [ebp+eax*4-798h],0
// 0048F29E  jne 0048F441
inline constexpr uintptr_t kTeamBranchRva = 0x0008F290;
inline constexpr std::array<uint8_t, 20> kTeamBranchBytes = {{
    0x8B, 0x85, 0x80, 0xF6, 0xFF, 0xFF,
    0x83, 0xBC, 0x85, 0x68, 0xF8, 0xFF, 0xFF, 0x00,
    0x0F, 0x85, 0x9D, 0x01, 0x00, 0x00,
}};
inline constexpr size_t kTeamDisplacedBytes = 6;

// The selected-unit pointer used by the same function is built at 0048E738:
// form = BP+33Ch; unit = BP+340h + form*658h; [ebp-768h] = unit.
inline constexpr uintptr_t kTeamSelectedUnitRva = 0x0008E738;
inline constexpr std::array<uint8_t, 32> kTeamSelectedUnitBytes = {{
    0x0F, 0xB6, 0x82, 0x3C, 0x03, 0x00, 0x00,
    0x69, 0xC0, 0x58, 0x06, 0x00, 0x00,
    0x8B, 0x8D, 0x40, 0xF8, 0xFF, 0xFF,
    0x8D, 0x94, 0x01, 0x40, 0x03, 0x00, 0x00,
    0x89, 0x95, 0x98, 0xF8, 0xFF, 0xFF,
}};

inline constexpr ptrdiff_t kFrameRoom = -0xBBC;
inline constexpr ptrdiff_t kFrameRoomPilot = -0x234;
inline constexpr ptrdiff_t kFrameBattlePilot = -0x7C0;
inline constexpr ptrdiff_t kFrameSelectedUnit = -0x768;
inline constexpr ptrdiff_t kFramePhysicalSlot = -0x8F4;
inline constexpr ptrdiff_t kFrameNativeRank = -0x980;
inline constexpr ptrdiff_t kFrameEligibilityFlags = -0x798;

inline constexpr size_t kRoomTitleLength = 0x79;
inline constexpr size_t kRoomTitle = 0x7A;
inline constexpr size_t kRoomTitleMax = 20;
inline constexpr size_t kBattlePilotForm = 0x33C;
inline constexpr size_t kBattlePilotUnits = 0x340;
inline constexpr size_t kBattleUnitStride = 0x658;
inline constexpr uint8_t kVerifiedRoomMode = 1;

enum class Branch : uint8_t {
    ContinueSettlement,
    SkipParticipant,
};

enum class CaptureError : uint8_t {
    None,
    InvalidArgument,
    InvalidNonce,
    UnreadableFrame,
    InvalidSlot,
    InvalidRank,
    PointerMismatch,
    UnreadableRoom,
    WrongMode,
    TitleMismatch,
    EmptyParticipant,
    InvalidSelectedUnit,
};

struct GameTestGate {
    char run_uuid[37] = {};
    char title_nonce[9] = {};
};

struct BattleKey {
    uint32_t room_cookie = 0;
    uint32_t room_id = 0;
    uint32_t room_started_at_unix = 0;

    bool operator==(const BattleKey& other) const noexcept {
        return room_cookie == other.room_cookie &&
               room_id == other.room_id &&
               room_started_at_unix == other.room_started_at_unix;
    }
};

struct Row {
    BattleKey battle;
    uint32_t native_id = 0;
    uint32_t selected_unit = 0;
    uint32_t eligibility_flags = 0;
    uint32_t physical_slot = 0;
    uint32_t native_rank = 0;
    uint8_t pilot_state = 0;
    bool ai = false;
    Branch branch = Branch::ContinueSettlement;
    char title_nonce[9] = {};
};

inline bool IsCanonicalUuid(const char* value) noexcept {
    if (!value || strlen(value) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool IsTitleNonce(const char* value) noexcept {
    if (!value) return false;
    const size_t length = strlen(value);
    if (length < 3 || length > 8) return false;
    for (size_t i = 0; i < length; ++i) {
        if (!((value[i] >= 'a' && value[i] <= 'z') ||
              (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9'))) {
            return false;
        }
    }
    return true;
}

inline bool ValidateGate(const char* environmentRun, const char* environmentRole,
                         const char* configuredRun, const char* configuredRole,
                         const char* nonce, GameTestGate* gate) noexcept {
    if (!gate || !IsCanonicalUuid(environmentRun) || !environmentRole ||
        !configuredRun || !configuredRole || strcmp(environmentRole, "server") != 0 ||
        strcmp(configuredRole, "server") != 0 ||
        strcmp(environmentRun, configuredRun) != 0 || !IsTitleNonce(nonce)) {
        return false;
    }
    GameTestGate staged{};
    memcpy(staged.run_uuid, environmentRun, 37);
    const size_t nonceLength = strlen(nonce);
    memcpy(staged.title_nonce, nonce, nonceLength + 1);
    *gate = staged;
    return true;
}

template <size_t N>
inline bool EqualFragment(const uint8_t* bytes,
                          const std::array<uint8_t, N>& expected) noexcept {
    return bytes && memcmp(bytes, expected.data(), expected.size()) == 0;
}

inline bool TitleContainsNonce(const char* title, size_t titleLength,
                               const char* nonce) noexcept {
    if (!title || !IsTitleNonce(nonce) || titleLength > kRoomTitleMax) return false;
    const size_t nonceLength = strlen(nonce);
    for (size_t offset = 0; offset + nonceLength <= titleLength; ++offset) {
        if (memcmp(title + offset, nonce, nonceLength) == 0) return true;
    }
    return false;
}

inline bool ReadTeamFrame(BYTE* frame, const char* nonce, Row* snapshot,
                          CaptureError* error = nullptr) noexcept {
    using namespace native_stats;
    const auto fail = [error](CaptureError value) noexcept {
        if (error) *error = value;
        return false;
    };
    if (error) *error = CaptureError::None;
    if (!frame || !snapshot) return fail(CaptureError::InvalidArgument);
    if (!IsTitleNonce(nonce)) return fail(CaptureError::InvalidNonce);

    uintptr_t roomValue = 0;
    uintptr_t roomPilotValue = 0;
    uintptr_t battlePilotValue = 0;
    uintptr_t selectedUnitValue = 0;
    int32_t slotValue = -1;
    int32_t rankValue = -1;
    if (!Read(frame, kFrameRoom, &roomValue) ||
        !Read(frame, kFrameRoomPilot, &roomPilotValue) ||
        !Read(frame, kFrameBattlePilot, &battlePilotValue) ||
        !Read(frame, kFrameSelectedUnit, &selectedUnitValue) ||
        !Read(frame, kFramePhysicalSlot, &slotValue) ||
        !Read(frame, kFrameNativeRank, &rankValue)) {
        return fail(CaptureError::UnreadableFrame);
    }
    if (slotValue < 0 || slotValue >= static_cast<int32_t>(kParticipantSlots)) {
        return fail(CaptureError::InvalidSlot);
    }
    if (rankValue < 0 || rankValue >= static_cast<int32_t>(kParticipantSlots)) {
        return fail(CaptureError::InvalidRank);
    }

    BYTE* room = reinterpret_cast<BYTE*>(roomValue);
    BYTE* roomPilot = reinterpret_cast<BYTE*>(roomPilotValue);
    BYTE* battlePilot = reinterpret_cast<BYTE*>(battlePilotValue);
    BYTE* selectedUnit = reinterpret_cast<BYTE*>(selectedUnitValue);
    BYTE* expectedRoomPilot = nullptr;
    BYTE* expectedBattlePilot = nullptr;
    if (!ExpectedPointer(room, kRoomPilotBase, kRoomPilotStride,
                         static_cast<uint32_t>(slotValue), &expectedRoomPilot) ||
        !ExpectedPointer(room, kBattlePilotBase, kBattlePilotStride,
                         static_cast<uint32_t>(slotValue), &expectedBattlePilot) ||
        roomPilot != expectedRoomPilot || battlePilot != expectedBattlePilot) {
        return fail(CaptureError::PointerMismatch);
    }

    uint8_t form = 0;
    if (!Read(battlePilot, kBattlePilotForm, &form)) {
        return fail(CaptureError::UnreadableRoom);
    }
    constexpr size_t kMaximumForm =
        (kBattlePilotStride - kBattlePilotUnits - sizeof(uint32_t)) / kBattleUnitStride;
    if (form > kMaximumForm) return fail(CaptureError::InvalidSelectedUnit);
    const uintptr_t battlePilotAddress = reinterpret_cast<uintptr_t>(battlePilot);
    const size_t unitOffset =
        kBattlePilotUnits + static_cast<size_t>(form) * kBattleUnitStride;
    if (battlePilotAddress > (std::numeric_limits<uintptr_t>::max)() - unitOffset ||
        selectedUnit != reinterpret_cast<BYTE*>(battlePilotAddress + unitOffset)) {
        return fail(CaptureError::InvalidSelectedUnit);
    }

    Row staged{};
    staged.battle.room_cookie =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(room));
    staged.physical_slot = static_cast<uint32_t>(slotValue);
    staged.native_rank = static_cast<uint32_t>(rankValue);
    const ptrdiff_t rankedFlags =
        kFrameEligibilityFlags +
        static_cast<ptrdiff_t>(staged.native_rank * sizeof(uint32_t));
    uint8_t roomMode = 0;
    uint8_t titleLength = 0;
    if (!Read(frame, rankedFlags, &staged.eligibility_flags) ||
        !Read(room, 0, &staged.battle.room_id) ||
        !Read(room, kRoomStartedAtUnix, &staged.battle.room_started_at_unix) ||
        !Read(room, kRoomMode, &roomMode) ||
        !Read(room, kRoomTitleLength, &titleLength) ||
        !Read(roomPilot, kRoomPilotId, &staged.native_id) ||
        !Read(battlePilot, kBattlePilotState, &staged.pilot_state) ||
        !Read(selectedUnit, 0, &staged.selected_unit)) {
        return fail(CaptureError::UnreadableRoom);
    }
    if (roomMode != kVerifiedRoomMode) return fail(CaptureError::WrongMode);
    if (titleLength > kRoomTitleMax) return fail(CaptureError::TitleMismatch);
    char title[kRoomTitleMax + 1] = {};
    for (size_t i = 0; i < titleLength; ++i) {
        if (!Read(room, static_cast<ptrdiff_t>(kRoomTitle + i), &title[i])) {
            return fail(CaptureError::UnreadableRoom);
        }
    }
    const size_t actualTitleLength = strnlen(title, titleLength);
    if (!TitleContainsNonce(title, actualTitleLength, nonce)) {
        return fail(CaptureError::TitleMismatch);
    }
    if (!staged.battle.room_id || !staged.battle.room_started_at_unix ||
        !staged.native_id) {
        return fail(CaptureError::EmptyParticipant);
    }
    staged.ai = (staged.native_id & kAiIdBit) != 0;
    staged.branch = staged.eligibility_flags == 0
        ? Branch::ContinueSettlement : Branch::SkipParticipant;
    const size_t nonceLength = strlen(nonce);
    memcpy(staged.title_nonce, nonce, nonceLength + 1);
    *snapshot = staged;
    return true;
}

class BattleRowLimiter {
public:
    bool TryAccept(const BattleKey& battle, uint32_t nativeRank) noexcept {
        if (!battle.room_cookie || !battle.room_id || !battle.room_started_at_unix ||
            nativeRank >= native_stats::kParticipantSlots) {
            return false;
        }
        Entry* available = nullptr;
        for (Entry& entry : entries_) {
            if (entry.used && entry.battle == battle) {
                const uint16_t bit = static_cast<uint16_t>(1u << nativeRank);
                if ((entry.rank_mask & bit) != 0 ||
                    entry.rows >= native_stats::kParticipantSlots) {
                    return false;
                }
                entry.rank_mask = static_cast<uint16_t>(entry.rank_mask | bit);
                ++entry.rows;
                return true;
            }
            if (!entry.used && !available) available = &entry;
        }
        if (!available) return false;
        available->used = true;
        available->battle = battle;
        available->rank_mask = static_cast<uint16_t>(1u << nativeRank);
        available->rows = 1;
        return true;
    }

private:
    struct Entry {
        BattleKey battle{};
        uint16_t rank_mask = 0;
        uint8_t rows = 0;
        bool used = false;
    };
    // A GameTest server process owns one canonical run. Refusing observations
    // after a malformed run creates 64 distinct battles preserves the hard
    // per-battle bound without evicting a key and later admitting duplicates.
    std::array<Entry, 64> entries_{};
};

namespace bridge {
using Callback = void(__stdcall*)(BYTE*);
inline Callback callback = nullptr;
inline void* original = nullptr;
inline const DWORD defaultMxcsr = 0x1F80;

inline void __stdcall Dispatch(BYTE* frame) noexcept {
    const DWORD error = GetLastError();
    try {
        if (callback) callback(frame);
    } catch (...) {
        // Diagnostics cannot unwind through the native settlement frame.
    }
    SetLastError(error);
}

__declspec(naked) inline void Site() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax, esp }
    __asm { sub esp, 0x210 }
    __asm { and esp, 0xfffffff0 }
    __asm { mov [esp + 0x200], eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [defaultMxcsr] }
    __asm { cld }
    __asm { push ebp }
    __asm { call Dispatch }
    __asm { fxrstor [esp] }
    __asm { mov esp, [esp + 0x200] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [original] }
}
} // namespace bridge

} // namespace sdgo::battle_rewards::eligibility_observer
