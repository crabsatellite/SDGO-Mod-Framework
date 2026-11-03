#pragma once

#include "sdgo/friend_unit_gift_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace sdgo::friend_unit_gift::client {

inline constexpr std::size_t kNativeFullUnitBytes = 153;

struct UnitSnapshot {
    std::uint64_t uid = 0;
    std::uint32_t unit_number = 0;
    std::uint16_t slot = 0;
    std::array<std::uint8_t, kNativeFullUnitBytes> full_record{};
    std::uint8_t level = 0;
};

inline bool operator==(const UnitSnapshot& left,
                       const UnitSnapshot& right) noexcept {
    return left.uid == right.uid &&
           left.unit_number == right.unit_number &&
           left.slot == right.slot &&
           left.full_record == right.full_record;
}

inline bool operator!=(const UnitSnapshot& left,
                       const UnitSnapshot& right) noexcept {
    return !(left == right);
}

struct OfferDraft {
    std::uint32_t receiver_id = 0;
    UnitSnapshot unit{};
    std::uint16_t note_code_units = 0;
    std::array<char16_t, kMaximumNoteCodeUnits> note{};
    bool server_previewed = false;
};

struct Detail {
    bool present = false;
    std::uint64_t gift_id = 0;
    std::uint64_t unit_uid = 0;
    std::uint64_t offered_unix_millis = 0;
    std::uint64_t accepted_unix_millis = 0;
    std::uint32_t peer_account = 0;
    std::uint32_t unit_number = 0;
    std::uint32_t level = 0;
    std::uint8_t unit_class = 0;
    std::uint8_t class_mark = 0;
    GiftState state = GiftState::pending;
    ListDirection direction = ListDirection::inbox;
    std::uint16_t peer_name_code_units = 0;
    std::array<char16_t, kMaximumPeerNameCodeUnits> peer_name{};
    std::uint16_t note_code_units = 0;
    std::array<char16_t, kMaximumNoteCodeUnits> note{};
};

class Model {
public:
    void SetSession(std::uint64_t session) noexcept {
        if (session_ == session) return;
        session_ = session;
        capabilities_ = 0;
        server_epoch_ = 0;
        pending_.reset();
        pending_started_ = 0;
        draft_.reset();
        row_count_ = 0;
        total_count_ = 0;
        page_ = 1;
        custom_storage_view_ = false;
        detail_ = {};
        provenance_ = {};
        last_status_ = session ? WireStatus::transport_unavailable
                               : WireStatus::disconnected;
    }

    std::uint64_t session() const noexcept { return session_; }
    std::uint64_t server_epoch() const noexcept { return server_epoch_; }
    std::uint32_t capabilities() const noexcept { return capabilities_; }
    WireStatus last_status() const noexcept { return last_status_; }
    bool pending() const noexcept { return pending_.has_value(); }

    void ApplyCapabilities(
        std::uint64_t session,
        std::uint32_t capabilities,
        std::uint64_t serverEpoch,
        WireStatus status = WireStatus::success) noexcept {
        SyncCapabilities(session, capabilities, serverEpoch);
        last_status_ = status;
    }

    void SyncCapabilities(
        std::uint64_t session,
        std::uint32_t capabilities,
        std::uint64_t serverEpoch) noexcept {
        SetSession(session);
        if (server_epoch_ && serverEpoch &&
            server_epoch_ != serverEpoch) {
            pending_.reset();
            pending_started_ = 0;
            draft_.reset();
            row_count_ = 0;
            total_count_ = 0;
            page_ = 1;
            custom_storage_view_ = false;
            detail_ = {};
            provenance_ = {};
        }
        capabilities_ = capabilities;
        if (serverEpoch) server_epoch_ = serverEpoch;
    }

    bool CanOffer() const noexcept {
        return session_ &&
               (capabilities_ & capability_protocol) &&
               (capabilities_ & capability_expanded_requests) &&
               (capabilities_ & capability_offer_enabled) &&
               (capabilities_ & capability_agent_validated);
    }

    bool CanResolveExisting() const noexcept {
        return session_ &&
               (capabilities_ & capability_protocol) &&
               (capabilities_ & capability_expanded_requests) &&
               (capabilities_ & capability_resolve_existing) &&
               (capabilities_ & capability_agent_validated);
    }

    bool CanReadProvenance() const noexcept {
        return session_ &&
               (capabilities_ & capability_protocol) &&
               (capabilities_ & capability_expanded_requests) &&
               (capabilities_ & capability_provenance) &&
               (capabilities_ & capability_agent_validated);
    }

    bool PrepareOffer(
        std::uint32_t receiverId,
        const UnitSnapshot& unit,
        const char16_t* note,
        std::uint16_t noteCodeUnits) noexcept {
        if (!CanOffer() || pending_ || !receiverId ||
            !unit.uid || !unit.unit_number ||
            noteCodeUnits > kMaximumNoteCodeUnits ||
            (noteCodeUnits && !note) ||
            !wire::ValidUtf16(note, noteCodeUnits)) {
            return false;
        }
        OfferDraft draft;
        draft.receiver_id = receiverId;
        draft.unit = unit;
        draft.note_code_units = noteCodeUnits;
        if (noteCodeUnits) {
            std::memcpy(
                draft.note.data(), note,
                static_cast<std::size_t>(noteCodeUnits) * sizeof(char16_t));
        }
        draft_ = draft;
        return true;
    }

    bool HasOfferConfirmation() const noexcept {
        return draft_.has_value();
    }

    void CancelOffer() noexcept { draft_.reset(); }

    std::optional<Request> PreviewOffer(
        const RequestId& requestId) const noexcept {
        if (!draft_ || draft_->server_previewed ||
            pending_ || Empty(requestId) || !CanOffer()) {
            return std::nullopt;
        }
        Request request;
        request.command = Command::preview_offer;
        request.request_id = requestId;
        request.receiver_id = draft_->receiver_id;
        request.unit_uid = draft_->unit.uid;
        if (!wire::ValidRequest(request)) return std::nullopt;
        return request;
    }

    std::optional<Request> ConfirmOffer(
        const UnitSnapshot& current,
        const RequestId& requestId) noexcept {
        if (!draft_ || !draft_->server_previewed ||
            pending_ || Empty(requestId)) {
            return std::nullopt;
        }
        const auto draft = *draft_;
        // Confirmation is single-use. A changed record must send the player
        // back through selection and confirmation; it is never retried.
        draft_.reset();
        if (!CanOffer() || current != draft.unit) return std::nullopt;

        Request request;
        request.command = Command::offer;
        request.request_id = requestId;
        request.receiver_id = draft.receiver_id;
        request.unit_uid = draft.unit.uid;
        request.note_code_units = draft.note_code_units;
        request.note = draft.note;
        if (!wire::ValidRequest(request)) return std::nullopt;
        return request;
    }

    bool Begin(const Request& request, std::uint64_t now) noexcept {
        if (!session_ || pending_ || Empty(request.request_id) ||
            !wire::ValidRequest(request)) {
            return false;
        }
        if ((request.command == Command::offer ||
             request.command == Command::preview_offer) &&
            !CanOffer()) {
            return false;
        }
        if ((request.command == Command::list_inbox ||
             request.command == Command::list_returned ||
             request.command == Command::inventory_returns ||
             request.command == Command::detail ||
             request.command == Command::accept ||
             request.command == Command::return_to_sender ||
             request.command == Command::takeback ||
             request.command == Command::receipt) &&
            !CanResolveExisting()) {
            return false;
        }
        if (request.command == Command::provenance &&
            !CanReadProvenance()) {
            return false;
        }
        if (request.command == Command::provenance) {
            provenance_ = {};
        }
        pending_ = request;
        pending_started_ = now;
        last_status_ = WireStatus::transport_unavailable;
        return true;
    }

    bool Receive(
        std::uint64_t session,
        const Reply& reply) noexcept {
        if (!session_ || session != session_ || !pending_ ||
            reply.command != pending_->command ||
            reply.request_id != pending_->request_id) {
            return false;
        }

        const auto command = pending_->command;
        const auto requestedGift = pending_->gift_id;
        const auto requestedUnit = pending_->unit_uid;
        const auto requestedReceiver = pending_->receiver_id;
        pending_.reset();
        pending_started_ = 0;
        last_status_ = reply.status;

        if (server_epoch_ && reply.server_epoch &&
            server_epoch_ != reply.server_epoch) {
            row_count_ = 0;
            total_count_ = 0;
            custom_storage_view_ = false;
            detail_ = {};
            provenance_ = {};
            draft_.reset();
        }
        if (reply.server_epoch) server_epoch_ = reply.server_epoch;
        capabilities_ = reply.capabilities;

        if (reply.status != WireStatus::success &&
            reply.status != WireStatus::returning_waiting_capacity) {
            return true;
        }

        if (command == Command::list_inbox ||
            command == Command::list_returned) {
            row_count_ = reply.row_count;
            total_count_ = reply.total_count;
            page_ = reply.page;
            for (std::size_t i = 0; i < row_count_; ++i) {
                rows_[i] = reply.rows[i];
            }
            custom_storage_view_ = true;
        } else if (command == Command::preview_offer) {
            if (draft_ &&
                draft_->receiver_id == requestedReceiver &&
                draft_->unit.uid == requestedUnit &&
                (!reply.receiver_id ||
                 reply.receiver_id == requestedReceiver) &&
                (!reply.unit_uid || reply.unit_uid == requestedUnit)) {
                draft_->server_previewed = true;
            }
        } else if (command == Command::detail &&
                   reply.gift_id == requestedGift) {
            detail_ = MakeDetail(reply);
        } else if (command == Command::provenance &&
                   reply.unit_uid == requestedUnit) {
            provenance_ = MakeDetail(reply);
        } else if (command == Command::receipt) {
            detail_ = MakeDetail(reply);
        } else if (command == Command::accept ||
                   command == Command::return_to_sender ||
                   command == Command::takeback) {
            // The server mutation is committed, but the visible list remains
            // stale until an explicit list request returns. Never fabricate a
            // native 402-byte row or silently remove a row client-side.
            custom_storage_view_ = false;
            row_count_ = 0;
            total_count_ = 0;
        }
        return true;
    }

    bool Fail(
        std::uint64_t session,
        Command command,
        const RequestId& requestId,
        WireStatus status) noexcept {
        if (!session_ || session != session_ || !pending_ ||
            pending_->command != command ||
            pending_->request_id != requestId) {
            return false;
        }
        pending_.reset();
        pending_started_ = 0;
        last_status_ = status;
        return true;
    }

    bool Timeout(std::uint64_t now, std::uint64_t timeout) noexcept {
        if (!pending_ || now - pending_started_ < timeout) return false;
        pending_.reset();
        pending_started_ = 0;
        last_status_ = WireStatus::timed_out;
        return true;
    }

    const std::optional<Request>& pending_request() const noexcept {
        return pending_;
    }

    std::uint16_t row_count() const noexcept { return row_count_; }
    std::uint32_t total_count() const noexcept { return total_count_; }
    std::uint32_t page() const noexcept { return page_; }

    const GiftSummary* row(std::size_t index) const noexcept {
        return index < row_count_ ? &rows_[index] : nullptr;
    }

    void LeaveCustomStorageView() noexcept {
        custom_storage_view_ = false;
    }

    bool custom_storage_view() const noexcept {
        return custom_storage_view_;
    }

    bool BlockNativeClaimAll() const noexcept {
        return custom_storage_view_;
    }

    const Detail& detail() const noexcept { return detail_; }
    const Detail& provenance() const noexcept { return provenance_; }

private:
    static Detail MakeDetail(const Reply& reply) noexcept {
        Detail detail;
        detail.present = true;
        detail.gift_id = reply.gift_id;
        detail.unit_uid = reply.unit_uid;
        detail.offered_unix_millis = reply.offered_unix_millis;
        detail.accepted_unix_millis = reply.accepted_unix_millis;
        detail.peer_account = reply.peer_account;
        detail.unit_number = reply.unit_number;
        detail.level = reply.level;
        detail.unit_class = reply.unit_class;
        detail.class_mark = reply.class_mark;
        detail.state = reply.state;
        detail.direction = reply.direction;
        detail.peer_name_code_units = reply.peer_name_code_units;
        detail.peer_name = reply.peer_name;
        detail.note_code_units = reply.note_code_units;
        detail.note = reply.note;
        return detail;
    }

    std::uint64_t session_ = 0;
    std::uint64_t server_epoch_ = 0;
    std::uint32_t capabilities_ = 0;
    WireStatus last_status_ = WireStatus::disconnected;
    std::optional<Request> pending_;
    std::uint64_t pending_started_ = 0;
    std::optional<OfferDraft> draft_;
    std::array<GiftSummary, kMaximumRows> rows_{};
    std::uint16_t row_count_ = 0;
    std::uint32_t total_count_ = 0;
    std::uint32_t page_ = 1;
    bool custom_storage_view_ = false;
    Detail detail_{};
    Detail provenance_{};
};

} // namespace sdgo::friend_unit_gift::client
