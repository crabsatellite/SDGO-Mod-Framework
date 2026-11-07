#include <cstdio>

#include "../mods/login_recovery_client/policy.h"

namespace {

using sdgo::login_recovery_client::Action;
using sdgo::login_recovery_client::ActionKind;
using sdgo::login_recovery_client::Config;
using sdgo::login_recovery_client::Outcome;
using sdgo::login_recovery_client::Policy;
using sdgo::login_recovery_client::State;

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

Config TestConfig() {
    Config config;
    config.initialOutcomeTimeoutMs = 100;
    config.retryOutcomeTimeoutMs = 80;
    config.actionTimeoutMs = 20;
    config.totalTimeoutMs = 1'000;
    config.retryBackoffMs = {10, 20, 40};
    return config;
}

void TestDefaultNativeBudgets() {
    const Config config;
    Expect(config.initialOutcomeTimeoutMs == 15'000 &&
               config.retryOutcomeTimeoutMs == 15'000,
           "default outcome phases allow normal native loading");
    Expect(config.actionTimeoutMs == 10'000 &&
               config.totalTimeoutMs == 90'000,
           "default action and total budgets are bounded");
    Expect(config.retryBackoffMs[0] == 1'000 &&
               config.retryBackoffMs[1] == 2'000 &&
               config.retryBackoffMs[2] == 4'000,
           "default retry backoff is one, two and four seconds");
}

void TestBackoffAndRetryLimit() {
    Policy policy(TestConfig());
    const auto generation = policy.Begin(1'000);

    Expect(!policy.Tick(1'099), "initial phase remains live before timeout");
    Expect(!policy.Tick(1'100), "initial timeout starts first backoff");
    Expect(policy.state() == State::Backoff,
           "initial timeout enters backoff");
    Expect(!policy.Tick(1'109), "first retry observes its backoff");

    const Action first = policy.Tick(1'110);
    Expect(first.kind == ActionKind::RetryLogin && first.retryNumber == 1,
           "first retry is issued after the first backoff");
    Expect(!policy.Tick(1'110), "retry action is not resent every tick");
    Expect(!policy.RecoveryCompleted(
               generation, first.id + 1, true, 1'111),
           "wrong action id cannot complete a retry");
    Expect(policy.RecoveryCompleted(generation, first.id, true, 1'111),
           "adapter confirms first retry dispatch");

    Expect(policy.Progress(generation, 1'170),
           "progress refreshes only the current phase timeout");
    Expect(!policy.Tick(1'199), "refreshed retry phase remains live");
    Expect(policy.Reject(generation, 1002, true, 1'200),
           "authenticated online rejection remains retryable");
    Expect(!policy.Tick(1'219), "second retry observes longer backoff");
    const Action second = policy.Tick(1'220);
    Expect(second.kind == ActionKind::RetryLogin && second.retryNumber == 2,
           "second retry is issued after second backoff");
    Expect(policy.RecoveryCompleted(generation, second.id, true, 1'221),
           "adapter confirms second retry dispatch");

    Expect(policy.TransportFailure(generation, 1'222),
           "explicit transport failure remains retryable");
    Expect(!policy.Tick(1'261), "third retry observes longest backoff");
    const Action third = policy.Tick(1'262);
    Expect(third.kind == ActionKind::RetryLogin && third.retryNumber == 3,
           "third and final retry is issued");
    Expect(policy.RecoveryCompleted(generation, third.id, true, 1'263),
           "adapter confirms third retry dispatch");

    Expect(policy.Reject(generation, 1002, true, 1'264),
           "fourth retryable result is consumed");
    Expect(policy.state() == State::ReturnToLoginQueued &&
               policy.outcome() == Outcome::RetryLimitReached,
           "retry limit queues an explicit return");
    Expect(!policy.Reject(generation, 1002, true, 1'264),
           "duplicate rejection cannot restart the retry budget");

    const Action fallback = policy.Tick(1'264);
    Expect(fallback.kind == ActionKind::ReturnToLogin,
           "retry exhaustion issues ReturnToLogin");
    Expect(!policy.Tick(1'264), "ReturnToLogin is not resent every tick");
    Expect(policy.RecoveryCompleted(
               generation, fallback.id, true, 1'265),
           "adapter confirms ReturnToLogin");
    Expect(policy.state() == State::ReturnedToLogin,
           "confirmed fallback reaches terminal returned state");
}

void TestTerminalRejects() {
    struct Case {
        int code;
        bool authenticated;
        Outcome outcome;
        const char* label;
    };
    const Case cases[] = {
        {1001, false, Outcome::PasswordRejected, "wrong password"},
        {1003, true, Outcome::Banned, "ban"},
        {1002, false, Outcome::OnlineRejectedBeforeAuthentication,
         "unauthenticated online"},
        {7777, true, Outcome::UnknownRejected, "unknown rejection"},
    };

    Policy policy(TestConfig());
    std::uint64_t now = 2'000;
    for (const Case& test : cases) {
        const auto generation = policy.Begin(now);
        Expect(policy.Reject(
                   generation, test.code, test.authenticated, now + 1),
               test.label);
        Expect(policy.outcome() == test.outcome &&
                   policy.retries_scheduled() == 0,
               "terminal rejection never consumes a retry");
        const Action fallback = policy.Tick(now + 1);
        Expect(fallback.kind == ActionKind::ReturnToLogin,
               "terminal rejection returns to login");
        Expect(policy.RecoveryCompleted(
                   generation, fallback.id, true, now + 2),
               "terminal fallback is adapter-confirmed");
        Expect(policy.state() == State::ReturnedToLogin,
               "terminal rejection completes once");
        Expect(!policy.Reject(
                   generation, test.code, test.authenticated, now + 3),
               "terminal rejection is idempotent");
        now += 100;
    }
}

void TestActionFailureAndTimeout() {
    Config config = TestConfig();
    config.retryBackoffMs = {0, 0, 0};
    Policy policy(config);
    const auto generation = policy.Begin(3'000);

    Expect(policy.TransportFailure(generation, 3'001),
           "transport failure schedules retry");
    const Action retry = policy.Tick(3'001);
    Expect(retry.kind == ActionKind::RetryLogin,
           "zero backoff retry is issued in the next tick");
    Expect(policy.RecoveryCompleted(generation, retry.id, false, 3'002),
           "adapter retry failure is consumed");
    Expect(policy.outcome() == Outcome::RetryActionFailed,
           "adapter failure records a precise outcome");
    const Action fallback = policy.Tick(3'002);
    Expect(fallback.kind == ActionKind::ReturnToLogin,
           "failed retry action has explicit fallback");
    Expect(policy.RecoveryCompleted(
               generation, fallback.id, false, 3'003),
           "failed ReturnToLogin completion is consumed once");
    Expect(policy.state() == State::ReturnToLoginFailed,
           "failed fallback terminates without a loop");
    Expect(!policy.Tick(3'100), "failed fallback is never reissued");
    Expect(!policy.RecoveryCompleted(
               generation, fallback.id, true, 3'101),
           "late duplicate completion cannot change terminal state");

    const auto nextGeneration = policy.Begin(4'000);
    Expect(policy.TransportFailure(nextGeneration, 4'001),
           "new manual login starts a fresh retry budget");
    const Action timed = policy.Tick(4'001);
    Expect(!policy.Tick(4'020),
           "retry action remains pending before its deadline");
    const Action timedFallback = policy.Tick(4'021);
    Expect(timedFallback.kind == ActionKind::ReturnToLogin &&
               policy.outcome() == Outcome::RetryActionTimedOut,
           "unconfirmed retry action times out to explicit fallback");
    Expect(!policy.RecoveryCompleted(
               nextGeneration, timed.id, true, 4'022),
           "late retry completion cannot replace fallback");
    Expect(policy.RecoveryCompleted(
               nextGeneration, timedFallback.id, true, 4'022),
           "timed-out retry fallback can complete");
}

void TestTotalDeadlineCannotBeExtended() {
    Config config = TestConfig();
    config.initialOutcomeTimeoutMs = 500;
    config.retryOutcomeTimeoutMs = 500;
    config.totalTimeoutMs = 50;
    config.retryBackoffMs = {100, 100, 100};
    Policy policy(config);
    const auto generation = policy.Begin(5'000);

    Expect(policy.Progress(generation, 5'049),
           "progress is accepted before total deadline");
    const Action fallback = policy.Tick(5'050);
    Expect(fallback.kind == ActionKind::ReturnToLogin &&
               policy.outcome() == Outcome::TotalDeadlineExceeded,
           "progress cannot extend the total recovery deadline");
    Expect(policy.RecoveryCompleted(
               generation, fallback.id, true, 5'051),
           "deadline fallback can finish after budget expiry");

    const auto backoffGeneration = policy.Begin(6'000);
    Expect(policy.TransportFailure(backoffGeneration, 6'001),
           "transport failure starts long backoff");
    Expect(!policy.Tick(6'049), "long backoff remains below total deadline");
    const Action backoffFallback = policy.Tick(6'050);
    Expect(backoffFallback.kind == ActionKind::ReturnToLogin &&
               policy.outcome() == Outcome::TotalDeadlineExceeded,
           "total deadline preempts an unfinished backoff");
}

void TestInteractionPauseAndLateCallbacks() {
    Config config = TestConfig();
    config.totalTimeoutMs = 300;
    config.retryBackoffMs = {5, 10, 20};
    Policy policy(config);
    const auto initialGeneration = policy.Begin(10'000);

    Expect(policy.TransportFailure(initialGeneration, 10'001),
           "pre-interaction failure consumes first retry");
    const Action retry = policy.Tick(10'006);
    Expect(retry.kind == ActionKind::RetryLogin,
           "pre-interaction retry follows normal backoff");
    Expect(policy.RecoveryCompleted(
               initialGeneration, retry.id, true, 10'007),
           "retry rebuild completes before user interaction");
    Expect(policy.Progress(initialGeneration, 10'020),
           "native authenticated progress refreshes loading phase");
    Expect(policy.PauseForInteraction(initialGeneration, 10'030),
           "nickname or terms interaction pauses recovery clocks");
    Expect(policy.state() == State::PausedForInteraction &&
               policy.retries_scheduled() == 1,
           "interaction preserves the consumed retry budget");

    Expect(!policy.Tick(100'000),
           "arbitrary user wait emits no credential replay");
    Expect(!policy.TransportFailure(initialGeneration, 100'001) &&
               !policy.Reject(initialGeneration, 1002, true, 100'002) &&
               !policy.Success(initialGeneration, 100'003),
           "callbacks cannot change policy while interaction is paused");
    Expect(policy.retries_scheduled() == 1,
           "paused callbacks do not consume additional retries");

    const auto resumedGeneration =
        policy.ResumeAfterInteraction(initialGeneration, 100'010);
    Expect(resumedGeneration != 0 &&
               resumedGeneration != initialGeneration,
           "resume returns a fresh event generation");
    Expect(policy.state() == State::AwaitingOutcome &&
               policy.retries_scheduled() == 1,
           "resume continues normal loading with the existing retry budget");
    Expect(!policy.TransportFailure(initialGeneration, 200'000) &&
               !policy.Success(initialGeneration, 200'001),
           "late callbacks captured before interaction stay stale");
    Expect(policy.Progress(resumedGeneration, 100'011),
           "stale callbacks do not advance the current monotonic clock");
    Expect(!policy.Tick(100'090),
           "resumed loading receives a complete retry phase");
    Expect(!policy.Tick(100'091) &&
               policy.state() == State::Backoff &&
               policy.retries_scheduled() == 2,
           "resumed loading times out into the next preserved retry");

    Policy totalPolicy(config);
    const auto totalGeneration = totalPolicy.Begin(20'000);
    Expect(totalPolicy.PauseForInteraction(totalGeneration, 20'050),
           "total budget can pause during interaction");
    const auto totalResumed =
        totalPolicy.ResumeAfterInteraction(totalGeneration, 300'000);
    Expect(totalResumed != 0, "long interaction resumes successfully");
    Expect(totalPolicy.Progress(totalResumed, 300'099) &&
               totalPolicy.Progress(totalResumed, 300'198),
           "normal loading progress can span the restored total budget");
    Expect(totalPolicy.Progress(totalResumed, 300'249),
           "user wait is excluded from the total deadline");
    const Action fallback = totalPolicy.Tick(300'250);
    Expect(fallback.kind == ActionKind::ReturnToLogin &&
               totalPolicy.outcome() == Outcome::TotalDeadlineExceeded,
           "only the remaining pre-interaction total budget is restored");

    Policy sameAttempt(config);
    const auto sameGeneration = sameAttempt.Begin(21'000);
    Expect(sameAttempt.PauseForInteraction(sameGeneration, 21'010),
           "same-attempt native interaction can pause");
    Expect(sameAttempt.ResumeAfterInteraction(
               sameGeneration, 30'000, false) == sameGeneration,
           "same TCP attempt can resume without rotating generation");
    Expect(sameAttempt.Progress(sameGeneration, 30'001),
           "same-attempt native replies retain their valid generation");
}

void TestProgressAndRetryDispatchAreNotLoginSuccess() {
    Config config = TestConfig();
    config.initialOutcomeTimeoutMs = 100;
    config.retryOutcomeTimeoutMs = 100;
    config.retryBackoffMs = {5, 10, 20};
    Policy policy(config);
    const auto generation = policy.Begin(30'000);

    Expect(policy.Progress(generation, 30'090),
           "authenticated native progress is accepted");
    Expect(policy.state() == State::AwaitingOutcome &&
               policy.outcome() == Outcome::None,
           "authenticated progress does not claim login success");
    Expect(!policy.Tick(30'188),
           "progress leaves time for remaining native loading");

    Expect(policy.TransportFailure(generation, 30'189),
           "TCP failure after progress remains retryable");
    const Action firstRetry = policy.Tick(30'194);
    Expect(firstRetry.kind == ActionKind::RetryLogin,
           "transport failure produces a retry action");
    Expect(policy.RecoveryCompleted(
               generation, firstRetry.id, true, 30'195),
           "native rebuild reports successful action completion");
    Expect(policy.state() == State::AwaitingOutcome &&
               policy.outcome() == Outcome::None,
           "successful rebuild is not a successful login");
    Expect(policy.TransportFailure(generation, 30'196),
           "TCP connect failure after rebuild consumes the next retry");
    Expect(policy.state() == State::Backoff &&
               policy.retries_scheduled() == 2,
           "rebuild success followed by transport failure keeps retry policy");
}

void TestOutcomeEventsWinBeforeActionIssuance() {
    Config config = TestConfig();
    config.initialOutcomeTimeoutMs = 100;
    config.retryBackoffMs = {10, 20, 40};

    Policy exactReject(config);
    const auto rejectGeneration = exactReject.Begin(40'000);
    Expect(exactReject.Reject(
               rejectGeneration, 1001, false, 40'100),
           "terminal reject wins exactly at the phase deadline");
    Expect(exactReject.state() == State::ReturnToLoginQueued &&
               exactReject.outcome() == Outcome::PasswordRejected &&
               exactReject.retries_scheduled() == 0,
           "exact-deadline password reject does not create a retry");

    Policy exactSuccess(config);
    const auto successGeneration = exactSuccess.Begin(41'000);
    Expect(exactSuccess.Success(successGeneration, 41'100),
           "usable native scene wins exactly at the phase deadline");
    Expect(exactSuccess.state() == State::Succeeded &&
               exactSuccess.retries_scheduled() == 0,
           "exact-deadline success completes without a retry");

    Policy exactProgress(config);
    const auto progressGeneration = exactProgress.Begin(41'500);
    Expect(exactProgress.Progress(progressGeneration, 41'600),
           "real progress wins exactly at the phase deadline");
    Expect(exactProgress.state() == State::AwaitingOutcome &&
               exactProgress.retries_scheduled() == 0 &&
               !exactProgress.Tick(41'699),
           "exact-deadline progress restores loading without a rebuild");

    Policy exactPause(config);
    const auto pauseGeneration = exactPause.Begin(41'700);
    Expect(exactPause.PauseForInteraction(pauseGeneration, 41'800),
           "user interaction wins exactly at the phase deadline");
    Expect(exactPause.state() == State::PausedForInteraction &&
               exactPause.retries_scheduled() == 0 &&
               !exactPause.Tick(50'000),
           "exact-deadline interaction emits no credential replay");

    Policy backoffReject(config);
    const auto backoffRejectGeneration = backoffReject.Begin(42'000);
    Expect(!backoffReject.Tick(42'100) &&
               backoffReject.state() == State::Backoff,
           "no-event phase timeout enters an unissued backoff");
    Expect(backoffReject.Reject(
               backoffRejectGeneration, 1003, true, 42'101),
           "ban reply overrides an unissued timeout retry");
    Expect(backoffReject.state() == State::ReturnToLoginQueued &&
               backoffReject.outcome() == Outcome::Banned &&
               backoffReject.retries_scheduled() == 0,
           "backoff terminal reject preserves its real outcome");

    Policy unknownBackoff(config);
    const auto unknownGeneration = unknownBackoff.Begin(43'000);
    Expect(!unknownBackoff.Tick(43'100),
           "unknown reject case enters unissued backoff");
    Expect(unknownBackoff.Reject(
               unknownGeneration, 7777, true, 43'101),
           "unknown terminal reply overrides unissued backoff");
    Expect(unknownBackoff.outcome() == Outcome::UnknownRejected &&
               unknownBackoff.retries_scheduled() == 0,
           "unknown terminal outcome remains explicit");

    Policy backoffSuccess(config);
    const auto backoffSuccessGeneration = backoffSuccess.Begin(44'000);
    Expect(!backoffSuccess.Tick(44'100),
           "success case enters unissued backoff");
    Expect(backoffSuccess.Success(
               backoffSuccessGeneration, 44'101),
           "confirmed usable scene overrides unissued backoff");
    Expect(backoffSuccess.state() == State::Succeeded &&
               backoffSuccess.retries_scheduled() == 0 &&
               !backoffSuccess.Tick(45'000),
           "overridden backoff never issues a credential replay");

    Policy backoffProgress(config);
    const auto backoffProgressGeneration = backoffProgress.Begin(45'000);
    Expect(!backoffProgress.Tick(45'100) &&
               backoffProgress.retries_scheduled() == 1,
           "progress case enters an unissued timeout backoff");
    Expect(backoffProgress.Progress(
               backoffProgressGeneration, 45'101),
           "real authentication progress overrides unissued backoff");
    Expect(backoffProgress.state() == State::AwaitingOutcome &&
               backoffProgress.retries_scheduled() == 0,
           "cancelled unissued backoff does not consume retry budget");
    Expect(!backoffProgress.Tick(45'200),
           "progress restores a complete normal loading phase");

    Policy backoffPause(config);
    const auto backoffPauseGeneration = backoffPause.Begin(46'000);
    Expect(!backoffPause.Tick(46'100) &&
               backoffPause.retries_scheduled() == 1,
           "interaction case enters an unissued timeout backoff");
    Expect(backoffPause.PauseForInteraction(
               backoffPauseGeneration, 46'101),
           "current user interaction cancels unissued backoff");
    Expect(backoffPause.state() == State::PausedForInteraction &&
               backoffPause.retries_scheduled() == 0 &&
               !backoffPause.Tick(60'000),
           "paused interaction never performs the cancelled rebuild");
}

void TestIssuedActionAndTotalDeadlineStillWin() {
    Config config = TestConfig();
    config.initialOutcomeTimeoutMs = 100;
    config.totalTimeoutMs = 200;
    config.retryBackoffMs = {0, 0, 0};

    Policy issued(config);
    const auto issuedGeneration = issued.Begin(50'000);
    const Action retry = issued.Tick(50'100);
    Expect(retry.kind == ActionKind::RetryLogin &&
               issued.state() == State::AwaitingRecoveryCompletion,
           "zero-backoff timeout issues a retry action");
    Expect(!issued.Success(issuedGeneration, 50'100) &&
               !issued.Reject(
                   issuedGeneration, 1001, false, 50'100),
           "success and reject cannot bypass an issued action");
    Expect(issued.state() == State::AwaitingRecoveryCompletion,
           "issued action still requires adapter completion");

    Policy totalSuccess(config);
    const auto totalSuccessGeneration = totalSuccess.Begin(51'000);
    Expect(!totalSuccess.Success(totalSuccessGeneration, 51'200),
           "success at the total deadline is not accepted");
    Expect(totalSuccess.state() == State::ReturnToLoginQueued &&
               totalSuccess.outcome() == Outcome::TotalDeadlineExceeded,
           "total deadline remains the hard terminal boundary");
    Expect(!totalSuccess.Success(totalSuccessGeneration, 51'201),
           "success cannot revive an expired total budget");

    Policy totalReject(config);
    const auto totalRejectGeneration = totalReject.Begin(52'000);
    Expect(!totalReject.Reject(
               totalRejectGeneration, 1003, true, 52'200),
           "terminal reject at the total deadline is not accepted");
    Expect(totalReject.outcome() == Outcome::TotalDeadlineExceeded,
           "expired budget reports deadline rather than late reject");
    Expect(!totalReject.Reject(
               totalRejectGeneration, 1001, false, 99'999),
           "later terminal events cannot extend an expired budget");
}

void TestRetryPhaseAndReturnActionTimeouts() {
    Config config = TestConfig();
    config.retryBackoffMs = {5, 5, 5};
    Policy policy(config);
    const auto generation = policy.Begin(7'000);

    Expect(policy.TransportFailure(generation, 7'001),
           "transport failure starts first timed retry");
    const Action retry = policy.Tick(7'006);
    Expect(retry.kind == ActionKind::RetryLogin,
           "first timed retry follows backoff");
    Expect(policy.RecoveryCompleted(generation, retry.id, true, 7'007),
           "timed retry action is confirmed");
    Expect(!policy.Tick(7'086),
           "retry response phase remains live before its timeout");
    Expect(!policy.Tick(7'087),
           "retry response timeout starts the next backoff");
    Expect(policy.state() == State::Backoff &&
               policy.retries_scheduled() == 2,
           "retry response has its own bounded phase timeout");

    const auto rejectGeneration = policy.Begin(8'000);
    Expect(policy.Reject(rejectGeneration, 1001, false, 8'001),
           "terminal reject queues return timeout test");
    const Action fallback = policy.Tick(8'001);
    Expect(fallback.kind == ActionKind::ReturnToLogin,
           "terminal reject issues one return action");
    Expect(!policy.Tick(8'020),
           "return action remains pending before confirmation timeout");
    Expect(!policy.Tick(8'021) &&
               policy.state() == State::ReturnToLoginFailed,
           "unconfirmed return action terminates at its own timeout");
    Expect(!policy.Tick(9'000),
           "timed-out return action is never issued again");
    Expect(!policy.RecoveryCompleted(
               rejectGeneration, fallback.id, true, 9'001),
           "late return completion cannot revive terminal state");
}

void TestGenerationIsolationSuccessAndCancel() {
    Config config = TestConfig();
    config.retryBackoffMs = {0, 0, 0};
    Policy policy(config);
    const auto oldGeneration = policy.Begin(7'000);
    Expect(policy.TransportFailure(oldGeneration, 7'001),
           "old generation schedules work");
    const Action oldAction = policy.Tick(7'001);

    const auto currentGeneration = policy.Begin(7'002);
    Expect(currentGeneration != oldGeneration,
           "manual login advances generation");
    Expect(!policy.RecoveryCompleted(
               oldGeneration, oldAction.id, true, 9'000),
           "old completion cannot affect new attempt or its clock");
    Expect(!policy.Success(oldGeneration, 9'000),
           "old success cannot affect new attempt");
    Expect(!policy.Reject(oldGeneration, 1001, false, 9'000),
           "old reject cannot affect new attempt");
    Expect(policy.state() == State::AwaitingOutcome &&
               policy.generation() == currentGeneration,
           "new manual login keeps its independent state");
    Expect(policy.Success(currentGeneration, 7'003),
           "current generation can succeed after late old events");
    Expect(policy.state() == State::Succeeded &&
               policy.outcome() == Outcome::Success,
           "success is terminal");
    Expect(!policy.Success(currentGeneration, 7'004),
           "duplicate success is idempotent");

    const auto cancelGeneration = policy.Begin(8'000);
    Expect(policy.TransportFailure(cancelGeneration, 8'001),
           "cancel test enters recovery");
    Expect(policy.Cancel(cancelGeneration, 8'002),
           "user cancellation aborts recovery budget");
    Expect(policy.state() == State::Cancelled &&
               policy.outcome() == Outcome::UserCancelled,
           "cancel is terminal without fallback resend");
    Expect(!policy.Cancel(cancelGeneration, 8'003),
           "duplicate cancellation is idempotent");
    Expect(!policy.Tick(9'000), "cancelled recovery emits no later action");
}

}  // namespace

int main() {
    TestDefaultNativeBudgets();
    TestBackoffAndRetryLimit();
    TestTerminalRejects();
    TestActionFailureAndTimeout();
    TestTotalDeadlineCannotBeExtended();
    TestInteractionPauseAndLateCallbacks();
    TestProgressAndRetryDispatchAreNotLoginSuccess();
    TestOutcomeEventsWinBeforeActionIssuance();
    TestIssuedActionAndTotalDeadlineStillWin();
    TestRetryPhaseAndReturnActionTimeouts();
    TestGenerationIsolationSuccessAndCancel();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test assertion(s) failed\n", g_failures);
        return 1;
    }
    std::puts(
        "PASS client login recovery timing, retry bounds, adapter "
        "completion, interaction pause, generations, native progress, "
        "idempotency and terminal rejects");
    return 0;
}
