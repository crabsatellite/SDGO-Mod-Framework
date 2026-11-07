#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sdgo::login_recovery_client {

using Millis = std::uint64_t;
using Generation = std::uint64_t;
using ActionId = std::uint32_t;

inline constexpr std::size_t kMaxAutomaticRetries = 3;

enum class ActionKind {
    None,
    RetryLogin,
    ReturnToLogin,
};

enum class State {
    Idle,
    AwaitingOutcome,
    PausedForInteraction,
    Backoff,
    AwaitingRecoveryCompletion,
    ReturnToLoginQueued,
    Succeeded,
    Cancelled,
    ReturnedToLogin,
    ReturnToLoginFailed,
};

enum class Outcome {
    None,
    Success,
    UserCancelled,
    PasswordRejected,
    Banned,
    OnlineRejectedBeforeAuthentication,
    UnknownRejected,
    RetryLimitReached,
    TotalDeadlineExceeded,
    RetryActionFailed,
    RetryActionTimedOut,
};

struct Config {
    Millis initialOutcomeTimeoutMs = 15'000;
    Millis retryOutcomeTimeoutMs = 15'000;
    Millis actionTimeoutMs = 10'000;
    Millis totalTimeoutMs = 90'000;
    std::array<Millis, kMaxAutomaticRetries> retryBackoffMs{
        1'000, 2'000, 4'000};
};

struct Action {
    ActionKind kind = ActionKind::None;
    Generation generation = 0;
    ActionId id = 0;
    std::uint32_t retryNumber = 0;

    explicit constexpr operator bool() const noexcept {
        return kind != ActionKind::None;
    }
};

// The caller supplies a non-decreasing monotonic millisecond clock. Tick emits
// each action once; the adapter must finish it through RecoveryCompleted.
class Policy {
public:
    explicit Policy(Config config = {}) noexcept : config_(config) {}

    Generation Begin(Millis now) noexcept {
        now = Observe(now);
        AdvanceGeneration();
        actionSequence_ = 0;
        retriesScheduled_ = 0;
        outcome_ = Outcome::None;
        ClearPending();
        pausedTotalRemaining_ = 0;
        totalDeadline_ = SaturatingAdd(now, config_.totalTimeoutMs);
        stageDeadline_ = BoundedDeadline(
            now, config_.initialOutcomeTimeoutMs);
        retryReadyAt_ = 0;
        state_ = State::AwaitingOutcome;
        return generation_;
    }

    bool Progress(Generation generation, Millis now) noexcept {
        if (!IsCurrent(generation) ||
            (state_ != State::AwaitingOutcome &&
             state_ != State::Backoff)) {
            return false;
        }
        now = Observe(now);
        if (!BeforeTotalDeadline(now)) {
            return false;
        }
        if (state_ == State::Backoff) {
            CancelUnissuedBackoff();
        }
        stageDeadline_ = BoundedDeadline(
            now,
            retriesScheduled_ == 0 ? config_.initialOutcomeTimeoutMs
                                   : config_.retryOutcomeTimeoutMs);
        return true;
    }

    bool PauseForInteraction(Generation generation, Millis now) noexcept {
        if (!IsCurrent(generation) ||
            (state_ != State::AwaitingOutcome &&
             state_ != State::Backoff)) {
            return false;
        }
        now = Observe(now);
        if (!BeforeTotalDeadline(now)) {
            return false;
        }
        if (state_ == State::Backoff) {
            CancelUnissuedBackoff();
        }
        pausedTotalRemaining_ = totalDeadline_ - now;
        state_ = State::PausedForInteraction;
        return true;
    }

    // Returns the active event generation, or zero if no interaction was
    // paused. Rotation is the safe default; a native adapter may retain the
    // generation when the same live transport continues through interaction.
    Generation ResumeAfterInteraction(Generation generation,
                                      Millis now,
                                      bool rotateGeneration = true) noexcept {
        if (!IsCurrent(generation) ||
            state_ != State::PausedForInteraction) {
            return 0;
        }
        now = Observe(now);
        totalDeadline_ = SaturatingAdd(now, pausedTotalRemaining_);
        stageDeadline_ = BoundedDeadline(
            now,
            retriesScheduled_ == 0 ? config_.initialOutcomeTimeoutMs
                                   : config_.retryOutcomeTimeoutMs);
        pausedTotalRemaining_ = 0;
        state_ = State::AwaitingOutcome;
        if (rotateGeneration) {
            AdvanceGeneration();
        }
        return generation_;
    }

    bool Reject(Generation generation,
                int errorCode,
                bool authenticatedOldSession,
                Millis now) noexcept {
        if (!IsCurrent(generation)) {
            return false;
        }

        // Error 1002 is recoverable only after authentication established that
        // this is the old-online-session case.
        if (errorCode == 1002 && authenticatedOldSession) {
            if (state_ != State::AwaitingOutcome) {
                return false;
            }
            now = Observe(now);
            if (!BeforeTotalDeadline(now)) return false;
            ScheduleRetry(now);
            return true;
        }

        // A real terminal reply wins over a phase timeout whose retry action
        // has not been issued yet. The total deadline remains a hard boundary.
        if (state_ != State::AwaitingOutcome &&
            state_ != State::Backoff) {
            return false;
        }
        now = Observe(now);
        if (!BeforeTotalDeadline(now)) return false;
        if (state_ == State::Backoff) {
            CancelUnissuedBackoff();
        }
        if (errorCode == 1001) {
            QueueReturnToLogin(Outcome::PasswordRejected);
        } else if (errorCode == 1003) {
            QueueReturnToLogin(Outcome::Banned);
        } else if (errorCode == 1002) {
            QueueReturnToLogin(Outcome::OnlineRejectedBeforeAuthentication);
        } else {
            QueueReturnToLogin(Outcome::UnknownRejected);
        }
        return true;
    }

    bool TransportFailure(Generation generation, Millis now) noexcept {
        if (!IsCurrent(generation) ||
            state_ != State::AwaitingOutcome) {
            return false;
        }
        now = Observe(now);
        if (!BeforeTotalDeadline(now)) {
            return false;
        }
        ScheduleRetry(now);
        return true;
    }

    bool Success(Generation generation, Millis now) noexcept {
        if (!IsCurrent(generation) ||
            (state_ != State::AwaitingOutcome &&
             state_ != State::Backoff)) {
            return false;
        }
        now = Observe(now);
        if (!BeforeTotalDeadline(now)) {
            return false;
        }
        if (state_ == State::Backoff) {
            CancelUnissuedBackoff();
        }
        state_ = State::Succeeded;
        outcome_ = Outcome::Success;
        ClearPending();
        return true;
    }

    bool Cancel(Generation generation, Millis now) noexcept {
        if (!IsCurrent(generation) || !Active()) {
            return false;
        }
        Observe(now);
        state_ = State::Cancelled;
        outcome_ = Outcome::UserCancelled;
        ClearPending();
        return true;
    }

    Action Tick(Millis now) noexcept {
        now = Observe(now);
        Expire(now);

        if (state_ == State::Backoff) {
            if (now < retryReadyAt_) {
                return {};
            }
            return Issue(ActionKind::RetryLogin, now);
        }
        if (state_ == State::ReturnToLoginQueued) {
            return Issue(ActionKind::ReturnToLogin, now);
        }
        return {};
    }

    bool RecoveryCompleted(Generation generation,
                           ActionId actionId,
                           bool succeeded,
                           Millis now) noexcept {
        if (!IsCurrent(generation) ||
            state_ != State::AwaitingRecoveryCompletion ||
            pending_.id != actionId) {
            return false;
        }

        now = Observe(now);
        const ActionKind completedKind = pending_.kind;
        if (now >= actionDeadline_) {
            if (completedKind == ActionKind::ReturnToLogin) {
                FinishReturnFailure();
            } else {
                QueueReturnToLogin(Outcome::RetryActionTimedOut);
            }
            return false;
        }
        if (completedKind == ActionKind::RetryLogin &&
            now >= totalDeadline_) {
            QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
            return false;
        }

        ClearPending();
        if (completedKind == ActionKind::ReturnToLogin) {
            state_ = succeeded ? State::ReturnedToLogin
                               : State::ReturnToLoginFailed;
            return true;
        }
        if (!succeeded) {
            QueueReturnToLogin(Outcome::RetryActionFailed);
            return true;
        }

        state_ = State::AwaitingOutcome;
        stageDeadline_ = BoundedDeadline(
            now, config_.retryOutcomeTimeoutMs);
        return true;
    }

    State state() const noexcept { return state_; }
    Outcome outcome() const noexcept { return outcome_; }
    Generation generation() const noexcept { return generation_; }
    std::uint32_t retries_scheduled() const noexcept {
        return retriesScheduled_;
    }
    ActionKind pending_action_kind() const noexcept {
        return state_ == State::AwaitingRecoveryCompletion
                   ? pending_.kind
                   : ActionKind::None;
    }

private:
    static Millis SaturatingAdd(Millis value, Millis delta) noexcept {
        const Millis maximum = std::numeric_limits<Millis>::max();
        return delta > maximum - value ? maximum : value + delta;
    }

    Millis Observe(Millis now) noexcept {
        if (now < lastNow_) {
            return lastNow_;
        }
        lastNow_ = now;
        return now;
    }

    Millis BoundedDeadline(Millis now, Millis duration) const noexcept {
        const Millis deadline = SaturatingAdd(now, duration);
        return deadline < totalDeadline_ ? deadline : totalDeadline_;
    }

    bool IsCurrent(Generation generation) const noexcept {
        return generation != 0 && generation == generation_;
    }

    bool Active() const noexcept {
        return state_ == State::AwaitingOutcome ||
               state_ == State::PausedForInteraction ||
               state_ == State::Backoff ||
               state_ == State::AwaitingRecoveryCompletion ||
               state_ == State::ReturnToLoginQueued;
    }

    bool BeforeTotalDeadline(Millis now) noexcept {
        if (now < totalDeadline_) {
            return true;
        }
        QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
        return false;
    }

    void ClearPending() noexcept {
        pending_ = {};
        actionDeadline_ = 0;
    }

    void AdvanceGeneration() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
    }

    void CancelUnissuedBackoff() noexcept {
        if (retriesScheduled_ != 0) {
            --retriesScheduled_;
        }
        retryReadyAt_ = 0;
        state_ = State::AwaitingOutcome;
    }

    void ScheduleRetry(Millis now) noexcept {
        if (now >= totalDeadline_) {
            QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
            return;
        }
        if (retriesScheduled_ >= kMaxAutomaticRetries) {
            QueueReturnToLogin(Outcome::RetryLimitReached);
            return;
        }
        ++retriesScheduled_;
        retryReadyAt_ = SaturatingAdd(
            now, config_.retryBackoffMs[retriesScheduled_ - 1]);
        state_ = State::Backoff;
    }

    void QueueReturnToLogin(Outcome outcome) noexcept {
        ClearPending();
        outcome_ = outcome;
        state_ = State::ReturnToLoginQueued;
    }

    void FinishReturnFailure() noexcept {
        ClearPending();
        state_ = State::ReturnToLoginFailed;
    }

    Action Issue(ActionKind kind, Millis now) noexcept {
        ++actionSequence_;
        if (actionSequence_ == 0) {
            ++actionSequence_;
        }
        pending_ = Action{
            kind,
            generation_,
            actionSequence_,
            kind == ActionKind::RetryLogin ? retriesScheduled_ : 0};
        actionDeadline_ = SaturatingAdd(now, config_.actionTimeoutMs);
        state_ = State::AwaitingRecoveryCompletion;
        return pending_;
    }

    void Expire(Millis now) noexcept {
        if (state_ == State::AwaitingRecoveryCompletion) {
            if (pending_.kind == ActionKind::ReturnToLogin) {
                if (now >= actionDeadline_) {
                    FinishReturnFailure();
                }
                return;
            }
            if (now >= totalDeadline_) {
                QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
            } else if (now >= actionDeadline_) {
                QueueReturnToLogin(Outcome::RetryActionTimedOut);
            }
            return;
        }

        if (state_ == State::AwaitingOutcome) {
            if (now >= totalDeadline_) {
                QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
            } else if (now >= stageDeadline_) {
                ScheduleRetry(now);
            }
            return;
        }

        if (state_ == State::Backoff && now >= totalDeadline_) {
            QueueReturnToLogin(Outcome::TotalDeadlineExceeded);
        }
    }

    Config config_{};
    State state_ = State::Idle;
    Outcome outcome_ = Outcome::None;
    Generation generation_ = 0;
    ActionId actionSequence_ = 0;
    std::uint32_t retriesScheduled_ = 0;
    Millis lastNow_ = 0;
    Millis totalDeadline_ = 0;
    Millis stageDeadline_ = 0;
    Millis retryReadyAt_ = 0;
    Millis actionDeadline_ = 0;
    Millis pausedTotalRemaining_ = 0;
    Action pending_{};
};

}  // namespace sdgo::login_recovery_client
