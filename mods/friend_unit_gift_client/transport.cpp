#include "transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>
#include <objbase.h>

namespace sdgo::friend_unit_gift::client {
namespace {

bool IsMutation(Command command) noexcept {
    return !wire::ReadOnly(command);
}

bool IsAuthoritativeReceiptStatus(WireStatus status) noexcept {
    const auto value = static_cast<std::int32_t>(status);
    return value >= static_cast<std::int32_t>(WireStatus::success) &&
           value <=
               static_cast<std::int32_t>(WireStatus::request_conflict);
}

} // namespace

bool Transport::Bind(const SdgoApi* api, SdgoMod* mod) noexcept {
    api_ = api;
    mod_ = mod;
    if (!api || !mod || !api->Symbol) {
        state_ = ConnectionState::unbound;
        return false;
    }
    const auto session = reinterpret_cast<NativeSession>(
        api->Symbol(mod, "storage.session"));
    const auto send = reinterpret_cast<NativeSend>(
        api->Symbol(mod, "net.send_packet"));
    if (!session || !send) {
        state_ = ConnectionState::unbound;
        return false;
    }
    return BindNative(session, send);
}

bool Transport::BindNative(
    NativeSession session,
    NativeSend send) noexcept {
    session_ = session;
    send_ = send;
    capabilities_ = 0;
    server_epoch_ = 0;
    session_key_ = 0;
    deadline_ = 0;
    pending_ = {};
    has_pending_ = false;
    mutation_uncertain_ = false;
    uncertain_request_id_ = {};
    uncertain_command_ = Command::hello;
    native_probe_send_attempts_ = 0;
    native_probe_send_successes_ = 0;
    native_probe_last_send_result_ = 0;
    native_probe_last_packet_size_ = 0;
    native_probe_last_session_ = 0;
    native_probe_reply_count_ = 0;
    native_probe_last_reason_ = NativeProbeReason::none;
    if (!session_ || !send_) {
        state_ = ConnectionState::unbound;
        return false;
    }
    // Mod load runs before the native client singleton is safe to call.
    // The first game-thread Tick/BeginProbe establishes the session.
    session_key_ = 0;
    state_ = ConnectionState::disconnected;
    return true;
}

void Transport::SetSink(
    TransportSink sink,
    void* user) noexcept {
    sink_ = sink;
    sink_user_ = user;
}

std::uint64_t Transport::Now() const noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

Transport::SessionSnapshot Transport::CurrentSession() const noexcept {
    SessionSnapshot snapshot;
    if (!session_) return snapshot;
    __try {
        snapshot.pointer = session_();
        if (!snapshot.pointer) return {};
        std::memcpy(
            &snapshot.connection,
            snapshot.pointer + 0x14,
            sizeof(snapshot.connection));
        if (snapshot.connection < 0) return {};
        snapshot.key =
            (static_cast<std::uint64_t>(
                 reinterpret_cast<std::uintptr_t>(snapshot.pointer))
             << 32u) |
            static_cast<std::uint32_t>(snapshot.connection);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return {};
    }
    return snapshot;
}

RequestId Transport::NewRequestId() noexcept {
    RequestId id;
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return id;
    static_assert(
        sizeof(guid) == sizeof(id.bytes),
        "GUID wire identity size changed");
    std::memcpy(id.bytes.data(), &guid, sizeof(guid));
    return id;
}

bool Transport::SendBytes(
    const std::uint8_t* packet,
    std::size_t size,
    SessionSnapshot* sent_session) noexcept {
    const bool native_probe = size == kNativeProbeBytes;
    if (native_probe) {
        ++native_probe_send_attempts_;
        native_probe_last_packet_size_ =
            static_cast<std::uint32_t>(size);
        native_probe_last_send_result_ = 0;
        native_probe_last_session_ = 0;
    }
    if (!packet || size == 0 ||
        size > static_cast<std::size_t>(INT_MAX) || !send_) {
        if (native_probe)
            native_probe_last_reason_ =
                NativeProbeReason::send_rejected;
        return false;
    }
    const auto current = CurrentSession();
    if (!current.key) {
        if (native_probe) {
            native_probe_last_reason_ =
                NativeProbeReason::session_unavailable;
            if (api_ && api_->Log && mod_) {
                const auto saved = GetLastError();
                api_->Log(
                    mod_, SDGO_LOG_INFO,
                    "native gift probe send attempt=%u bytes=%u "
                    "session=0 result=0 reason=session-unavailable",
                    native_probe_send_attempts_,
                    native_probe_last_packet_size_);
                SetLastError(saved);
            }
        }
        return false;
    }
    if (native_probe)
        native_probe_last_session_ = current.key;
    int result = 0;
    bool threw = false;
    std::uint32_t packet_count = 0;
    std::uint32_t byte_count = 0;
    __try {
        std::memcpy(
            &packet_count,
            current.pointer + 0x58,
            sizeof(packet_count));
        std::memcpy(
            &byte_count,
            current.pointer + 0x5C,
            sizeof(byte_count));
        ++packet_count;
        byte_count += static_cast<std::uint32_t>(size);
        std::memcpy(
            current.pointer + 0x58,
            &packet_count,
            sizeof(packet_count));
        std::memcpy(
            current.pointer + 0x5C,
            &byte_count,
            sizeof(byte_count));
        result = send_(
            current.pointer,
            current.connection,
            packet,
            static_cast<int>(size),
            0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = 0;
        threw = true;
    }
    if (native_probe) {
        native_probe_last_send_result_ = result;
        if (result) {
            ++native_probe_send_successes_;
            native_probe_last_reason_ =
                NativeProbeReason::awaiting_reply;
        } else {
            native_probe_last_reason_ =
                threw
                    ? NativeProbeReason::send_exception
                    : NativeProbeReason::send_rejected;
        }
        if (api_ && api_->Log && mod_) {
            const auto saved = GetLastError();
            api_->Log(
                mod_, result ? SDGO_LOG_INFO : SDGO_LOG_WARN,
                "native gift probe send attempt=%u success=%u "
                "bytes=%u session=%llu connection=%d result=%d "
                "packet_count_after=%u byte_count_after=%u reason=%s",
                native_probe_send_attempts_,
                native_probe_send_successes_,
                native_probe_last_packet_size_,
                static_cast<unsigned long long>(
                    native_probe_last_session_),
                current.connection, result, packet_count, byte_count,
                result ? "awaiting-reply"
                       : (threw ? "send-exception"
                                : "send-rejected"));
            SetLastError(saved);
        }
    }
    if (!result) return false;
    session_key_ = current.key;
    if (sent_session) *sent_session = current;
    return true;
}

bool Transport::BeginProbe() noexcept {
    return BeginProbeAt(Now());
}

bool Transport::BeginProbeAt(
    std::uint64_t now_millis) noexcept {
    if (!session_ || !send_ || has_pending_) return false;
    const auto current = CurrentSession();
    if (!current.key) {
        ChangeState(ConnectionState::disconnected);
        return false;
    }
    std::array<std::uint8_t, kNativeProbeBytes> probe{};
    if (!EncodeProbe(&probe)) return false;
    capabilities_ = 0;
    server_epoch_ = 0;
    pending_ = {};
    pending_.command = Command::hello;
    pending_.flags = envelope_read_only;
    has_pending_ = true;
    deadline_ = now_millis + kProbeTimeoutMillis;
    if (!SendBytes(probe.data(), probe.size())) {
        FailPending(
            WireStatus::send_failed,
            ConnectionState::idle);
        return false;
    }
    ChangeState(ConnectionState::probing_native);
    return true;
}

bool Transport::SendExpandedProbe(
    std::uint64_t now_millis) noexcept {
    Request request;
    request.command = Command::hello;
    request.flags =
        envelope_read_only | envelope_expanded_probe;
    request.request_id = NewRequestId();
    std::array<std::uint8_t, kCarrierBytes> packet{};
    if (!EncodeRequest(request, &packet)) return false;
    pending_ = request;
    has_pending_ = true;
    deadline_ = now_millis + kProbeTimeoutMillis;
    if (!SendBytes(packet.data(), packet.size())) {
        FailPending(
            WireStatus::send_failed,
            ConnectionState::unavailable);
        return false;
    }
    ChangeState(ConnectionState::probing_expanded);
    return true;
}

bool Transport::CapabilityAllows(Command command) const noexcept {
    if (!(capabilities_ & capability_protocol)) return false;
    if (command == Command::hello) return false;
    if ((command == Command::offer ||
         command == Command::preview_offer) &&
        !(capabilities_ & capability_offer_enabled))
        return false;
    if ((command == Command::accept ||
         command == Command::return_to_sender ||
         command == Command::takeback) &&
        !(capabilities_ & capability_resolve_existing))
        return false;
    if (command == Command::provenance &&
        !(capabilities_ & capability_provenance))
        return false;
    return true;
}

bool Transport::Send(Request request) noexcept {
    return SendAt(request, Now());
}

bool Transport::SendAt(
    Request request,
    std::uint64_t now_millis) noexcept {
    if (!Ready() || has_pending_ ||
        !CapabilityAllows(request.command))
        return false;
    if (state_ == ConnectionState::uncertain &&
        IsMutation(request.command))
        return false;
    request.flags = static_cast<std::uint16_t>(
        request.flags & ~envelope_agent_validated);
    if (wire::ReadOnly(request.command))
        request.flags = static_cast<std::uint16_t>(
            request.flags | envelope_read_only);
    else
        request.flags = static_cast<std::uint16_t>(
            request.flags & ~envelope_read_only);
    if (request.command == Command::receipt) {
        if (Empty(request.request_id) && mutation_uncertain_)
            request.request_id = uncertain_request_id_;
        if (Empty(request.request_id))
            return false;
    } else if (Empty(request.request_id)) {
        request.request_id = NewRequestId();
    }
    std::array<std::uint8_t, kCarrierBytes> packet{};
    if (!EncodeRequest(request, &packet)) return false;
    pending_ = request;
    has_pending_ = true;
    deadline_ = now_millis + kRequestTimeoutMillis;
    if (!SendBytes(packet.data(), packet.size())) {
        const auto next = IsMutation(request.command)
                              ? ConnectionState::uncertain
                              : state_;
        if (IsMutation(request.command))
            MarkMutationUncertain(request);
        FailPending(WireStatus::send_failed, next);
        return false;
    }
    return true;
}

bool Transport::AcceptInitialCapability(
    const Reply& reply,
    std::uint64_t now_millis) noexcept {
    constexpr std::uint32_t required =
        capability_protocol |
        capability_expanded_requests |
        capability_agent_validated;
    if (reply.command != Command::hello ||
        !Empty(reply.request_id) ||
        reply.status != WireStatus::success ||
        (reply.capabilities & required) != required) {
        native_probe_last_reason_ =
            NativeProbeReason::capability_rejected;
        if (api_ && api_->Log && mod_) {
            const auto saved = GetLastError();
            api_->Log(
                mod_, SDGO_LOG_WARN,
                "native gift probe capability reply rejected "
                "count=%u status=%d capabilities=%08X",
                native_probe_reply_count_,
                static_cast<int>(reply.status),
                reply.capabilities);
            SetLastError(saved);
        }
        FailPending(
            WireStatus::transport_unavailable,
            ConnectionState::unavailable);
        return false;
    }
    capabilities_ = reply.capabilities;
    server_epoch_ = reply.server_epoch;
    native_probe_last_reason_ =
        NativeProbeReason::capability_accepted;
    if (api_ && api_->Log && mod_) {
        const auto saved = GetLastError();
        api_->Log(
            mod_, SDGO_LOG_INFO,
            "native gift probe capability reply accepted "
            "count=%u status=%d capabilities=%08X epoch=%llu",
            native_probe_reply_count_,
            static_cast<int>(reply.status),
            reply.capabilities,
            static_cast<unsigned long long>(reply.server_epoch));
        SetLastError(saved);
    }
    has_pending_ = false;
    deadline_ = 0;
    return SendExpandedProbe(now_millis);
}

bool Transport::MatchesPending(const Reply& reply) const noexcept {
    if (!has_pending_ ||
        reply.command != pending_.command ||
        reply.request_id != pending_.request_id)
        return false;
    if (state_ == ConnectionState::probing_expanded &&
        !(reply.flags & envelope_expanded_probe))
        return false;
    return true;
}

bool Transport::OnStoragePacket(
    std::uint16_t opcode,
    const std::uint8_t* packet,
    std::uint32_t fixed_size) noexcept {
    if (opcode != kNativeReplyOpcode || !packet)
        return false;
    if (fixed_size != kCarrierBytes) {
        if (state_ == ConnectionState::probing_native) {
            native_probe_last_reason_ =
                NativeProbeReason::stock_unproved_size;
            if (api_ && api_->Log && mod_) {
                const auto saved = GetLastError();
                api_->Log(
                    mod_, SDGO_LOG_INFO,
                    "native gift probe stock fallback "
                    "reason=unproved-size bytes=%u",
                    fixed_size);
                SetLastError(saved);
            }
        }
        return false;
    }
    Reply reply;
    if (!DecodeReply(packet, fixed_size, &reply)) {
        if (state_ == ConnectionState::probing_native) {
            native_probe_last_reason_ =
                NativeProbeReason::stock_not_extension;
            if (api_ && api_->Log && mod_) {
                const auto saved = GetLastError();
                api_->Log(
                    mod_, SDGO_LOG_INFO,
                    "native gift probe stock fallback "
                    "reason=not-extension bytes=%u",
                    fixed_size);
                SetLastError(saved);
            }
        }
        return false;
    }

    // From this point the envelope is positively ours. Consume even stale
    // replies so native 0x07BE parsing never sees the extension payload.
    if (state_ == ConnectionState::probing_native) {
        ++native_probe_reply_count_;
        AcceptInitialCapability(reply, Now());
        return true;
    }
    if (!MatchesPending(reply)) {
        Emit(
            EventKind::unexpected_reply,
            WireStatus::transport_invalid,
            reply.command,
            reply.request_id,
            &reply);
        return true;
    }
    Complete(reply);
    return true;
}

void Transport::Complete(const Reply& reply) noexcept {
    const auto completed = pending_;
    has_pending_ = false;
    deadline_ = 0;
    capabilities_ = reply.capabilities;
    server_epoch_ = reply.server_epoch;
    if (state_ == ConnectionState::probing_expanded) {
        constexpr std::uint32_t required =
            capability_protocol |
            capability_expanded_requests |
            capability_agent_validated;
        if (reply.status != WireStatus::success ||
            (reply.capabilities & required) != required ||
            !(reply.flags & envelope_expanded_probe)) {
            Emit(
                EventKind::request_failed,
                WireStatus::transport_unavailable,
                completed.command,
                completed.request_id,
                &reply);
            ChangeState(ConnectionState::unavailable);
            return;
        }
        ChangeState(
            mutation_uncertain_
                ? ConnectionState::uncertain
                : ConnectionState::ready);
    } else if (IsMutation(completed.command)) {
        if (reply.status == WireStatus::outcome_unknown) {
            MarkMutationUncertain(completed);
            ChangeState(ConnectionState::uncertain);
        } else {
            ClearMutationUncertain();
            ChangeState(ConnectionState::ready);
        }
    } else if (completed.command == Command::receipt &&
               mutation_uncertain_ &&
               completed.request_id == uncertain_request_id_) {
        if (IsAuthoritativeReceiptStatus(reply.status)) {
            ClearMutationUncertain();
            ChangeState(ConnectionState::ready);
        } else {
            ChangeState(ConnectionState::uncertain);
        }
    }
    Emit(
        EventKind::reply,
        reply.status,
        completed.command,
        completed.request_id,
        &reply);
}

void Transport::Tick() noexcept {
    TickAt(Now());
}

void Transport::TickAt(std::uint64_t now_millis) noexcept {
    if (!session_ || !send_) return;
    const auto current = CurrentSession();
    if (!current.key) {
        if (state_ != ConnectionState::disconnected) {
            if (has_pending_) {
                if (IsMutation(pending_.command))
                    MarkMutationUncertain(pending_);
                FailPending(
                    WireStatus::disconnected,
                    ConnectionState::disconnected);
            } else {
                ChangeState(ConnectionState::disconnected);
            }
        }
        capabilities_ = 0;
        server_epoch_ = 0;
        session_key_ = 0;
        return;
    }
    if (session_key_ && current.key != session_key_) {
        if (has_pending_) {
            if (IsMutation(pending_.command))
                MarkMutationUncertain(pending_);
            FailPending(
                WireStatus::disconnected,
                ConnectionState::idle);
        } else {
            ChangeState(ConnectionState::idle);
        }
        capabilities_ = 0;
        server_epoch_ = 0;
    } else if (state_ == ConnectionState::disconnected) {
        ChangeState(ConnectionState::idle);
    }
    session_key_ = current.key;
    if (!has_pending_ || !deadline_ ||
        now_millis < deadline_)
        return;
    if (state_ == ConnectionState::probing_native ||
        state_ == ConnectionState::probing_expanded) {
        if (state_ == ConnectionState::probing_native) {
            native_probe_last_reason_ =
                NativeProbeReason::reply_timeout;
            if (api_ && api_->Log && mod_) {
                const auto saved = GetLastError();
                api_->Log(
                    mod_, SDGO_LOG_INFO,
                    "native gift probe timeout sends=%u successes=%u "
                    "last_result=%d bytes=%u session=%llu replies=%u "
                    "reason=no-extension-reply",
                    native_probe_send_attempts_,
                    native_probe_send_successes_,
                    native_probe_last_send_result_,
                    native_probe_last_packet_size_,
                    static_cast<unsigned long long>(
                        native_probe_last_session_),
                    native_probe_reply_count_);
                SetLastError(saved);
            }
        }
        capabilities_ = 0;
        server_epoch_ = 0;
        FailPending(
            WireStatus::transport_unavailable,
            ConnectionState::unavailable);
        return;
    }
    if (IsMutation(pending_.command)) {
        MarkMutationUncertain(pending_);
        FailPending(
            WireStatus::timed_out,
            ConnectionState::uncertain);
    } else {
        FailPending(
            WireStatus::timed_out,
            mutation_uncertain_
                ? ConnectionState::uncertain
                : ConnectionState::ready);
    }
}

void Transport::Reset(WireStatus reason) noexcept {
    const auto current = CurrentSession();
    if (has_pending_) {
        if (IsMutation(pending_.command))
            MarkMutationUncertain(pending_);
        FailPending(
            reason,
            current.key
                ? ConnectionState::idle
                : ConnectionState::disconnected);
    }
    capabilities_ = 0;
    server_epoch_ = 0;
    deadline_ = 0;
    pending_ = {};
    has_pending_ = false;
    session_key_ = current.key;
    ChangeState(
        current.key
            ? ConnectionState::idle
            : ConnectionState::disconnected);
}

void Transport::MarkMutationUncertain(
    const Request& request) noexcept {
    if (!IsMutation(request.command) ||
        Empty(request.request_id))
        return;
    mutation_uncertain_ = true;
    uncertain_request_id_ = request.request_id;
    uncertain_command_ = request.command;
}

void Transport::ClearMutationUncertain() noexcept {
    mutation_uncertain_ = false;
    uncertain_request_id_ = {};
    uncertain_command_ = Command::hello;
}

void Transport::FailPending(
    WireStatus status,
    ConnectionState next_state) noexcept {
    const auto failed = pending_;
    const bool had_pending = has_pending_;
    has_pending_ = false;
    deadline_ = 0;
    pending_ = {};
    if (had_pending) {
        Emit(
            EventKind::request_failed,
            status,
            failed.command,
            failed.request_id);
    }
    ChangeState(next_state);
}

void Transport::ChangeState(ConnectionState state) noexcept {
    if (state_ == state) return;
    state_ = state;
    Emit(
        EventKind::state_changed,
        WireStatus::success,
        Command::hello,
        {});
}

void Transport::Emit(
    EventKind kind,
    WireStatus status,
    Command command,
    const RequestId& request_id,
    const Reply* reply) noexcept {
    if (!sink_) return;
    TransportEvent event;
    event.kind = kind;
    event.state = state_;
    event.status = status;
    event.command = command;
    event.request_id = request_id;
    event.reply = reply;
    sink_(&event, sink_user_);
}

} // namespace sdgo::friend_unit_gift::client
