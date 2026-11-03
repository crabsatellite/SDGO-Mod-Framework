#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "friend_actions.h"
#include "sdgo/friend_unit_gift_client_observer.h"
#include "sdgo/game_test.h"
#include "sdgo/game_test_page_observer.h"

namespace friends {

// Developer orchestration only. Product/native UI callbacks perform every
// action; this adapter neither sends gift packets nor modifies inventory.
class UnitGiftActions {
public:
    // Stable developer-only capture_error values written to [unit_gift]:
    // 0 none, 1 unscoped context, 2 module missing, 3 export missing,
    // 4 query rejected, 5 size/version mismatch, 6 run hash/PID mismatch,
    // 7 row count bounds, 8 peer-name bounds, 9 provenance bounds.
    enum CaptureError : uint32_t {
        kCaptureNone = 0,
        kCaptureUnscopedContext = 1,
        kCaptureModuleMissing = 2,
        kCaptureExportMissing = 3,
        kCaptureQueryRejected = 4,
        kCaptureSizeVersionMismatch = 5,
        kCaptureScopeMismatch = 6,
        kCaptureRowCountBounds = 7,
        kCapturePeerNameBounds = 8,
        kCaptureProvenanceBounds = 9,
    };

    static uint64_t RunHash(const SdgoGameTestContextV1* context) {
        if (!context || !context->run_id) return 0;
        size_t length = 0;
        while (length <= 36 && context->run_id[length]) ++length;
        if (length != 36) return 0;
        return sdgo::game_test_page_observer::ClientRunHash(
            context->run_id, length, "client", 6);
    }

    bool Capture(const SdgoGameTestContextV1* context) {
        snapshot_ = {};
        available_ = false;
        valid_ = false;
        captureError_ = kCaptureNone;
        const auto runHash = RunHash(context);
        if (!runHash) {
            captureError_ = kCaptureUnscopedContext;
            return false;
        }
        const auto module = GetModuleHandleW(L"friend_unit_gift_client.dll");
        if (!module) {
            captureError_ = kCaptureModuleMissing;
            return false;
        }
        const auto query = Procedure<SdgoFriendUnitGiftClientGetSnapshotV1Fn>(
            module, SDGO_FRIEND_UNIT_GIFT_CLIENT_SNAPSHOT_EXPORT);
        if (!query) {
            captureError_ = kCaptureExportMissing;
            return false;
        }
        available_ = true;
        snapshot_.size = sizeof(snapshot_);
        snapshot_.version = SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
        if (!query(&snapshot_)) {
            captureError_ = kCaptureQueryRejected;
            return false;
        }
        if (snapshot_.size != sizeof(snapshot_) ||
            snapshot_.version != SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION) {
            captureError_ = kCaptureSizeVersionMismatch;
            return false;
        }
        if (snapshot_.run_hash != runHash ||
            snapshot_.process_id != GetCurrentProcessId()) {
            captureError_ = kCaptureScopeMismatch;
            return false;
        }
        if (snapshot_.row_count > 20) {
            captureError_ = kCaptureRowCountBounds;
            return false;
        }
        if (snapshot_.provenance_text_code_units > 256) {
            captureError_ = kCaptureProvenanceBounds;
            return false;
        }
        for (uint32_t index = 0; index < snapshot_.row_count; ++index) {
            if (snapshot_.rows[index].peer_name_code_units > 20) {
                captureError_ = kCapturePeerNameBounds;
                return false;
            }
        }
        valid_ = true;
        return true;
    }

    FriendActions::Result Invoke(
        const SdgoGameTestContextV1* context, uint32_t action,
        uint32_t receiver, uint64_t unit, uint64_t gift,
        const uint16_t* note, uint16_t noteLength) {
        if (action < 1 || action > 16 || noteLength > 200 ||
            (noteLength && !note))
            return FriendActions::kInvalidCommand;
        const auto runHash = RunHash(context);
        if (!runHash) return FriendActions::kInvalidCommand;
        const auto invoke = Procedure<SdgoFriendUnitGiftClientActionV1Fn>(
            SDGO_FRIEND_UNIT_GIFT_CLIENT_ACTION_EXPORT);
        if (!invoke) return FriendActions::kUnsupported;
        SdgoFriendUnitGiftClientActionRequestV1 request{};
        request.size = sizeof(request);
        request.version = SDGO_FRIEND_UNIT_GIFT_CLIENT_OBSERVER_VERSION;
        request.run_hash = runHash;
        request.action = action;
        request.receiver_id = receiver;
        request.unit_uid = unit;
        request.gift_id = gift;
        request.note_code_units = noteLength;
        if (noteLength)
            std::memcpy(request.note, note, noteLength * sizeof(uint16_t));
        const int accepted = invoke(&request);
        Capture(context);
        return accepted ? FriendActions::kReady : FriendActions::kReplyRejected;
    }

    void Write(FILE* file, const SdgoGameTestContextV1* context) {
        Capture(context);
        std::fprintf(
            file,
            "[unit_gift]\navailable=%u\nvalid=%u\ncapture_error=%u\n",
            available_ ? 1u : 0u, valid_ ? 1u : 0u,
            static_cast<unsigned>(captureError_));
        if (!valid_) return;
        const auto& s = snapshot_;
        std::fprintf(file,
            "process_id=%u\nui_thread_id=%u\ntransport_state=%u\ncapabilities=%u\n"
            "server_epoch=%llu\nready=%u\nmutation_uncertain=%u\n"
            "storage_broker=%u\nstorage_fallback=%u\ncontrol_flags=%u\n"
            "composer_phase=%u\naction_sequence=%u\nreply_sequence=%u\n"
            "last_status=%d\npending_command=%u\nlast_action=%u\n"
            "last_accepted_operation=%u\nlast_reply_command=%u\nlast_reply_status=%d\n"
            "last_reply_gift_id=%llu\nlast_reply_unit_uid=%llu\n"
            "selected_unit_number=%u\nselected_unit_slot=%u\n"
            "selected_unit_uid=%llu\nselected_gift_id=%llu\npage=%u\n"
            "total_count=%u\nrow_count=%u\nprovenance_present=%u\n",
            s.process_id, s.ui_thread_id, s.transport_state, s.capabilities,
            static_cast<unsigned long long>(s.server_epoch), s.ready,
            s.mutation_uncertain, s.storage_broker, s.storage_fallback,
            s.control_flags, s.composer_phase, s.action_sequence, s.reply_sequence,
            s.last_status, s.pending_command, s.last_action,
            s.last_accepted_operation, s.last_reply_command, s.last_reply_status,
            static_cast<unsigned long long>(s.last_reply_gift_id),
            static_cast<unsigned long long>(s.last_reply_unit_uid),
            s.selected_unit_number, static_cast<unsigned>(s.selected_unit_slot),
            static_cast<unsigned long long>(s.selected_unit_uid),
            static_cast<unsigned long long>(s.selected_gift_id), s.page,
            s.total_count, s.row_count, s.provenance_present);
        std::fprintf(file,
            "inventory_sync_busy=%u\ninventory_sync_error=%u\n"
            "inventory_last_added_uid=%llu\ninventory_last_removed_uid=%llu\n",
            s.inventory_sync_busy, s.inventory_sync_error,
            static_cast<unsigned long long>(s.inventory_last_added_uid),
            static_cast<unsigned long long>(s.inventory_last_removed_uid));
        std::fprintf(file, "pending_request_id=");
        HexBytes(file, s.pending_request_id, 16);
        std::fprintf(file, "\nlast_reply_request_id=");
        HexBytes(file, s.last_reply_request_id, 16);
        std::fprintf(file, "\nprovenance_utf16_hex=");
        HexText(file, s.provenance_text, s.provenance_text_code_units);
        std::fprintf(file, "\n");
        for (uint32_t index = 0; index < s.row_count; ++index) {
            const auto& row = s.rows[index];
            std::fprintf(file,
                "[unit_gift_row%u]\ngift_id=%llu\nunit_uid=%llu\n"
                "offered_unix_millis=%llu\npeer_account=%u\nunit_number=%u\n"
                "level=%u\nstate=%u\ndirection=%u\npeer_name_utf16_hex=",
                index, static_cast<unsigned long long>(row.gift_id),
                static_cast<unsigned long long>(row.unit_uid),
                static_cast<unsigned long long>(row.offered_unix_millis),
                row.peer_account, row.unit_number, row.level, row.state, row.direction);
            HexText(file, row.peer_name, row.peer_name_code_units);
            std::fprintf(file, "\n");
        }
    }

    static bool DecodeHexNote(const wchar_t* text, uint16_t* output, uint16_t* length) {
        if (!length) return false;
        *length = 0;
        if (!output) return false;
        std::memset(output, 0, 200 * sizeof(*output));
        if (!text) return false;
        size_t count = 0;
        while (count <= 800 && text[count]) ++count;
        if (count > 800 || count % 4) return false;
        uint16_t decoded[200]{};
        for (size_t index = 0; index < count; index += 4) {
            uint16_t value = 0;
            for (size_t digit = 0; digit < 4; ++digit) {
                const auto c = text[index + digit];
                const int digitValue = c >= L'0' && c <= L'9' ? c - L'0' :
                    c >= L'a' && c <= L'f' ? c - L'a' + 10 :
                    c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
                if (digitValue < 0) return false;
                value = static_cast<uint16_t>((value << 4) | digitValue);
            }
            if (!value) return false;
            decoded[index / 4] = value;
        }
        const auto codeUnits = count / 4;
        for (size_t index = 0; index < codeUnits; ++index) {
            const auto value = decoded[index];
            if (value >= 0xD800 && value <= 0xDBFF) {
                if (index + 1 >= codeUnits ||
                    decoded[index + 1] < 0xDC00 ||
                    decoded[index + 1] > 0xDFFF)
                    return false;
                ++index;
            } else if (value >= 0xDC00 && value <= 0xDFFF) {
                return false;
            }
        }
        if (codeUnits)
            std::memcpy(output, decoded, codeUnits * sizeof(*output));
        *length = static_cast<uint16_t>(codeUnits);
        return true;
    }

private:
    template<class T> static T Procedure(const char* name) {
        const auto module = GetModuleHandleW(L"friend_unit_gift_client.dll");
        return Procedure<T>(module, name);
    }
    template<class T> static T Procedure(HMODULE module, const char* name) {
        const auto raw = module ? GetProcAddress(module, name) : nullptr;
        T result = nullptr;
        static_assert(sizeof(result) == sizeof(raw));
        std::memcpy(&result, &raw, sizeof(result));
        return result;
    }
    static void HexBytes(FILE* file, const uint8_t* data, size_t count) {
        for (size_t index = 0; index < count; ++index)
            std::fprintf(file, "%02X", static_cast<unsigned>(data[index]));
    }
    static void HexText(FILE* file, const uint16_t* data, size_t count) {
        for (size_t index = 0; index < count; ++index)
            std::fprintf(file, "%04X", static_cast<unsigned>(data[index]));
    }
    SdgoFriendUnitGiftClientSnapshotV1 snapshot_{};
    bool available_ = false;
    bool valid_ = false;
    CaptureError captureError_ = kCaptureNone;
};

} // namespace friends
