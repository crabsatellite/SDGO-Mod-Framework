#pragma once
#include "native_actions.h"
#include "quiet_graphics.h"
#include "sdgo/game_test.h"
#include <string>

// Optional local preview adapter observation. This bounded suite proves the
// genuine legacy scene and frame; it makes no multiplayer/style-binding claim.
struct RoomStylePreviewSnapshotV1 {
    uint32_t size, version, requested_target, actual_app, room_id, map_id;
    uint32_t transitions, ready, refresh_count, expected_refresh_opcode;
    uint32_t last_decision, reject_reason, factory_type, source_app, caller_rva;
    uint32_t payload_address, profile_room_id, profile_map_id;
    uint32_t manager_found, manager_ids_match, tail_crc32, reserved[5];
};
static_assert(sizeof(RoomStylePreviewSnapshotV1) == 104, "preview observation ABI changed");

class RoomStylePreviewActions {
public:
    int Poll(const SdgoGameTestContextV1* context, bool mission, NativeActions& native,
             QuietGraphics& graphics, const std::wstring& outputDirectory) {
        using Query = int(__cdecl*)(RoomStylePreviewSnapshotV1*, uint32_t);
        const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"room_style_preview.dll"),
            "SdgoRoomStylePreviewGetSnapshotV1"));
        if (!query) { context->Assert(0, "explicit room-style preview adapter is absent"); return SDGO_GAME_TEST_FAIL; }
        RoomStylePreviewSnapshotV1 state{};
        if (!query(&state, sizeof(state)) || state.size != sizeof(state) || state.version != 1) {
            context->Assert(0, "room-style preview observation ABI is incompatible"); return SDGO_GAME_TEST_FAIL;
        }
        const uint32_t target = mission ? 16 : 7;
        if (context->elapsed_ms >= nextObservation_) {
            char line[384]; sprintf_s(line,
                "legacy preview target=%u app=%u room=%u map=%u transitions=%u ready=%u manager=%u/%u decision=%u reject=%u refresh=%04X profile_diag=%u/%u authority=%u source=%u checks=%u/%u/%u",
                state.requested_target, state.actual_app, state.room_id, state.map_id, state.transitions,
                state.ready, state.manager_found, state.manager_ids_match, state.last_decision,
                state.reject_reason, state.expected_refresh_opcode, state.profile_room_id, state.profile_map_id,
                state.reserved[0], state.reserved[1], state.reserved[2], state.reserved[3], state.reserved[4]);
            context->Observe(line); nextObservation_ = context->elapsed_ms + 5000;
        }
        if (!state.ready) return SDGO_GAME_TEST_WAIT;
        const uint16_t opcode = mission ? 0x08A5 : 0x0943;
        const auto refreshed = native.RoomRefresh(opcode);
        if (!refreshed.count || refreshed.room_id != state.room_id) return SDGO_GAME_TEST_WAIT;
        if (!stableAt_) stableAt_ = GetTickCount64();
        if (GetTickCount64() - stableAt_ < 2000) return SDGO_GAME_TEST_WAIT;
        if (!requested_) {
            const bool exact = state.requested_target == target && state.actual_app == target &&
                native.AppState() == target && state.transitions == 1 && state.room_id &&
                state.reserved[0] == state.room_id && state.reserved[1] &&
                state.reserved[2] == 1 && state.reserved[3] == 1 && state.reserved[4] == 1 &&
                state.manager_found && state.manager_ids_match && state.expected_refresh_opcode == opcode;
            context->Assert(exact, "the exact native legacy scene publishes the genuine server room/map and refresh request");
            if (!exact) return SDGO_GAME_TEST_FAIL;
            output_ = outputDirectory + (mission ? L"\\room-style-mission.bmp" : L"\\room-style-pvp.bmp");
            if (!graphics.RequestFrameCapture(output_.c_str())) {
                context->Assert(0, "real-device frame request could not be queued"); return SDGO_GAME_TEST_FAIL;
            }
            requested_ = true;
            return SDGO_GAME_TEST_WAIT;
        }
        HRESULT result = E_PENDING;
        const auto status = graphics.FrameCaptureState(&result);
        if (status == QuietFrameCapture::Failed) {
            char line[96]; sprintf_s(line, "native frame capture HRESULT=%08lX", static_cast<unsigned long>(result));
            context->Observe(line); context->Assert(0, "genuine device backbuffer capture failed"); return SDGO_GAME_TEST_FAIL;
        }
        if (status != QuietFrameCapture::Complete) return SDGO_GAME_TEST_WAIT;
        context->Assert(GetFileAttributesW(output_.c_str()) != INVALID_FILE_ATTRIBUTES,
            "the real legacy room backbuffer was saved without showing or activating a desktop window");
        context->Observe(mission ? "native mission legacy frame: room-style-mission.bmp" : "native PvP legacy frame: room-style-pvp.bmp");
        return SDGO_GAME_TEST_PASS;
    }
private:
    bool requested_ = false;
    uint64_t stableAt_ = 0, nextObservation_ = 0;
    std::wstring output_;
};
