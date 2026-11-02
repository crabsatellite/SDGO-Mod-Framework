#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <float.h>
#include <xmmintrin.h>

#include "sdgo/battle_eligibility_observer.h"

using namespace sdgo::battle_rewards;
using namespace sdgo::battle_rewards::eligibility_observer;

extern "C" {
volatile uint32_t g_bridgeAfterEax = 0;
volatile uint32_t g_bridgeAfterEbx = 0;
volatile uint32_t g_bridgeAfterEcx = 0;
volatile uint32_t g_bridgeAfterEdx = 0;
volatile uint32_t g_bridgeAfterEsi = 0;
volatile uint32_t g_bridgeAfterEdi = 0;
volatile uint32_t g_bridgeAfterFlags = 0;
volatile uint32_t g_bridgeExpectedFrame = 0;
volatile uint32_t g_bridgeObservedFrame = 0;
void (*g_bridgeSite)() = &bridge::Site;
}

extern "C" __declspec(naked) void BridgeReturn() {
    __asm { ret }
}

extern "C" __declspec(naked) void BridgeHarness() {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        mov dword ptr [g_bridgeExpectedFrame], ebp
        mov eax, 011223344h
        mov ebx, 055667788h
        mov ecx, 013579BDFh
        mov edx, 02468ACE0h
        mov esi, 00BADB002h
        mov edi, 00C0FFEE0h
        stc
        call dword ptr [g_bridgeSite]
        mov dword ptr [g_bridgeAfterEax], eax
        mov dword ptr [g_bridgeAfterEbx], ebx
        mov dword ptr [g_bridgeAfterEcx], ecx
        mov dword ptr [g_bridgeAfterEdx], edx
        mov dword ptr [g_bridgeAfterEsi], esi
        mov dword ptr [g_bridgeAfterEdi], edi
        pushfd
        pop dword ptr [g_bridgeAfterFlags]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}

namespace {

int failures = 0;

void Check(bool ok, const char* message) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}

template <typename T>
void Put(BYTE* base, ptrdiff_t offset, const T& value) {
    memcpy(base + offset, &value, sizeof(value));
}

bool ReadRva(const std::filesystem::path& path, uint32_t rva,
             void* output, size_t size) {
    if (!output || !size) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length <= 0 || static_cast<uint64_t>(length) > 64 * 1024 * 1024ULL) return false;
    stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), length)) return false;
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) >
            bytes.size() - sizeof(IMAGE_NT_HEADERS32)) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    const auto* section = IMAGE_FIRST_SECTION(nt);
    const uintptr_t sectionOffset =
        reinterpret_cast<const uint8_t*>(section) - bytes.data();
    const size_t sectionBytes =
        static_cast<size_t>(nt->FileHeader.NumberOfSections) *
        sizeof(IMAGE_SECTION_HEADER);
    if (sectionOffset > bytes.size() ||
        sectionBytes > bytes.size() - sectionOffset) {
        return false;
    }
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const uint32_t span = (std::max)(
            section[i].Misc.VirtualSize, section[i].SizeOfRawData);
        if (rva < section[i].VirtualAddress ||
            rva - section[i].VirtualAddress > span ||
            size > span - (rva - section[i].VirtualAddress)) {
            continue;
        }
        const uint64_t fileOffset =
            static_cast<uint64_t>(section[i].PointerToRawData) +
            (rva - section[i].VirtualAddress);
        if (fileOffset > bytes.size() || size > bytes.size() - fileOffset) return false;
        memcpy(output, bytes.data() + fileOffset, size);
        return true;
    }
    return false;
}

void PopulateSyntheticFrame(std::array<BYTE, 0x33000>& roomBytes,
                            std::array<BYTE, 0xC80>& frameBytes,
                            BYTE** frameResult, const char* title,
                            uint32_t nativeId, uint32_t flags) {
    roomBytes.fill(0);
    frameBytes.fill(0);
    BYTE* room = roomBytes.data();
    BYTE* frame = frameBytes.data() + 0xC40;
    constexpr int32_t slot = 3;
    constexpr int32_t rank = 2;
    BYTE* roomPilot =
        room + native_stats::kRoomPilotBase + slot * native_stats::kRoomPilotStride;
    BYTE* battlePilot =
        room + native_stats::kBattlePilotBase + slot * native_stats::kBattlePilotStride;
    BYTE* selectedUnit = battlePilot + kBattlePilotUnits;

    const uint32_t roomId = 76341284;
    const uint32_t started = 1793500000;
    const uint8_t mode = kVerifiedRoomMode;
    const uint8_t titleLength = static_cast<uint8_t>(strlen(title));
    const uint8_t form = 0;
    const uint8_t pilotState = 3;
    const uint32_t selectedUnitId = 22097;
    Put(room, 0, roomId);
    Put(room, native_stats::kRoomStartedAtUnix, started);
    Put(room, native_stats::kRoomMode, mode);
    Put(room, kRoomTitleLength, titleLength);
    memcpy(room + kRoomTitle, title, titleLength);
    Put(roomPilot, native_stats::kRoomPilotId, nativeId);
    Put(battlePilot, kBattlePilotForm, form);
    Put(battlePilot, native_stats::kBattlePilotState, pilotState);
    Put(selectedUnit, 0, selectedUnitId);

    Put(frame, kFrameRoom, reinterpret_cast<uintptr_t>(room));
    Put(frame, kFrameRoomPilot, reinterpret_cast<uintptr_t>(roomPilot));
    Put(frame, kFrameBattlePilot, reinterpret_cast<uintptr_t>(battlePilot));
    Put(frame, kFrameSelectedUnit, reinterpret_cast<uintptr_t>(selectedUnit));
    Put(frame, kFramePhysicalSlot, slot);
    Put(frame, kFrameNativeRank, rank);
    Put(frame, kFrameEligibilityFlags + rank * static_cast<ptrdiff_t>(sizeof(uint32_t)),
        flags);
    *frameResult = frame;
}

void __stdcall MutateBridgeState(BYTE* frame) {
    g_bridgeObservedFrame = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frame));
    SetLastError(0xDEADu);
    _mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | _MM_ROUND_DOWN);
    unsigned ignored = 0;
    _controlfp_s(&ignored, _RC_DOWN, _MCW_RC);
    __asm {
        mov eax, 0A0A0A0A0h
        mov ecx, 0C0C0C0C0h
        mov edx, 0D0D0D0D0h
        clc
    }
}

void __stdcall ThrowFromDispatch(BYTE*) {
    SetLastError(0xBADu);
    throw 7;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("OFFLINE eligibility observer checks; no game process or account is used\n");

    auto branchBytes=kTeamBranchBytes;
    auto unitBytes=kTeamSelectedUnitBytes;
    if(argc>1) {
      const std::filesystem::path binary(argv[1]);
      Check(ReadRva(binary, static_cast<uint32_t>(kTeamBranchRva),
                  branchBytes.data(), branchBytes.size()) &&
              EqualFragment(branchBytes.data(), kTeamBranchBytes),
          "pinned BattleServer branch bytes match the real PE image");
      Check(ReadRva(binary, static_cast<uint32_t>(kTeamSelectedUnitRva),
                  unitBytes.data(), unitBytes.size()) &&
              EqualFragment(unitBytes.data(), kTeamSelectedUnitBytes),
          "selected-unit derivation bytes match the same real PE function");
    } else {
      puts("Native PE checks skipped: supply the owned BattleServer.exe path explicitly; no game assets are required for the offline logic/bridge checks");
    }
    branchBytes[0] ^= 1;
    Check(!EqualFragment(branchBytes.data(), kTeamBranchBytes),
          "a changed native branch fragment fails closed");
    Check(kTeamDisplacedBytes == 6,
          "the trampoline owns exactly the complete rank-load instruction");

    GameTestGate gate{};
    constexpr char run[] = "ca6b5be2-afe8-4e55-b6d8-45a81d86d6c1";
    Check(ValidateGate(run, "server", run, "server", "gt5f5c", &gate) &&
              strcmp(gate.run_uuid, run) == 0 &&
              strcmp(gate.title_nonce, "gt5f5c") == 0,
          "canonical server run and matching run.ini values enable the observer");
    Check(!ValidateGate("CA6B5BE2-afe8-4e55-b6d8-45a81d86d6c1",
                        "server", run, "server", "gt5f5c", &gate) &&
              !ValidateGate(run, "client", run, "server", "gt5f5c", &gate) &&
              !ValidateGate(run, "server", run, "server", "../bad", &gate),
          "noncanonical UUID, wrong role, and unsafe nonce remain inert");

    alignas(16) std::array<BYTE, 0x33000> roomBytes{};
    alignas(16) std::array<BYTE, 0xC80> frameBytes{};
    BYTE* frame = nullptr;
    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "gt5f5c !ai e2 a1", 9229, 0);
    Row row{};
    CaptureError capture = CaptureError::None;
    Check(ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::None && row.native_id == 9229 &&
              !row.ai && row.physical_slot == 3 && row.native_rank == 2 &&
              row.pilot_state == 3 && row.eligibility_flags == 0 &&
              row.selected_unit == 22097 &&
              row.branch == Branch::ContinueSettlement,
          "synthetic verified team frame captures the human continue branch");

    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "gt5f5c !ai e2 a1",
        native_stats::kAiIdBit | 2u, 4);
    Check(ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              row.native_id == (native_stats::kAiIdBit | 2u) &&
              row.ai && row.eligibility_flags == 4 &&
              row.branch == Branch::SkipParticipant,
          "synthetic frame keeps the raw AI bit and native skip decision");

    const uintptr_t wrongUnit =
        reinterpret_cast<uintptr_t>(roomBytes.data() + 0x100);
    Put(frame, kFrameSelectedUnit, wrongUnit);
    Check(!ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::InvalidSelectedUnit,
          "a selected-unit pointer not derived by the real function fails closed");

    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "gt5f5c !ai e2 a1", 9229, 0);
    const int32_t badRank = 12;
    Put(frame, kFrameNativeRank, badRank);
    Check(!ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::InvalidRank,
          "out-of-range rank fails closed before indexing eligibility flags");
    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "another !ai e2 a1", 9229, 0);
    Check(!ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::TitleMismatch,
          "a room without the configured native title nonce is ignored");
    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "placeholder", 9229, 0);
    constexpr char staleTitle[] = {'x', '\0', 'g', 't', '5', 'f', '5', 'c'};
    const uint8_t staleTitleLength = static_cast<uint8_t>(sizeof(staleTitle));
    Put(roomBytes.data(), kRoomTitleLength, staleTitleLength);
    memcpy(roomBytes.data() + kRoomTitle, staleTitle, sizeof(staleTitle));
    Check(!ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::TitleMismatch,
          "nonce bytes after the native title terminator cannot match a battle");
    PopulateSyntheticFrame(
        roomBytes, frameBytes, &frame, "gt5f5c !ai e2 a1", 9229, 0);
    const uint8_t unsupportedMode = 4;
    Put(roomBytes.data(), native_stats::kRoomMode, unsupportedMode);
    Check(!ReadTeamFrame(frame, "gt5f5c", &row, &capture) &&
              capture == CaptureError::WrongMode,
          "unverified settlement modes remain outside this observer");

    BattleRowLimiter limiter;
    const BattleKey key{0x12345678u, 76341284u, 1793500000u};
    bool acceptedAll = true;
    for (uint32_t rank = 0; rank < native_stats::kParticipantSlots; ++rank)
        acceptedAll = limiter.TryAccept(key, rank) && acceptedAll;
    Check(acceptedAll && !limiter.TryAccept(key, 0) &&
              !limiter.TryAccept(key, native_stats::kParticipantSlots),
          "one battle admits at most twelve unique native ranks");
    const BattleKey nextBattle{0x12345678u, 76341284u, 1793500001u};
    Check(limiter.TryAccept(nextBattle, 0),
          "a new native start timestamp creates a distinct battle key");

    bridge::original = reinterpret_cast<void*>(&BridgeReturn);
    bridge::callback = &MutateBridgeState;
    const uint32_t originalMxcsr = _mm_getcsr();
    unsigned originalControl = 0;
    _controlfp_s(&originalControl, 0, 0);
    _mm_setcsr((originalMxcsr & ~_MM_ROUND_MASK) | _MM_ROUND_UP);
    unsigned ignored = 0;
    _controlfp_s(&ignored, _RC_UP, _MCW_RC);
    const uint32_t expectedMxcsr = _mm_getcsr();
    unsigned expectedControl = 0;
    _controlfp_s(&expectedControl, 0, 0);
    SetLastError(0x1234u);
    BridgeHarness();
    unsigned afterControl = 0;
    _controlfp_s(&afterControl, 0, 0);
    Check(g_bridgeAfterEax == 0x11223344u &&
              g_bridgeAfterEbx == 0x55667788u &&
              g_bridgeAfterEcx == 0x13579BDFu &&
              g_bridgeAfterEdx == 0x2468ACE0u &&
              g_bridgeAfterEsi == 0x0BADB002u &&
              g_bridgeAfterEdi == 0x0C0FFEE0u &&
              (g_bridgeAfterFlags & 1u) != 0,
          "the actual naked bridge restores integer registers and flags");
    Check(g_bridgeObservedFrame == g_bridgeExpectedFrame &&
              GetLastError() == 0x1234u,
          "the bridge passes native EBP and restores the thread last-error value");
    Check(_mm_getcsr() == expectedMxcsr &&
              (afterControl & _MCW_RC) == (expectedControl & _MCW_RC),
          "the actual naked bridge restores SIMD and x87 control state");
    _mm_setcsr(originalMxcsr);
    _controlfp_s(&ignored, originalControl, _MCW_RC);

    bridge::callback = &ThrowFromDispatch;
    SetLastError(0x4321u);
    bridge::Dispatch(nullptr);
    Check(GetLastError() == 0x4321u,
          "a throwing diagnostic callback is contained and preserves LastError");
    bridge::callback = nullptr;

    printf("%s\n", failures
        ? "OFFLINE ELIGIBILITY OBSERVER CHECKS FAILED"
        : "ALL OFFLINE ELIGIBILITY OBSERVER CHECKS PASSED");
    return failures ? 1 : 0;
}
