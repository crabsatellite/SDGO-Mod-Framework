#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "native_actions.h"
#include "sdgo/game_test.h"
#include "sdgo/login_recovery.h"
#include "../login_recovery_client/policy.h"

// Read-only acceptance driver for the production login_recovery_client Mod.
//
// The GameTest sends exactly one genuine login request through NativeActions.
// Every retry, reconnect, queue purge and replay remains owned by the production
// Mod. The earlier flow.login case proves a real 0x232B acknowledgement; the
// later flow.onboarding completion gate proves that the same recovery generation
// reached a loaded pilot in the native lobby or hangar.
class LoginRecoveryActions
{
public:
    bool Configure(const std::wstring& runFile) noexcept
    {
        enabled_ = false;
        minimumRetries_ = 0;
        holdAfterCompletion_ = false;
        controllerAckRequired_ = false;
        runId_.clear();
        armEventName_.clear();
        configurationError_ = nullptr;

        wchar_t enabled[8] = {};
        GetPrivateProfileStringW(
            L"run",
            L"login_recovery",
            L"",
            enabled,
            static_cast<DWORD>(sizeof(enabled) / sizeof(enabled[0])),
            runFile.c_str());
        if (!enabled[0] || !wcscmp(enabled, L"0"))
            return true;
        if (wcscmp(enabled, L"1")) {
            configurationError_ =
                "login_recovery must be the canonical boolean 0 or 1";
            return false;
        }

        wchar_t retries[8] = {};
        GetPrivateProfileStringW(
            L"run",
            L"login_recovery_min_retries",
            L"",
            retries,
            static_cast<DWORD>(sizeof(retries) / sizeof(retries[0])),
            runFile.c_str());
        if (!retries[0] ||
            (wcscmp(retries, L"0") && wcscmp(retries, L"1"))) {
            configurationError_ =
                "login_recovery_min_retries must be canonical 0 or 1";
            return false;
        }
        minimumRetries_ = retries[0] == L'1' ? 1u : 0u;
        controllerAckRequired_ = minimumRetries_ != 0;
        if (controllerAckRequired_) {
            wchar_t runId[64] = {};
            GetPrivateProfileStringW(
                L"run",
                L"id",
                L"",
                runId,
                static_cast<DWORD>(
                    sizeof(runId) / sizeof(runId[0])),
                runFile.c_str());
            if (!CanonicalRunId(runId)) {
                configurationError_ =
                    "fault-armed login recovery requires a canonical run id";
                return false;
            }
            runId_ = runId;
            armEventName_ =
                L"Local\\SDGO.GameTest.LoginRecoveryArm." + runId_;
        }

        wchar_t hold[8] = {};
        GetPrivateProfileStringW(
            L"run",
            L"login_recovery_hold_after_completion",
            L"",
            hold,
            static_cast<DWORD>(sizeof(hold) / sizeof(hold[0])),
            runFile.c_str());
        if (wcscmp(hold, L"0") && wcscmp(hold, L"1")) {
            configurationError_ =
                "login_recovery_hold_after_completion must be canonical 0 or 1";
            return false;
        }
        holdAfterCompletion_ = hold[0] == L'1';
        enabled_ = true;
        return true;
    }

    bool Enabled() const noexcept { return enabled_; }
    const char* ConfigurationError() const noexcept
    {
        return configurationError_;
    }

    int PollLogin(
        const SdgoGameTestContextV1* context,
        NativeActions& actions) noexcept
    {
        if (!enabled_)
            return SDGO_GAME_TEST_FAIL;
        if (!context->tick)
            ResetCase(actions);

        SdgoLoginRecoverySnapshotV1 snapshot{};
        if (!ReadSnapshot(snapshot)) {
            if (!missingExportReported_) {
                context->Observe(
                    "waiting for the production login-recovery snapshot export");
                missingExportReported_ = true;
            }
            if (context->elapsed_ms < 5000)
                return SDGO_GAME_TEST_WAIT;
            return Fail(
                context,
                "the production login-recovery snapshot export is unavailable or incompatible");
        }
        ObserveSnapshot(context, snapshot);

        if (!sent_) {
            if (!ReadyForInitialLogin(snapshot))
                return SDGO_GAME_TEST_WAIT;
            if (snapshot.credentials_present || snapshot.recovery_active ||
                snapshot.interaction_wait ||
                snapshot.state != StateValue(
                    sdgo::login_recovery_client::State::Idle)) {
                return Fail(
                    context,
                    "the production Mod exposed stale recovery state before the initial login");
            }
            if (controllerAckRequired_) {
                if (!EnsureArmEvent())
                    return Fail(
                        context,
                        "the sealed login-recovery controller acknowledgement event could not be created");
                if (!readyToSendReported_) {
                    preArmTransportSequence_ =
                        actions.TransportState().send_sequence;
                    context->Observe(
                        "login_recovery_ready_to_send controller_ack_required=1 native_ui_ready=1");
                    readyToSendReported_ = true;
                }
                if (actions.TransportState().send_sequence !=
                    preArmTransportSequence_) {
                    return Fail(
                        context,
                        "native transport traffic appeared after fault arming but before the sealed login request");
                }
                const DWORD wait = WaitForSingleObject(armEvent_, 0);
                if (wait == WAIT_TIMEOUT)
                    return SDGO_GAME_TEST_WAIT;
                if (wait != WAIT_OBJECT_0)
                    return Fail(
                        context,
                        "the sealed login-recovery controller acknowledgement event became invalid");
                if (!controllerAckReported_) {
                    context->Observe(
                        "login_recovery_controller_ack armed=1 same_run=1");
                    controllerAckReported_ = true;
                }
            }

            baseline_ = snapshot;
            haveBaseline_ = true;
            lastAuthReplySequence_ =
                actions.LoginState().reply_sequence;
            const NativeActions::TransportObservation beforeTransport =
                actions.TransportState();
            if (controllerAckRequired_ &&
                beforeTransport.send_sequence !=
                    preArmTransportSequence_) {
                return Fail(
                    context,
                    "native transport traffic appeared between controller acknowledgement and the sealed login request");
            }
            const bool loginResult = actions.Login();
            const NativeActions::TransportObservation afterTransport =
                actions.TransportState();
            const bool nativeTransportValid =
                loginResult &&
                afterTransport.send_sequence ==
                    beforeTransport.send_sequence + 1u &&
                afterTransport.opcode == 0x2329 &&
                afterTransport.packet_size == 0x4D &&
                afterTransport.result != 0;
            context->Assert(
                nativeTransportValid,
                "the one permitted original login used native transport opcode 0x2329 with the 0x4D plaintext request and succeeded");
            if (!nativeTransportValid)
                return SDGO_GAME_TEST_FAIL;
            char nativeTransport[192] = {};
            sprintf_s(
                nativeTransport,
                "login_recovery_native_transport sequence=%u opcode=2329 size=77 success=1 result=%u",
                afterTransport.send_sequence,
                afterTransport.result);
            context->Observe(nativeTransport);
            sent_ = true;
            initialSendCount_ = 1;
            sentAtElapsed_ = context->elapsed_ms;
            context->Observe(
                "login_recovery_initial_send count=1 path=native_login_ctor retries_by_gametest=0");
            return SDGO_GAME_TEST_WAIT;
        }

        if (!haveBaseline_)
            return Fail(context, "login-recovery baseline was not captured");
        if (snapshot.generation == baseline_.generation) {
            if (context->elapsed_ms - sentAtElapsed_ < 5000)
                return SDGO_GAME_TEST_WAIT;
            return Fail(
                context,
                "the production Mod did not capture the genuine initial login request");
        }
        if (!generation_) {
            generation_ = snapshot.generation;
            char captured[192] = {};
            sprintf_s(
                captured,
                "login_recovery_native_capture generation=%llu state=%u active=%u credentials=%u",
                static_cast<unsigned long long>(snapshot.generation),
                snapshot.state,
                snapshot.recovery_active,
                snapshot.credentials_present);
            context->Observe(captured);
        } else if (snapshot.generation != generation_) {
            return Fail(
                context,
                "the production recovery generation changed after the single initial request");
        }

        if (snapshot.last_reject) {
            if (snapshot.last_reject != 1002)
                BeginTerminalWait(context, snapshot);
            else
                sawRetryable1002_ = true;
        }
        if (UnexpectedTerminal(snapshot))
            BeginTerminalWait(context, snapshot);

        const auto auth = actions.LoginState();
        if (auth.reply_sequence != lastAuthReplySequence_) {
            lastAuthReplySequence_ = auth.reply_sequence;
            char message[176] = {};
            sprintf_s(
                message,
                "login_recovery_real_auth opcode=%04X status=%u sequence=%u result=%u",
                auth.reply_opcode,
                auth.reply_status,
                auth.reply_sequence,
                auth.result);
            context->Observe(message);
            if (auth.result == NativeActions::kAuthRejected) {
                if (auth.reply_opcode != 0x232A ||
                    auth.reply_status != 1002) {
                    BeginTerminalWait(context, snapshot);
                } else {
                    sawRetryable1002_ = true;
                }
            } else if (auth.result == NativeActions::kAuthAccepted) {
                if (auth.reply_opcode != 0x232B ||
                    auth.reply_status != 0) {
                    return Fail(
                        context,
                        "the accepted native login observation is not a genuine 0x232B reply");
                }
                accepted_ = true;
                acceptedReplySequence_ = auth.reply_sequence;
            }
        }
        if (terminalSeen_)
            return PollTerminal(context, snapshot);

        if (!accepted_)
            return SDGO_GAME_TEST_WAIT;
        if (!LoginEvidenceComplete(snapshot))
            return SDGO_GAME_TEST_WAIT;

        context->Assert(
            initialSendCount_ == 1,
            "GameTest sent exactly one genuine initial login request and performed no retry");
        context->Assert(
            acceptedReplySequence_ != 0,
            "the original login service consumed a genuine 0x232B acknowledgement");
        context->Assert(
            snapshot.automatic_retries >= minimumRetries_,
            "the production Mod performed the minimum recovery retries required by the sealed scenario");
        context->Observe(
            "login recovery authentication passed to the standard native onboarding flow");
        return SDGO_GAME_TEST_PASS;
    }

    int VerifyCompletion(
        const SdgoGameTestContextV1* context,
        NativeActions& actions,
        std::uint32_t authenticatedPilot) noexcept
    {
        if (!enabled_)
            return SDGO_GAME_TEST_PASS;
        if (completionVerified_)
            return holdAfterCompletion_
                ? SDGO_GAME_TEST_WAIT
                : SDGO_GAME_TEST_PASS;
        if (!accepted_ || !generation_ || !haveBaseline_)
            return Fail(
                context,
                "onboarding reached completion without the earlier login-recovery authentication proof");

        SdgoLoginRecoverySnapshotV1 snapshot{};
        if (!ReadSnapshot(snapshot))
            return Fail(
                context,
                "the production login-recovery snapshot disappeared before onboarding completed");
        ObserveSnapshot(context, snapshot);

        const auto onboarding = actions.OnboardingState();
        const std::uint32_t appState = actions.AppState();
        const bool usableScene = appState == 5 || appState == 6;
        const bool succeeded =
            snapshot.state == StateValue(
                sdgo::login_recovery_client::State::Succeeded) &&
            snapshot.outcome == OutcomeValue(
                sdgo::login_recovery_client::Outcome::Success);
        if (!succeeded) {
            if (UnexpectedTerminal(snapshot))
                BeginTerminalWait(context, snapshot);
            if (terminalSeen_)
                return PollTerminal(context, snapshot);
            if (!completionWaitStarted_) {
                completionWaitStarted_ = true;
                completionWaitAt_ = context->elapsed_ms;
            }
            if (context->elapsed_ms - completionWaitAt_ < 10000)
                return SDGO_GAME_TEST_WAIT;
            return Fail(
                context,
                "the production recovery policy did not publish Succeeded after the usable native scene loaded");
        }

        const Counters counters = CounterDeltas(snapshot);
        const bool countersMonotonic = CountersMonotonic(snapshot);
        const bool pilotLoaded =
            authenticatedPilot != 0 &&
            onboarding.profile_pilot_id == authenticatedPilot;
        const bool snapshotCoherent =
            snapshot.generation == generation_ &&
            snapshot.app_state == appState &&
            !snapshot.credentials_present &&
            !snapshot.recovery_active &&
            !snapshot.interaction_wait;
        const bool retryCoherent =
            countersMonotonic &&
            snapshot.automatic_retries <= 3 &&
            snapshot.automatic_retries >= minimumRetries_ &&
            counters.retrySends >= minimumRetries_ &&
            counters.rebuilds >= minimumRetries_ &&
            counters.connectionGenerations >= minimumRetries_ &&
            counters.authReplies >= 1 &&
            (!sawRetryable1002_ ||
             (snapshot.last_reject == 1002 &&
              counters.authReplies >= 2 &&
              snapshot.automatic_retries >= 1 &&
              counters.retrySends >= 1 &&
              counters.rebuilds >= 1));

        context->Assert(
            usableScene && pilotLoaded,
            "the genuine accepted login loaded the same real pilot into the native lobby or hangar");
        context->Assert(
            snapshotCoherent,
            "the production recovery generation succeeded and erased its retained credentials");
        context->Assert(
            retryCoherent,
            "production retry, reconnect and authentic reply counters match the sealed recovery scenario");
        if (!usableScene || !pilotLoaded ||
            !snapshotCoherent || !retryCoherent)
            return SDGO_GAME_TEST_FAIL;

        char final[448] = {};
        sprintf_s(
            final,
            "login_recovery_final generation=%llu state=%u outcome=%u retries=%u retry_sends_delta=%u rebuilds_delta=%u connection_delta=%u auth_replies_delta=%u reject=%u app=%u pilot=%u credentials=%u active=%u interaction=%u",
            static_cast<unsigned long long>(snapshot.generation),
            snapshot.state,
            snapshot.outcome,
            snapshot.automatic_retries,
            counters.retrySends,
            counters.rebuilds,
            counters.connectionGenerations,
            counters.authReplies,
            snapshot.last_reject,
            snapshot.app_state,
            onboarding.profile_pilot_id,
            snapshot.credentials_present,
            snapshot.recovery_active,
            snapshot.interaction_wait);
        context->Observe(final);
        completionVerified_ = true;
        if (holdAfterCompletion_) {
            if (!holdReadyReported_) {
                context->Observe(
                    "login_recovery_hold_ready loaded_pilot=1 inventory_complete=1 awaiting_owned_controller");
                holdReadyReported_ = true;
            }
            return SDGO_GAME_TEST_WAIT;
        }
        return SDGO_GAME_TEST_PASS;
    }

private:
    struct Counters
    {
        std::uint32_t connectionGenerations;
        std::uint32_t rebuilds;
        std::uint32_t retrySends;
        std::uint32_t authReplies;
    };

    static std::uint32_t StateValue(
        sdgo::login_recovery_client::State value) noexcept
    {
        return static_cast<std::uint32_t>(value);
    }

    static std::uint32_t OutcomeValue(
        sdgo::login_recovery_client::Outcome value) noexcept
    {
        return static_cast<std::uint32_t>(value);
    }

    static bool CanonicalRunId(const wchar_t* value) noexcept
    {
        if (!value || wcslen(value) != 36)
            return false;
        for (std::size_t index = 0; index < 36; ++index) {
            const wchar_t character = value[index];
            const bool separator =
                index == 8 || index == 13 ||
                index == 18 || index == 23;
            if (separator) {
                if (character != L'-')
                    return false;
                continue;
            }
            if (!((character >= L'0' && character <= L'9') ||
                  (character >= L'a' && character <= L'f')))
                return false;
        }
        return true;
    }

    bool EnsureArmEvent() noexcept
    {
        if (armEvent_)
            return true;
        if (armEventName_.empty())
            return false;
        SetLastError(ERROR_SUCCESS);
        HANDLE event = CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            armEventName_.c_str());
        if (!event)
            return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(event);
            return false;
        }
        armEvent_ = event;
        return true;
    }

    void ResetCase(NativeActions& actions) noexcept
    {
        if (armEvent_) {
            CloseHandle(armEvent_);
            armEvent_ = nullptr;
        }
        snapshot_ = nullptr;
        missingExportReported_ = false;
        haveLastSnapshot_ = false;
        haveBaseline_ = false;
        sent_ = false;
        accepted_ = false;
        sawRetryable1002_ = false;
        completionWaitStarted_ = false;
        completionVerified_ = false;
        holdReadyReported_ = false;
        readyToSendReported_ = false;
        controllerAckReported_ = false;
        terminalSeen_ = false;
        terminalReported_ = false;
        initialSendCount_ = 0;
        sentAtElapsed_ = 0;
        completionWaitAt_ = 0;
        terminalWaitStartedAt_ = 0;
        generation_ = 0;
        preArmTransportSequence_ = 0;
        acceptedReplySequence_ = 0;
        lastAuthReplySequence_ =
            actions.LoginState().reply_sequence;
        baseline_ = {};
        lastSnapshot_ = {};
    }

    bool ReadSnapshot(
        SdgoLoginRecoverySnapshotV1& snapshot) noexcept
    {
        if (!snapshot_) {
            const HMODULE module =
                GetModuleHandleW(L"login_recovery_client.dll");
            if (module) {
                snapshot_ =
                    reinterpret_cast<
                        SdgoLoginRecoveryGetSnapshotV1Fn>(
                        GetProcAddress(
                            module,
                            SDGO_LOGIN_RECOVERY_SNAPSHOT_EXPORT));
            }
        }
        snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version = 1;
        return snapshot_ &&
            snapshot_(&snapshot) != 0 &&
            snapshot.size == sizeof(snapshot) &&
            snapshot.version == 1;
    }

    static bool ReadyForInitialLogin(
        const SdgoLoginRecoverySnapshotV1& snapshot) noexcept
    {
        return snapshot.app_state == 4 &&
            snapshot.gate_state == 3 &&
            snapshot.return_ready != 0 &&
            snapshot.recovery_active == 0;
    }

    bool UnexpectedTerminal(
        const SdgoLoginRecoverySnapshotV1& snapshot) const noexcept
    {
        using sdgo::login_recovery_client::State;
        const std::uint32_t state = snapshot.state;
        const bool terminalOutcome =
            snapshot.outcome != OutcomeValue(
                sdgo::login_recovery_client::Outcome::None) &&
            snapshot.outcome != OutcomeValue(
                sdgo::login_recovery_client::Outcome::Success);
        return terminalOutcome ||
            state == StateValue(State::Cancelled) ||
            state == StateValue(State::ReturnedToLogin) ||
            state == StateValue(State::ReturnToLoginFailed);
    }

    void BeginTerminalWait(
        const SdgoGameTestContextV1* context,
        const SdgoLoginRecoverySnapshotV1& snapshot) noexcept
    {
        if (terminalSeen_)
            return;
        terminalSeen_ = true;
        terminalWaitStartedAt_ = GetTickCount64();
        char message[224] = {};
        sprintf_s(
            message,
            "login_recovery_terminal_wait generation=%llu state=%u outcome=%u reject=%u",
            static_cast<unsigned long long>(snapshot.generation),
            snapshot.state,
            snapshot.outcome,
            snapshot.last_reject);
        context->Observe(message);
    }

    bool TerminalReady(
        const SdgoLoginRecoverySnapshotV1& snapshot) const noexcept
    {
        using sdgo::login_recovery_client::Outcome;
        using sdgo::login_recovery_client::State;
        const bool terminalState =
            snapshot.state == StateValue(State::Cancelled) ||
            snapshot.state == StateValue(State::ReturnedToLogin);
        const bool terminalOutcome =
            snapshot.outcome != OutcomeValue(Outcome::None) &&
            snapshot.outcome != OutcomeValue(Outcome::Success);
        // return_ready is the production Mod's native UI proof. A connected
        // login page commonly has gate 3; the offline fallback deliberately
        // reconstructs the same real controls with gate 1.
        return terminalState &&
            terminalOutcome &&
            snapshot.app_state == 4 &&
            snapshot.return_ready != 0 &&
            snapshot.credentials_present == 0 &&
            snapshot.recovery_active == 0 &&
            snapshot.interaction_wait == 0;
    }

    int PollTerminal(
        const SdgoGameTestContextV1* context,
        const SdgoLoginRecoverySnapshotV1& snapshot) noexcept
    {
        if (!terminalSeen_ || !generation_ || !haveBaseline_)
            return Fail(
                context,
                "terminal recovery was observed without a sealed native login generation");
        if (snapshot.generation != generation_)
            return Fail(
                context,
                "the production recovery generation changed while restoring the login page");
        if (!CountersMonotonic(snapshot))
            return Fail(
                context,
                "production recovery counters regressed while restoring the login page");
        if (TerminalReady(snapshot)) {
            const Counters counters = CounterDeltas(snapshot);
            if (!terminalReported_) {
                char terminal[448] = {};
                sprintf_s(
                    terminal,
                    "login_recovery_terminal generation=%llu state=%u outcome=%u retries=%u retry_sends_delta=%u rebuilds_delta=%u connection_delta=%u auth_replies_delta=%u reject=%u app=%u gate=%u ready=%u credentials=%u active=%u interaction=%u",
                    static_cast<unsigned long long>(snapshot.generation),
                    snapshot.state,
                    snapshot.outcome,
                    snapshot.automatic_retries,
                    counters.retrySends,
                    counters.rebuilds,
                    counters.connectionGenerations,
                    counters.authReplies,
                    snapshot.last_reject,
                    snapshot.app_state,
                    snapshot.gate_state,
                    snapshot.return_ready,
                    snapshot.credentials_present,
                    snapshot.recovery_active,
                    snapshot.interaction_wait);
                context->Observe(terminal);
                terminalReported_ = true;
            }
            return Fail(
                context,
                "production login recovery restored the native login page after a terminal outcome");
        }
        if (terminalWaitStartedAt_ &&
            GetTickCount64() - terminalWaitStartedAt_ < 30000)
            return SDGO_GAME_TEST_WAIT;
        return Fail(
            context,
            "production login recovery reached a terminal outcome but did not restore a credential-free native login page within 30 seconds");
    }

    bool LoginEvidenceComplete(
        const SdgoLoginRecoverySnapshotV1& snapshot) const noexcept
    {
        const Counters counters = CounterDeltas(snapshot);
        if (!CountersMonotonic(snapshot) ||
            snapshot.generation != generation_ ||
            snapshot.automatic_retries < minimumRetries_ ||
            snapshot.automatic_retries > 3 ||
            counters.retrySends < minimumRetries_ ||
            counters.rebuilds < minimumRetries_ ||
            counters.connectionGenerations < minimumRetries_ ||
            counters.authReplies < 1)
            return false;
        if (snapshot.last_reject &&
            snapshot.last_reject != 1002)
            return false;
        if (sawRetryable1002_ &&
            (snapshot.last_reject != 1002 ||
             snapshot.automatic_retries < 1 ||
             counters.retrySends < 1 ||
             counters.rebuilds < 1 ||
             counters.authReplies < 2))
            return false;
        return true;
    }

    Counters CounterDeltas(
        const SdgoLoginRecoverySnapshotV1& snapshot) const noexcept
    {
        return Counters{
            MonotonicDelta(
                snapshot.connection_generation,
                baseline_.connection_generation),
            MonotonicDelta(snapshot.rebuilds, baseline_.rebuilds),
            MonotonicDelta(
                snapshot.retry_sends,
                baseline_.retry_sends),
            MonotonicDelta(
                snapshot.auth_replies,
                baseline_.auth_replies),
        };
    }

    bool CountersMonotonic(
        const SdgoLoginRecoverySnapshotV1& snapshot) const noexcept
    {
        return snapshot.connection_generation >=
                baseline_.connection_generation &&
            snapshot.rebuilds >= baseline_.rebuilds &&
            snapshot.retry_sends >= baseline_.retry_sends &&
            snapshot.auth_replies >= baseline_.auth_replies;
    }

    static std::uint32_t MonotonicDelta(
        std::uint32_t current,
        std::uint32_t baseline) noexcept
    {
        return current >= baseline ? current - baseline : UINT32_MAX;
    }

    void ObserveSnapshot(
        const SdgoGameTestContextV1* context,
        const SdgoLoginRecoverySnapshotV1& snapshot) noexcept
    {
        if (haveLastSnapshot_ &&
            !memcmp(&lastSnapshot_, &snapshot, sizeof(snapshot)))
            return;
        char message[448] = {};
        sprintf_s(
            message,
            "login_recovery_snapshot generation=%llu state=%u outcome=%u retries=%u action=%u connection=%u rebuilds=%u retry_sends=%u auth_replies=%u reject=%u app=%u gate=%u purged=%u dropped=%u credentials=%u ready=%u active=%u interaction=%u",
            static_cast<unsigned long long>(snapshot.generation),
            snapshot.state,
            snapshot.outcome,
            snapshot.automatic_retries,
            snapshot.action_id,
            snapshot.connection_generation,
            snapshot.rebuilds,
            snapshot.retry_sends,
            snapshot.auth_replies,
            snapshot.last_reject,
            snapshot.app_state,
            snapshot.gate_state,
            snapshot.purged_packets,
            snapshot.dropped_callbacks,
            snapshot.credentials_present,
            snapshot.return_ready,
            snapshot.recovery_active,
            snapshot.interaction_wait);
        context->Observe(message);
        lastSnapshot_ = snapshot;
        haveLastSnapshot_ = true;
    }

    static int Fail(
        const SdgoGameTestContextV1* context,
        const char* message) noexcept
    {
        context->Assert(0, message);
        return SDGO_GAME_TEST_FAIL;
    }

    bool enabled_ = false;
    bool missingExportReported_ = false;
    bool haveLastSnapshot_ = false;
    bool haveBaseline_ = false;
    bool sent_ = false;
    bool accepted_ = false;
    bool sawRetryable1002_ = false;
    bool completionWaitStarted_ = false;
    bool completionVerified_ = false;
    bool holdAfterCompletion_ = false;
    bool holdReadyReported_ = false;
    bool controllerAckRequired_ = false;
    bool readyToSendReported_ = false;
    bool controllerAckReported_ = false;
    bool terminalSeen_ = false;
    bool terminalReported_ = false;
    std::uint32_t minimumRetries_ = 0;
    std::uint32_t initialSendCount_ = 0;
    std::uint32_t preArmTransportSequence_ = 0;
    std::uint32_t acceptedReplySequence_ = 0;
    std::uint32_t lastAuthReplySequence_ = 0;
    std::uint64_t sentAtElapsed_ = 0;
    std::uint64_t completionWaitAt_ = 0;
    std::uint64_t terminalWaitStartedAt_ = 0;
    std::uint64_t generation_ = 0;
    const char* configurationError_ = nullptr;
    HANDLE armEvent_ = nullptr;
    std::wstring runId_;
    std::wstring armEventName_;
    SdgoLoginRecoveryGetSnapshotV1Fn snapshot_ = nullptr;
    SdgoLoginRecoverySnapshotV1 baseline_{};
    SdgoLoginRecoverySnapshotV1 lastSnapshot_{};
};
