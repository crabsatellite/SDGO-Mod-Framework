// OFFLINE unit checks for the passive GameTest server observer.
//
// This includes the production implementation in the same translation unit so
// its lifecycle, correlation, filtering and bounded queue logic can be checked
// without a BattleServer process. Passing these checks is not evidence of real
// authentication, room creation, PvP AI setup, battle ticks or server runtime.

#include "../mods/game_test_server/game_test_server.cpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    unsigned g_checks = 0;
    unsigned g_failures = 0;
    std::string g_roomTitle;

    uint32_t __cdecl QueryRoomTitle(
        uint32_t pilot, char* output, uint32_t capacity)
    {
        if (pilot != 9229 || !output ||
            g_roomTitle.size() + 1 > capacity) {
            return 0;
        }
        memcpy(output, g_roomTitle.c_str(), g_roomTitle.size() + 1);
        return static_cast<uint32_t>(g_roomTitle.size());
    }

    const SdgoRoomTitleApi kRoomTitleApi = {
        sizeof(SdgoRoomTitleApi),
        SDGO_ROOM_TITLE_API_VERSION,
        &QueryRoomTitle};

    void Check(bool passed, const char* expression, int line)
    {
        ++g_checks;
        if (passed)
            return;
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }

#define CHECK(expression) Check((expression), #expression, __LINE__)

    SdgoPvpAiObserverEventV1 Event(
        uint32_t type,
        uint32_t roomCookie = 0x10203040u,
        uint32_t hostPilotId = 9229,
        uint8_t mode = 3,
        const char* title = "!ai e2 T7x")
    {
        SdgoPvpAiObserverEventV1 event = {};
        event.size = sizeof(event);
        event.version = SDGO_PVP_AI_OBSERVER_VERSION;
        event.type = type;
        event.room_cookie = roomCookie;
        event.host_pilot_id = hostPilotId;
        event.mode = mode;
        event.ai_team_1 = 1;
        event.ai_team_2 = 1;
        event.active_slots = 3;
        if (title)
            strncpy_s(event.title, title, _TRUNCATE);
        return event;
    }

    void ResetConfig(
        const char* nonce = "T7x",
        uint32_t expectedHost = 9229,
        uint8_t mode = 3,
        uint8_t minimumAi = 1,
        bool requireEnd = false)
    {
        g_config = RunConfig{};
        strcpy_s(g_config.nonce, nonce);
        g_config.mode = mode;
        g_config.minimumAi = minimumAi;
        g_config.requireEnd = requireEnd;
        InterlockedExchange(&g_expectedHost, static_cast<LONG>(expectedHost));
    }

    void ResetQueue()
    {
        EnterCriticalSection(&g_queueLock);
        memset(g_queue, 0, sizeof(g_queue));
        g_queueRead = 0;
        g_queueWrite = 0;
        g_queueCount = 0;
        memset(g_damageQueue, 0, sizeof(g_damageQueue));
        g_damageQueueRead = 0;
        g_damageQueueWrite = 0;
        g_damageQueueCount = 0;
        LeaveCriticalSection(&g_queueLock);
        InterlockedExchange(&g_dropped, 0);
        InterlockedExchange(&g_damageDropped, 0);
        InterlockedExchange(&g_damageSequence, 0);
    }

    void TestAbiAndValidation()
    {
        CHECK(sizeof(SdgoPvpAiObserverSlotV1) ==
              SDGO_PVP_AI_OBSERVER_SLOT_V1_SIZE);
        CHECK(sizeof(SdgoPvpAiObserverEventV1) ==
              SDGO_PVP_AI_OBSERVER_EVENT_V1_SIZE);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, sequence) == 16);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, title) == 60);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, slots) == 84);

        CHECK(IsUuid("01234567-89ab-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("01234567-89AB-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("0123456789ab-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("01234567-89ab-cdef-0123-456789abcdeg"));

        ResetConfig();
        CHECK(TitleContainsNonce("prefix-T7x"));
        CHECK(!TitleContainsNonce("prefix-t7x"));
        CHECK(!TitleContainsNonce("no marker"));
        CHECK(std::string(EventName(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE)) ==
              "battle_active");
        CHECK(std::string(EventName(0xffffffffu)) == "unknown");
        CHECK(EscapeJson("\"\\\n\t") == "\\\"\\\\\\n\\t");
    }

    bool EmitObserverReadyJson(const char* source)
    {
        ResetConfig("T7x", 9229, 3, 2, true);
        strcpy_s(
            g_config.runId,
            "01234567-89ab-cdef-0123-456789abcdef");
        strcpy_s(g_config.suite, "pvp-ai");
        if (strcmp(source, "battle_rewards_bridge") == 0) {
            g_deathEnabled = true;
            g_damageSource = DamageSource::BattleRewardsBridge;
        } else if (strcmp(source, "direct_native_hook") == 0) {
            g_deathEnabled = true;
            g_damageSource = DamageSource::DirectHook;
        } else if (strcmp(source, "disabled") == 0) {
            g_deathEnabled = false;
            g_damageSource = DamageSource::None;
        } else {
            return false;
        }
        const std::string ready = ObserverReadyJson(
            12345, "BattleServer-548A891D-0046C000");
        return std::fwrite(
                   ready.data(), 1, ready.size(), stdout) ==
            ready.size();
    }

    void TestObserverFiltering()
    {
        ResetConfig();
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);

        SdgoPvpAiObserverEventV1 invalid = event;
        invalid.size = sizeof(invalid) - 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.version = SDGO_PVP_AI_OBSERVER_VERSION + 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.host_pilot_id = event.host_pilot_id + 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.mode = static_cast<uint8_t>(event.mode + 1);
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        strcpy_s(invalid.title, "!ai e2 wrong");
        ObservePvpAi(&invalid, nullptr);
        CHECK(g_queueCount == 0);

        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);
        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].host_pilot_id == 9229);
        CHECK(output[0].room_cookie == 0x10203040u);
        CHECK(g_queueCount == 0);
    }

    void TestObserverWildcardAndDisabledHost()
    {
        ResetConfig("T7x", 0, 0);
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 7, 9229, 12);
        ObservePvpAi(nullptr, nullptr);
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 0);

        ResetConfig("T7x", 9229, 0);
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);

        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].mode == 12);
        CHECK(g_queueCount == 0);
    }

    void TestTitleBoundary()
    {
        ResetConfig("ABCDEFGH");
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 7, 9229, 3, nullptr);
        memset(event.title, 'x', sizeof(event.title));
        memcpy(event.title + 12, "ABCDEFGH", 8);
        event.title[20] = 'Z'; // Deliberately no NUL in the producer's 21 bytes.
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);

        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].title[19] == 'H');
        CHECK(output[0].title[20] == '\0');
        CHECK(strlen(output[0].title) == 20);
        CHECK(TitleContainsNonce(output[0].title));
    }

    void TestNormalLifecycle()
    {
        ResetConfig("T7x", 9229, 3, 2, false);
        Lifecycle lifecycle;
        const SdgoPvpAiObserverEventV1 request =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);
        SdgoPvpAiObserverEventV1 setup =
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE);
        setup.ai_team_1 = 1;
        setup.ai_team_2 = 1;
        const SdgoPvpAiObserverEventV1 active =
            Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
        const SdgoPvpAiObserverEventV1 ended =
            Event(SDGO_PVP_AI_EVENT_BATTLE_END);

        ProcessEvent(&lifecycle, request);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 1);
        CHECK(SameBattle(lifecycle, setup));

        ProcessEvent(&lifecycle, setup);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 2);

        ProcessEvent(&lifecycle, active);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 3);

        SdgoPvpAiObserverEventV1 unrelated =
            Event(SDGO_PVP_AI_EVENT_SETUP_FAILED, 0x55667788u);
        ProcessEvent(&lifecycle, unrelated);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 3);

        ProcessEvent(&lifecycle, ended);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
        CHECK(lifecycle.eventCount == 5);
    }


    void TestZeroAiMissionLifecycle()
    {
        ResetConfig("T7x", 9229, 5, 0, true);
        Lifecycle lifecycle;
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 0x10203040u, 9229, 5));
        SdgoPvpAiObserverEventV1 setup =
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 0x10203040u, 9229, 5);
        setup.ai_team_1 = 0;
        setup.ai_team_2 = 0;
        setup.active_slots = 1;
        ProcessEvent(&lifecycle, setup);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 2);
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE, 0x10203040u, 9229, 5));
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 3);
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_END, 0x10203040u, 9229, 5));
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
    }

    void TestRequireEndLifecycle()
    {
        ResetConfig("T7x", 9229, 3, 1, true);
        Lifecycle lifecycle;
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 3);

        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
    }

    void TestBattleEndUsesCapturedIdentityWithoutRelaxingFilters()
    {
        ResetConfig("T7x", 9229, 3, 1, true);
        ResetQueue();

        Lifecycle lifecycle;
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 3);

        SdgoPvpAiObserverEventV1 afterNativeEnd =
            Event(SDGO_PVP_AI_EVENT_BATTLE_END);
        afterNativeEnd.host_pilot_id = 0;
        afterNativeEnd.mode = 0;
        afterNativeEnd.title[0] = '\0';
        ObservePvpAi(&afterNativeEnd, nullptr);
        CHECK(g_queueCount == 0);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 3);

        const SdgoPvpAiObserverEventV1 capturedBeforeNativeEnd =
            Event(SDGO_PVP_AI_EVENT_BATTLE_END);
        ObservePvpAi(&capturedBeforeNativeEnd, nullptr);
        CHECK(g_queueCount == 1);
        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].host_pilot_id == lifecycle.hostPilotId);
        CHECK(output[0].room_cookie == lifecycle.roomCookie);
        CHECK(output[0].mode == lifecycle.mode);
        CHECK(TitleContainsNonce(output[0].title));
        ProcessEvent(&lifecycle, output[0]);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
    }

    void TestLifecycleFailures()
    {
        ResetConfig("T7x", 9229, 3, 1, false);
        Lifecycle missingRequest;
        ProcessEvent(
            &missingRequest, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(missingRequest.outcome == Lifecycle::Fail);
        CHECK(missingRequest.phase == 0);

        Lifecycle setupFailed;
        SdgoPvpAiObserverEventV1 failure =
            Event(SDGO_PVP_AI_EVENT_SETUP_FAILED);
        failure.status = 0xc0000005u;
        ProcessEvent(&setupFailed, failure);
        CHECK(setupFailed.outcome == Lifecycle::Fail);
        CHECK(setupFailed.reason ==
              "PvP AI setup raised a native exception");

        ResetConfig("T7x", 9229, 3, 3, false);
        Lifecycle tooFewAi;
        ProcessEvent(
            &tooFewAi, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        SdgoPvpAiObserverEventV1 setup =
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE);
        setup.ai_team_1 = 1;
        setup.ai_team_2 = 1;
        ProcessEvent(&tooFewAi, setup);
        CHECK(tooFewAi.outcome == Lifecycle::Fail);
        CHECK(tooFewAi.phase == 1);

        ResetConfig();
        Lifecycle wrongRoom;
        ProcessEvent(
            &wrongRoom, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1));
        ProcessEvent(
            &wrongRoom, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 2));
        CHECK(wrongRoom.outcome == Lifecycle::Fail);

        Lifecycle wrongHost;
        ProcessEvent(
            &wrongHost,
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1, 9229));
        ProcessEvent(
            &wrongHost,
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 1, 9230));
        CHECK(wrongHost.outcome == Lifecycle::Fail);

        Lifecycle wrongMode;
        ProcessEvent(
            &wrongMode,
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1, 9229, 3));
        ProcessEvent(
            &wrongMode,
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 1, 9229, 4));
        CHECK(wrongMode.outcome == Lifecycle::Fail);

        Lifecycle prematureEnd;
        ProcessEvent(
            &prematureEnd, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        ProcessEvent(
            &prematureEnd, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(prematureEnd.outcome == Lifecycle::Fail);
    }

    void TestLifecycleDuplicatesAndTerminalOutcomes()
    {
        ResetConfig("T7x", 9229, 3, 1, false);

        Lifecycle repeatedRequest;
        const SdgoPvpAiObserverEventV1 request =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);
        ProcessEvent(&repeatedRequest, request);
        ProcessEvent(&repeatedRequest, request);
        CHECK(repeatedRequest.outcome == Lifecycle::Running);
        CHECK(repeatedRequest.phase == 1);
        CHECK(repeatedRequest.eventCount == 2);

        ProcessEvent(
            &repeatedRequest, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(repeatedRequest.outcome == Lifecycle::Fail);
        CHECK(repeatedRequest.phase == 1);
        const std::string failureReason = repeatedRequest.reason;
        ProcessEvent(
            &repeatedRequest, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(repeatedRequest.outcome == Lifecycle::Fail);
        CHECK(repeatedRequest.phase == 1);
        CHECK(repeatedRequest.reason == failureReason);
        CHECK(repeatedRequest.eventCount == 4);

        Lifecycle duplicateSetup;
        ProcessEvent(&duplicateSetup, request);
        ProcessEvent(
            &duplicateSetup, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &duplicateSetup, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(duplicateSetup.outcome == Lifecycle::Fail);
        CHECK(duplicateSetup.phase == 2);

        Lifecycle terminalPass;
        ProcessEvent(&terminalPass, request);
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 3);
        const std::string passReason = terminalPass.reason;

        ProcessEvent(&terminalPass, request);
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_FAILED));
        ProcessEvent(
            &terminalPass,
            Event(SDGO_PVP_AI_EVENT_BATTLE_END, 0x55667788u));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 3);
        CHECK(terminalPass.reason == passReason);

        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 4);
        const std::string completedReason = terminalPass.reason;

        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_FAILED));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 4);
        CHECK(terminalPass.reason == completedReason);
        CHECK(terminalPass.eventCount == 11);
    }

    void TestQueueCapacityAndOrder()
    {
        ResetConfig();
        ResetQueue();
        for (unsigned i = 0; i < kQueueCapacity; ++i) {
            SdgoPvpAiObserverEventV1 event =
                Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
            event.sequence = i + 1;
            ObservePvpAi(&event, nullptr);
        }
        CHECK(g_queueCount == kQueueCapacity);
        CHECK(InterlockedCompareExchange(&g_dropped, 0, 0) == 0);

        SdgoPvpAiObserverEventV1 overflow =
            Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
        overflow.sequence = kQueueCapacity + 1;
        ObservePvpAi(&overflow, nullptr);
        CHECK(g_queueCount == kQueueCapacity);
        CHECK(InterlockedCompareExchange(&g_dropped, 0, 0) == 1);

        uint64_t expectedSequence = 1;
        unsigned drained = 0;
        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        for (;;) {
            const unsigned count = Drain(output);
            if (!count)
                break;
            CHECK(count <= kDrainBatch);
            for (unsigned i = 0; i < count; ++i) {
                CHECK(output[i].sequence == expectedSequence);
                ++expectedSequence;
            }
            drained += count;
        }
        CHECK(drained == kQueueCapacity);
        CHECK(expectedSequence == kQueueCapacity + 1);
        CHECK(g_queueCount == 0);
    }

    template <typename T>
    void Put(void* base, size_t offset, T value)
    {
        *reinterpret_cast<T*>(
            static_cast<uint8_t*>(base) + offset) = value;
    }

    void TestNativeDamageIdentityReceipts()
    {
        ResetConfig("T7x", 9229, 3);
        ResetQueue();
        g_deathEnabled = true;
        std::vector<uint8_t> room(0x20000);
        std::vector<uint8_t> unit(0x100);
        room[kRoomMode] = 3;
        room[kRoomTitleLength] = 10;
        memcpy(room.data() + kRoomTitle, "!ai e2 T7x", 10);
        Put(room.data(), kRoomHost, uint32_t{9229});

        const int attackerSlot = 1;
        const int victimSlot = 6;
        uint8_t* attacker =
            room.data() + kBattlePilots +
            attackerSlot * kBattlePilotStride;
        uint8_t* victim =
            room.data() + kBattlePilots +
            victimSlot * kBattlePilotStride;
        const uint32_t attackerId = 401;
        const uint32_t victimId = 0x80000123u;
        Put(attacker, kPilotEntity, attackerId);
        Put(attacker, kPilotTeam, uint8_t{1});
        Put(victim, kPilotEntity, victimId);
        Put(victim, kPilotTeam, uint8_t{2});
        Put(
            room.data(),
            kSlotInfo + attackerSlot * kSlotInfoStride + kSlotPilotId,
            attackerId);
        Put(
            room.data(),
            kSlotInfo + victimSlot * kSlotInfoStride + kSlotPilotId,
            victimId);
        Put(unit.data(), kUnitKey, uint32_t{7321});
        Put(unit.data(), kUnitMaxHp, uint32_t{14000});
        Put(unit.data(), kUnitHp, uint32_t{0});
        Put(unit.data(), kUnitData, uintptr_t{0x12345678u});

        DamageEvent event = {};
        CHECK(CaptureDamage(
            room.data(), victim, attacker, unit.data(), 875, 1, &event));
        CHECK(event.attackerSlot == attackerSlot);
        CHECK(event.victimSlot == victimSlot);
        CHECK(event.attackerPilotId == attackerId);
        CHECK(event.victimPilotId == victimId);
        CHECK(event.attackerSlotPilotId == attackerId);
        CHECK(event.victimSlotPilotId == victimId);
        CHECK(event.attackerTeam == 1);
        CHECK(event.victimTeam == 2);
        CHECK(event.attackerIsAi == 0);
        CHECK(event.victimIsAi == 1);
        CHECK(event.identityValid == 1);
        CHECK(event.samePilot == 0);
        CHECK(event.sameSlot == 0);
        CHECK(event.friendly == 0);
        CHECK(event.effectiveLoss == 875);
        CHECK(event.deathFlag == 1);
        CHECK(event.victimHp == 0);
        CHECK(event.victimMaxHp == 14000);
        CHECK(event.victimUnitKey == 7321);
        CHECK(event.victimUnitData == 0x12345678u);

        QueueDamage(event);
        CHECK(g_damageQueueCount == 1);
        DamageEvent output[kDamageDrainBatch] = {};
        CHECK(DrainDamage(output) == 1);
        CHECK(output[0].sequence == 1);
        const std::string json = DamageJson(output[0]);
        CHECK(json.find("\"event\":\"native_effective_hp_loss\"") !=
              std::string::npos);
        CHECK(json.find("\"victim_pilot_id\":2147483939") !=
              std::string::npos);
        CHECK(json.find("\"death_flag\":1") != std::string::npos);
        CHECK(json.find("\"same_pilot\":false") != std::string::npos);

        const SdgoBattleRewardsDamageEventV1 bridged = {
            sizeof(bridged),
            SDGO_BATTLE_REWARDS_DAMAGE_OBSERVER_VERSION,
            room.data(),
            victim,
            attacker,
            unit.data(),
            425,
            0};
        ObserveBattleRewardsDamage(&bridged, nullptr);
        CHECK(DrainDamage(output) == 1);
        CHECK(output[0].effectiveLoss == 425);
        CHECK(output[0].deathFlag == 0);
        CHECK(output[0].attackerPilotId == attackerId);
        CHECK(output[0].victimPilotId == victimId);

        DamageEvent self = {};
        CHECK(CaptureDamage(
            room.data(), victim, victim, unit.data(), 200, 1, &self));
        CHECK(self.samePilot == 1);
        CHECK(self.sameSlot == 1);
        CHECK(self.friendly == 1);
        CHECK(self.identityValid == 1);

        room[kRoomTitleLength] = 5;
        memcpy(room.data() + kRoomTitle, "other", 5);
        DamageEvent unrelated = {};
        CHECK(!CaptureDamage(
            room.data(), victim, attacker, unit.data(), 100, 0,
            &unrelated));
        g_deathEnabled = false;
    }

    void TestMissionNpcDamagesHuman()
    {
        ResetConfig("T7x", 9229, 5, 0, true);
        g_config.missionGameModeId = 88072;
        g_deathEnabled = true;
        g_titleApi = &kRoomTitleApi;
        g_roomTitle = "T7x lv2";

        std::vector<uint8_t> room(0x40000);
        std::vector<uint8_t> npcData(0x100);
        std::vector<uint8_t> unit(0x100);
        room[kRoomMode] = 5;
        Put(room.data(), kRoomHost, uint32_t{9229});
        Put(room.data(), kRoomMissionId, uint32_t{88072});

        const int attackerSlot = 12;
        const int victimSlot = 0;
        uint8_t* attacker = room.data() + kBattlePilots +
            attackerSlot * kBattlePilotStride;
        uint8_t* victim = room.data() + kBattlePilots +
            victimSlot * kBattlePilotStride;
        const uint32_t attackerId = 0x80000021u;
        const uint32_t victimId = 9229;
        Put(attacker, kPilotEntity, attackerId);
        Put(attacker, kPilotTeam, uint8_t{2});
        Put(attacker, kBattlePilotNpcData, npcData.data());
        Put(npcData.data(), kNpcDataId, uint32_t{97621});
        Put(victim, kPilotEntity, victimId);
        Put(victim, kPilotTeam, uint8_t{1});
        Put(
            room.data(),
            kSlotInfo + victimSlot * kSlotInfoStride + kSlotPilotId,
            victimId);
        Put(unit.data(), kUnitMaxHp, uint32_t{14500});
        Put(unit.data(), kUnitHp, uint32_t{0});

        DamageEvent event = {};
        CHECK(CaptureDamage(
            room.data(), victim, attacker, unit.data(), 875, 1, &event));
        CHECK(event.missionId == 88072);
        CHECK(event.attackerSlot == 12);
        CHECK(event.victimSlot == 0);
        CHECK(event.attackerNativeNpc == 1);
        CHECK(event.attackerIsAi == 1);
        CHECK(event.attackerNpcDataId == 97621);
        CHECK(event.victimPilotId == 9229);
        CHECK(event.victimIsAi == 0);
        CHECK(event.identityValid == 1);
        CHECK(event.effectiveLoss == 875);
        CHECK(event.deathFlag == 1);

        Lifecycle lifecycle;
        ProcessMissionDamage(&lifecycle, event);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.roomCookie == event.roomCookie);
        CHECK(lifecycle.mode == 5);
        CHECK(lifecycle.phase == 4);
        CHECK(lifecycle.eventCount == 1);

        const std::string json = DamageJson(event);
        CHECK(json.find("\"mission_game_mode_id\":88072") !=
              std::string::npos);
        CHECK(json.find("\"attacker_native_npc\":true") !=
              std::string::npos);
        CHECK(json.find("\"attacker_npc_data_id\":97621") !=
              std::string::npos);
        CHECK(json.find("\"victim_is_ai\":false") !=
              std::string::npos);

        g_roomTitle = "other";
        CHECK(!CaptureDamage(
            room.data(), victim, attacker, unit.data(), 1, 0, &event));
        g_titleApi = nullptr;
        g_deathEnabled = false;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 &&
        strcmp(argv[1], "--emit-ready-json") == 0) {
        return EmitObserverReadyJson(argv[2]) ? 0 : 2;
    }
    std::puts(
        "OFFLINE ONLY: no BattleServer/client/authentication/room/gameplay is exercised.");
    InitializeCriticalSection(&g_queueLock);

    TestAbiAndValidation();
    TestObserverFiltering();
    TestObserverWildcardAndDisabledHost();
    TestTitleBoundary();
    TestNormalLifecycle();
    TestZeroAiMissionLifecycle();
    TestRequireEndLifecycle();
    TestBattleEndUsesCapturedIdentityWithoutRelaxingFilters();
    TestLifecycleFailures();
    TestLifecycleDuplicatesAndTerminalOutcomes();
    TestQueueCapacityAndOrder();
    TestNativeDamageIdentityReceipts();
    TestMissionNpcDamagesHuman();

    DeleteCriticalSection(&g_queueLock);
    std::printf("%u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
