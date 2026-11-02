#pragma once

#include <cstdint>

#include "sdgo/friend_unit_gift_protocol.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::friend_unit_gift::client {

enum class ConnectionState : std::uint8_t {
    unbound = 0,
    disconnected = 1,
    idle = 2,
    probing_native = 3,
    probing_expanded = 4,
    ready = 5,
    uncertain = 6,
    unavailable = 7,
};

enum class EventKind : std::uint8_t {
    state_changed = 0,
    reply = 1,
    request_failed = 2,
    unexpected_reply = 3,
};

enum class NativeProbeReason : std::uint32_t {
    none = 0,
    awaiting_reply = 1,
    session_unavailable = 2,
    send_rejected = 3,
    send_exception = 4,
    reply_timeout = 5,
    stock_unproved_size = 6,
    stock_not_extension = 7,
    capability_rejected = 8,
    capability_accepted = 9,
};

struct TransportEvent {
    EventKind kind = EventKind::state_changed;
    ConnectionState state = ConnectionState::unbound;
    WireStatus status = WireStatus::success;
    Command command = Command::hello;
    RequestId request_id{};

    // Present only for reply and unexpected_reply. The pointer is valid only
    // for the duration of the callback; copy the reply when it must be kept.
    const Reply* reply = nullptr;
};

using TransportSink =
    void(__cdecl*)(const TransportEvent* event, void* user);

class Transport {
public:
    using NativeSession = std::uint8_t*(__cdecl*)();
    using NativeSend = int(__thiscall*)(
        void* session,
        int connection,
        const std::uint8_t* packet,
        int size,
        int flags);

    Transport() noexcept = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Resolves storage.session and net.send_packet. This owns no hook; the
    // storage extension broker forwards fixed-size 0x07BE packets to
    // OnStoragePacket before native packet ownership.
    bool Bind(const SdgoApi* api, SdgoMod* mod) noexcept;

    // Deterministic fixture seam. Product code calls Bind.
    bool BindNative(NativeSession session, NativeSend send) noexcept;

    void SetSink(TransportSink sink, void* user) noexcept;

    // Sends the exact ten-byte native request first. A 2026-byte read-only
    // hello is sent only after a valid Agent+World capability reply.
    bool BeginProbe() noexcept;
    bool BeginProbeAt(std::uint64_t now_millis) noexcept;

    // One request may be outstanding. Empty request IDs are generated here.
    // Mutations are never retried automatically.
    bool Send(Request request) noexcept;
    bool SendAt(Request request, std::uint64_t now_millis) noexcept;

    // Returns true only for a positively decoded custom reply. fixed_size must
    // be kCarrierBytes; zero or an unproved size is never dereferenced.
    bool OnStoragePacket(
        std::uint16_t opcode,
        const std::uint8_t* packet,
        std::uint32_t fixed_size) noexcept;

    void Tick() noexcept;
    void TickAt(std::uint64_t now_millis) noexcept;

    // Clears capability and correlation state. An outstanding request receives
    // request_failed with the supplied reason before state is reset.
    void Reset(WireStatus reason = WireStatus::disconnected) noexcept;

    ConnectionState State() const noexcept { return state_; }
    std::uint32_t Capabilities() const noexcept { return capabilities_; }
    std::uint64_t ServerEpoch() const noexcept { return server_epoch_; }
    bool HasOutstanding() const noexcept { return has_pending_; }
    bool MutationUncertain() const noexcept {
        return mutation_uncertain_;
    }
    RequestId UncertainRequestId() const noexcept {
        return uncertain_request_id_;
    }
    Command UncertainCommand() const noexcept {
        return uncertain_command_;
    }
    std::uint32_t NativeProbeSendAttempts() const noexcept {
        return native_probe_send_attempts_;
    }
    std::uint32_t NativeProbeSendSuccesses() const noexcept {
        return native_probe_send_successes_;
    }
    std::int32_t NativeProbeLastSendResult() const noexcept {
        return native_probe_last_send_result_;
    }
    std::uint32_t NativeProbeLastPacketSize() const noexcept {
        return native_probe_last_packet_size_;
    }
    std::uint64_t NativeProbeLastSession() const noexcept {
        return native_probe_last_session_;
    }
    std::uint32_t NativeProbeReplyCount() const noexcept {
        return native_probe_reply_count_;
    }
    NativeProbeReason NativeProbeLastReason() const noexcept {
        return native_probe_last_reason_;
    }
    bool Ready() const noexcept {
        return state_ == ConnectionState::ready ||
               state_ == ConnectionState::uncertain;
    }

private:
    struct SessionSnapshot {
        std::uint8_t* pointer = nullptr;
        int connection = -1;
        std::uint64_t key = 0;
    };

    static constexpr std::uint64_t kProbeTimeoutMillis = 3000;
    static constexpr std::uint64_t kRequestTimeoutMillis = 10000;

    std::uint64_t Now() const noexcept;
    SessionSnapshot CurrentSession() const noexcept;
    RequestId NewRequestId() noexcept;
    bool SendBytes(
        const std::uint8_t* packet,
        std::size_t size,
        SessionSnapshot* sent_session = nullptr) noexcept;
    bool SendExpandedProbe(std::uint64_t now_millis) noexcept;
    bool AcceptInitialCapability(
        const Reply& reply,
        std::uint64_t now_millis) noexcept;
    bool CapabilityAllows(Command command) const noexcept;
    bool MatchesPending(const Reply& reply) const noexcept;
    void Complete(const Reply& reply) noexcept;
    void MarkMutationUncertain(const Request& request) noexcept;
    void ClearMutationUncertain() noexcept;
    void FailPending(
        WireStatus status,
        ConnectionState next_state) noexcept;
    void ChangeState(ConnectionState state) noexcept;
    void Emit(
        EventKind kind,
        WireStatus status,
        Command command,
        const RequestId& request_id,
        const Reply* reply = nullptr) noexcept;

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeSession session_ = nullptr;
    NativeSend send_ = nullptr;
    TransportSink sink_ = nullptr;
    void* sink_user_ = nullptr;

    ConnectionState state_ = ConnectionState::unbound;
    std::uint32_t capabilities_ = 0;
    std::uint64_t server_epoch_ = 0;
    std::uint64_t session_key_ = 0;
    std::uint64_t deadline_ = 0;
    Request pending_{};
    bool has_pending_ = false;
    bool mutation_uncertain_ = false;
    RequestId uncertain_request_id_{};
    Command uncertain_command_ = Command::hello;
    std::uint32_t native_probe_send_attempts_ = 0;
    std::uint32_t native_probe_send_successes_ = 0;
    std::int32_t native_probe_last_send_result_ = 0;
    std::uint32_t native_probe_last_packet_size_ = 0;
    std::uint64_t native_probe_last_session_ = 0;
    std::uint32_t native_probe_reply_count_ = 0;
    NativeProbeReason native_probe_last_reason_ =
        NativeProbeReason::none;
};

} // namespace sdgo::friend_unit_gift::client
