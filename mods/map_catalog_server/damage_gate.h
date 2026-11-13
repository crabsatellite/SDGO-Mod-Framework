#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "sdgo/map_boundary_contact.h"
#include "sdgo/mission_damage_gate_format.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::map_catalog_server {

struct DamageGateContext {
    void* room = nullptr;
    uint32_t start = 0;
    uint32_t mission = 0;
    uint32_t phase = 0;
};

class DamageGateTracker {
    static constexpr size_t kSessions = 256;
    static constexpr size_t kGuardKinds = 256;
    static constexpr size_t kSeenEntities = 256;
    static constexpr size_t kDefeatedEntities = 256;

    struct GuardCount {
        uint32_t npc = 0;
        uint32_t count = 0;
    };
    struct SeenEntity {
        uint32_t npc = 0;
        uint32_t entity = 0;
    };
    struct Session {
        DamageGateContext context{};
        uint64_t touched = 0;
        std::array<GuardCount, kGuardKinds> guards{};
        std::array<SeenEntity, kSeenEntities> seen{};
        std::array<uint32_t, kDefeatedEntities> defeated{};
    };

    const std::vector<mission_damage_gate::Entry>* rules_ = nullptr;
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::unique_ptr<std::array<Session, kSessions>> sessions_;

    static bool Same(const DamageGateContext& left, const DamageGateContext& right) noexcept {
        return left.room == right.room && left.start == right.start &&
               left.mission == right.mission && left.phase == right.phase;
    }

    Session* FindSession(const DamageGateContext& context, bool create) noexcept {
        Session* empty = nullptr;
        Session* oldest = nullptr;
        for (auto& session : *sessions_) {
            if (session.context.room && Same(session.context, context)) return &session;
            if (!create) continue;
            if (!session.context.room) {
                if (!empty) empty = &session;
            } else if (!oldest || session.touched < oldest->touched) {
                oldest = &session;
            }
        }
        Session* selected = empty ? empty : oldest;
        if (selected) {
            *selected = {};
            selected->context = context;
        }
        return selected;
    }

    static uint32_t Count(const Session& session, uint32_t npc) noexcept {
        for (const auto& guard : session.guards) {
            if (guard.npc == npc) return guard.count;
        }
        return 0;
    }

    static bool Seen(const Session& session, uint32_t npc, uint32_t entity) noexcept {
        return std::find_if(session.seen.begin(), session.seen.end(),
            [npc, entity](const auto& known) {
                return known.npc == npc && known.entity == entity;
            }) != session.seen.end();
    }

    static bool RememberSeen(Session& session, uint32_t npc, uint32_t entity) noexcept {
        if (Seen(session, npc, entity)) return true;
        for (auto& known : session.seen) {
            if (!known.entity) {
                known = SeenEntity{npc, entity};
                return true;
            }
        }
        return false;
    }

    static bool DuplicateDefeat(const Session& session, uint32_t entity) noexcept {
        return std::find(session.defeated.begin(), session.defeated.end(), entity) !=
               session.defeated.end();
    }

    static bool RememberDefeat(Session& session, uint32_t entity) noexcept {
        for (auto& known : session.defeated) {
            if (!known) {
                known = entity;
                return true;
            }
        }
        return false;
    }

    static bool Increment(Session& session, uint32_t npc) noexcept {
        GuardCount* empty = nullptr;
        for (auto& guard : session.guards) {
            if (guard.npc == npc) {
                if (guard.count != UINT32_MAX) ++guard.count;
                return true;
            }
            if (!guard.npc && !empty) empty = &guard;
        }
        if (!empty) return false;
        empty->npc = npc;
        empty->count = 1;
        return true;
    }

    auto PhaseBegin(uint32_t mission, uint32_t phase) const {
        const mission_damage_gate::Entry key{mission, phase, 0, 0, 0};
        return std::lower_bound(rules_->begin(), rules_->end(), key,
            [](const auto& left, const auto& right) {
                return mission_damage_gate::Key(left) < mission_damage_gate::Key(right);
            });
    }

    bool RelevantGuard(uint32_t mission, uint32_t phase, uint32_t guard) const noexcept {
        for (auto current = PhaseBegin(mission, phase);
             current != rules_->end() && current->missionId == mission &&
             current->phase == phase; ++current) {
            if (current->guardNpcId == guard) return true;
        }
        return false;
    }

public:
    DamageGateTracker()
        : sessions_(std::make_unique<std::array<Session, kSessions>>()) {}

    void Bind(const std::vector<mission_damage_gate::Entry>* rules) noexcept {
        AcquireSRWLockExclusive(&lock_);
        rules_ = rules;
        std::memset(sessions_->data(), 0, sizeof(*sessions_));
        ReleaseSRWLockExclusive(&lock_);
    }

    bool Blocks(const DamageGateContext& context, uint32_t target) noexcept {
        if (!rules_ || rules_->empty() || !context.room || !context.mission ||
            !context.phase || !target) return false;
        const auto key = mission_damage_gate::Entry{
            context.mission, context.phase, target, 0, 0};
        auto first = std::lower_bound(rules_->begin(), rules_->end(), key,
            [](const auto& left, const auto& right) {
                return mission_damage_gate::Key(left) < mission_damage_gate::Key(right);
            });
        if (first == rules_->end() || first->missionId != context.mission ||
            first->phase != context.phase || first->targetNpcId != target) return false;
        auto last = first;
        while (last != rules_->end() && last->missionId == context.mission &&
               last->phase == context.phase && last->targetNpcId == target) ++last;

        AcquireSRWLockExclusive(&lock_);
        Session* session = FindSession(context, false);
        bool blocked = session == nullptr;
        if (session) {
            session->touched = GetTickCount64();
            for (auto current = first; current != last; ++current) {
                if (Count(*session, current->guardNpcId) < current->requiredDefeats) {
                    blocked = true;
                    break;
                }
            }
        }
        ReleaseSRWLockExclusive(&lock_);
        return blocked;
    }

    bool Observe(const DamageGateContext& context, uint32_t guard,
                 uint32_t entity) noexcept {
        if (!rules_ || rules_->empty() || !context.room || !context.mission ||
            !context.phase || !guard || !entity ||
            !RelevantGuard(context.mission, context.phase, guard)) return false;
        AcquireSRWLockExclusive(&lock_);
        Session* session = FindSession(context, true);
        const bool observed = session && RememberSeen(*session, guard, entity);
        if (observed) session->touched = GetTickCount64();
        ReleaseSRWLockExclusive(&lock_);
        return observed;
    }

    bool RecordDefeat(const DamageGateContext& context, uint32_t guard,
                      uint32_t entity) noexcept {
        if (!rules_ || rules_->empty() || !context.room || !context.mission ||
            !context.phase || !guard || !entity ||
            !RelevantGuard(context.mission, context.phase, guard)) return false;

        AcquireSRWLockExclusive(&lock_);
        Session* session = FindSession(context, false);
        bool recorded = false;
        if (session && Seen(*session, guard, entity) &&
            !DuplicateDefeat(*session, entity) && RememberDefeat(*session, entity)) {
            recorded = Increment(*session, guard);
            session->touched = GetTickCount64();
        }
        ReleaseSRWLockExclusive(&lock_);
        return recorded;
    }
};

class DamageGate;
inline DamageGate* g_damageGateActive = nullptr;
inline void* g_damageGateBodyOriginal = nullptr;
inline void* g_damageGatePartOriginal = nullptr;
inline void* g_damageGateLossOriginal = nullptr;
inline const DWORD g_damageGateDefaultMxcsr = 0x1f80;

void __stdcall DamageGateApplyBody(void* frame) noexcept;
void __stdcall DamageGateApplyPart(void* frame) noexcept;
void __stdcall DamageGateApplyLoss(void* frame) noexcept;
void DamageGateBodySite();
void DamageGatePartSite();
void DamageGateLossSite();

class DamageGate {
    using Broadcast = int(__thiscall*)(uint8_t*, uint8_t*, uint32_t, uint32_t*);
    struct NoticeCooldown {
        void* room = nullptr;
        uint32_t start = 0, mission = 0, phase = 0, target = 0, attacker = 0;
        uint64_t at = 0, until = 0;
    };

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    std::vector<mission_damage_gate::Entry> rules_;
    DamageGateTracker tracker_;
    Broadcast broadcast_ = nullptr;
    SRWLOCK noticeLock_ = SRWLOCK_INIT;
    std::array<NoticeCooldown, 1024> noticeCooldowns_{};
    volatile LONG blocked_ = 0;
    volatile LONG defeats_ = 0;
    bool enabled_ = false;

    template<class T>
    static bool Read(const void* base, ptrdiff_t offset, T& value) noexcept {
        if (!base) return false;
        __try {
            std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template<class T>
    static bool Write(void* base, ptrdiff_t offset, const T& value) noexcept {
        if (!base) return false;
        __try {
            std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool MissionMode(uint8_t mode) noexcept {
        return mode == 5 || mode == 8 || mode == 10;
    }

    static bool Context(void* room, DamageGateContext& context) noexcept {
        uint32_t running = 0, state = 0;
        uint8_t mode = 0, phase = 0;
        DamageGateContext result{};
        result.room = room;
        if (!Read(room, 0x21, running) || !running || !Read(room, 5, mode) ||
            !MissionMode(mode) || !Read(room, 0x316A4, state) || state != 2 ||
            !Read(room, 0x1B, result.mission) || !result.mission ||
            !Read(room, 0x316F0, phase) || !phase || phase > 8 ||
            !Read(room, 0x2EEB4, result.start)) return false;
        result.phase = phase;
        context = result;
        return true;
    }

    static bool PilotIdentity(void* room, void* pilot, uint32_t& npc,
                              uint32_t& entity) noexcept {
        if (!room || !pilot) return false;
        const auto first = reinterpret_cast<uintptr_t>(room) + 0x3CE4;
        const auto candidate = reinterpret_cast<uintptr_t>(pilot);
        if (candidate < first) return false;
        const auto distance = candidate - first;
        if (distance % 0x1F54 != 0) return false;
        const auto slot = distance / 0x1F54;
        if (slot < 12 || slot >= 22 || !Read(pilot, 0, entity) ||
            !(entity & 0x80000000u)) return false;
        void* data = nullptr;
        if (!Read(pilot, 0x1CEC, data) || !data || !Read(data, 0, npc) || !npc) return false;
        return true;
    }

    static bool SelectedHealth(void* pilot, uint32_t& health) noexcept {
        uint8_t form = 0;
        if (!pilot || !Read(pilot, 0x33C, form) || form >= 4) return false;
        auto* selected = static_cast<uint8_t*>(pilot) + 0x340 + form * 0x658;
        return Read(selected, 0x34, health);
    }

    static bool NativeBodyDead(void* pilot, void* unit) noexcept {
        uint8_t form = 0;
        if (!pilot || !unit || !Read(pilot, 0x33C, form) || form >= 4) return false;
        auto* selected = static_cast<uint8_t*>(pilot) + 0x340 + form * 0x658;
        uint32_t health = 0;
        return unit == selected && Read(selected, 0x34, health) && health == 0;
    }

    static bool AttackerIdentity(void* room, void* frame, bool part,
                                 uint32_t& attacker) noexcept {
        uint32_t slot = 0;
        void* pilot = nullptr;
        if (!Read(frame, part ? 0x08 : 0x0C, slot) || slot >= 22 ||
            !Read(frame, part ? 0x0C : 0x10, pilot) ||
            pilot != static_cast<uint8_t*>(room) + 0x3CE4 + slot * 0x1F54 ||
            !Read(pilot, 0, attacker) || !attacker) return false;
        return true;
    }

    bool SpendNoticeCooldown(const DamageGateContext& context, uint32_t target,
                             uint32_t attacker) noexcept {
        const uint64_t now = GetTickCount64();
        AcquireSRWLockExclusive(&noticeLock_);
        NoticeCooldown* selected = nullptr;
        for (auto& entry : noticeCooldowns_) {
            if (entry.room == context.room && entry.start == context.start &&
                entry.mission == context.mission && entry.phase == context.phase &&
                entry.target == target && entry.attacker == attacker) {
                if (now < entry.until) {
                    ReleaseSRWLockExclusive(&noticeLock_);
                    return false;
                }
                selected = &entry;
                break;
            }
            if ((!entry.at || now >= entry.until) &&
                (!selected || entry.at < selected->at)) selected = &entry;
        }
        if (selected) {
            *selected = {context.room, context.start, context.mission, context.phase,
                         target, attacker, now, now + 3000};
        }
        ReleaseSRWLockExclusive(&noticeLock_);
        return selected != nullptr;
    }

    void NotifyBlocked(const DamageGateContext& context, void* targetPilot,
                       uint32_t target, void* frame, bool part) noexcept {
        if (!broadcast_ || !target || (target & map_boundary_contact::kMarker)) return;
        uint32_t attacker = 0, health = 0;
        if (!AttackerIdentity(context.room, frame, part, attacker) ||
            !SelectedHealth(targetPilot, health) || !health ||
            health > map_boundary_contact::kMaximumHealth) return;
        const uint32_t marked = target | map_boundary_contact::kMarker;
        for (uint32_t slot = 0; slot < 22; ++slot) {
            uint32_t existing = 0;
            auto* pilot = static_cast<uint8_t*>(context.room) + 0x3CE4 + slot * 0x1F54;
            if (!Read(pilot, 0, existing) || existing == marked) return;
        }
        if (!SpendNoticeCooldown(context, target, attacker)) return;
        std::array<uint8_t, 13> notice{};
        const uint16_t opcode = 0x0793;
        std::memcpy(notice.data(), &opcode, sizeof(opcode));
        std::memcpy(notice.data() + 2, &attacker, sizeof(attacker));
        std::memcpy(notice.data() + 6, &marked, sizeof(marked));
        if (!map_boundary_contact::EncodeHealth(health, notice.data() + 10)) return;
        uint32_t recipients = 0;
        const int sent = broadcast_(static_cast<uint8_t*>(context.room), notice.data(),
                                    static_cast<uint32_t>(notice.size()), &recipients);
        if (api_ && api_->Log && (!sent || !recipients)) {
            api_->Log(mod_, SDGO_LOG_WARN,
                "mission damage gate notice failed: mission=%u phase=%u target=%u attacker=%u recipients=%u sent=%d",
                context.mission, context.phase, target, attacker, recipients, sent);
        }
    }

    static bool LoadFile(const std::wstring& path, std::vector<uint8_t>& data,
                         bool& present) {
        present = false;
        FILE* stream = nullptr;
        const auto opened = _wfopen_s(&stream, path.c_str(), L"rb");
        if (opened == ENOENT) return true;
        if (opened || !stream) return false;
        present = true;
        std::array<uint8_t, 4096> buffer{};
        while (const auto count = std::fread(buffer.data(), 1, buffer.size(), stream)) {
            data.insert(data.end(), buffer.begin(), buffer.begin() + count);
            if (data.size() > mission_damage_gate::kHeaderBytes +
                                  mission_damage_gate::kMaxRows * mission_damage_gate::kRowBytes) break;
        }
        const bool okay = std::ferror(stream) == 0;
        std::fclose(stream);
        return okay;
    }

public:
    bool Enabled() const noexcept { return enabled_; }
    uint32_t Blocked() const noexcept {
        return static_cast<uint32_t>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&blocked_), 0, 0));
    }
    uint32_t Defeats() const noexcept {
        return static_cast<uint32_t>(InterlockedCompareExchange(
            const_cast<volatile LONG*>(&defeats_), 0, 0));
    }

    void Configure(std::vector<mission_damage_gate::Entry> rules) {
        rules_ = std::move(rules);
        tracker_.Bind(&rules_);
        enabled_ = !rules_.empty();
    }

    void HandleHitFrame(void* frame, bool part) noexcept {
        const DWORD incoming = GetLastError();
        void* room = nullptr;
        uint32_t slot = 0;
        const ptrdiff_t roomOffset = part ? -0x14 : -0x480;
        const ptrdiff_t slotOffset = part ? 0x30 : 0x34;
        if (Read(frame, roomOffset, room) && Read(frame, slotOffset, slot) &&
            slot >= 12 && slot < 22) {
            auto* pilot = static_cast<uint8_t*>(room) + 0x3CE4 + slot * 0x1F54;
            uint32_t npc = 0, entity = 0;
            DamageGateContext context{};
            if (Context(room, context) && PilotIdentity(room, pilot, npc, entity)) {
                tracker_.Observe(context, npc, entity);
            }
            if (context.room && tracker_.Blocks(context, npc)) {
                const uint32_t zero = 0;
                const ptrdiff_t damageOffset = part ? 0x1C : 0x20;
                const ptrdiff_t forceOffset = part ? 0x24 : 0x2C;
                if (Write(frame, damageOffset, zero) && Write(frame, forceOffset, zero)) {
                    InterlockedIncrement(&blocked_);
                    NotifyBlocked(context, pilot, entity, frame, part);
                }
            }
        }
        SetLastError(incoming);
    }

    void HandleLossFrame(void* frame) noexcept {
        const DWORD incoming = GetLastError();
        void* room = nullptr;
        void* pilot = nullptr;
        void* unit = nullptr;
        uint32_t loss = 0;
        int32_t death = 0;
        if (Read(frame, -0x34, room) && Read(frame, 0x08, pilot) &&
            Read(frame, 0x10, unit) && NativeBodyDead(pilot, unit) &&
            Read(frame, 0x14, loss) && loss && Read(frame, 0x18, death) && death == 1) {
            uint32_t npc = 0, entity = 0;
            DamageGateContext context{};
            if (Context(room, context) && PilotIdentity(room, pilot, npc, entity) &&
                tracker_.RecordDefeat(context, npc, entity)) {
                InterlockedIncrement(&defeats_);
                if (api_ && api_->Log) {
                    api_->Log(mod_, SDGO_LOG_INFO,
                        "mission damage guard defeated: mission=%u phase=%u npc=%u entity=%u",
                        context.mission, context.phase, npc, entity);
                }
            }
        }
        SetLastError(incoming);
    }

    bool Install(const SdgoApi* api, SdgoMod* mod) {
        api_ = api;
        mod_ = mod;
        std::wstring path = api->ModDirectory(mod);
        path += L"\\damage-gates.bin";
        std::vector<uint8_t> data;
        bool present = false;
        if (!LoadFile(path, data, present)) {
            api->Log(mod, SDGO_LOG_ERROR, "damage gate declaration could not be read");
            return false;
        }
        if (!present) return true;
        std::vector<mission_damage_gate::Entry> parsed;
        std::string error;
        if (!mission_damage_gate::Parse(data.data(), data.size(), parsed, error)) {
            api->Log(mod, SDGO_LOG_ERROR, "invalid damage gate declaration: %s", error.c_str());
            return false;
        }
        Configure(std::move(parsed));
        if (!enabled_) return true;
        if (!api->Symbol ||
            !(broadcast_ = reinterpret_cast<Broadcast>(
                api->Symbol(mod, "battle.room_random_broadcast")))) {
            api->Log(mod, SDGO_LOG_ERROR, "mission damage gate broadcast symbol unavailable");
            return false;
        }
        g_damageGateActive = this;
        if (!api->HookSite(mod, "map_catalog_server.damage_gate_body",
                           reinterpret_cast<void*>(&DamageGateBodySite), &g_damageGateBodyOriginal) ||
            !api->HookSite(mod, "map_catalog_server.damage_gate_part",
                           reinterpret_cast<void*>(&DamageGatePartSite), &g_damageGatePartOriginal) ||
            !api->HookSite(mod, "map_catalog_server.damage_gate_loss",
                           reinterpret_cast<void*>(&DamageGateLossSite), &g_damageGateLossOriginal)) return false;
        api->Log(mod, SDGO_LOG_INFO, "mission damage gates active: %u guard requirements",
                 static_cast<unsigned>(rules_.size()));
        return true;
    }

};

inline void __stdcall DamageGateApplyBody(void* frame) noexcept {
    if (g_damageGateActive) g_damageGateActive->HandleHitFrame(frame, false);
}
inline void __stdcall DamageGateApplyPart(void* frame) noexcept {
    if (g_damageGateActive) g_damageGateActive->HandleHitFrame(frame, true);
}
inline void __stdcall DamageGateApplyLoss(void* frame) noexcept {
    if (g_damageGateActive) g_damageGateActive->HandleLossFrame(frame);
}

__declspec(naked) inline void DamageGateBodySite() {
    __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 210h
        and esp, 0fffffff0h
        mov [esp + 200h], eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_damageGateDefaultMxcsr]
        cld
        mov ecx, [esp + 200h]
        mov eax, [ecx + 08h]
        push eax
        call DamageGateApplyBody
        fxrstor [esp]
        mov esp, [esp + 200h]
        popad
        popfd
        jmp dword ptr [g_damageGateBodyOriginal]
    }
}

__declspec(naked) inline void DamageGatePartSite() {
    __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 210h
        and esp, 0fffffff0h
        mov [esp + 200h], eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_damageGateDefaultMxcsr]
        cld
        mov ecx, [esp + 200h]
        mov eax, [ecx + 08h]
        push eax
        call DamageGateApplyPart
        fxrstor [esp]
        mov esp, [esp + 200h]
        popad
        popfd
        jmp dword ptr [g_damageGatePartOriginal]
    }
}

__declspec(naked) inline void DamageGateLossSite() {
    __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 210h
        and esp, 0fffffff0h
        mov [esp + 200h], eax
        fxsave [esp]
        fninit
        ldmxcsr dword ptr [g_damageGateDefaultMxcsr]
        cld
        mov ecx, [esp + 200h]
        mov eax, [ecx + 08h]
        push eax
        call DamageGateApplyLoss
        fxrstor [esp]
        mov esp, [esp + 200h]
        popad
        popfd
        jmp dword ptr [g_damageGateLossOriginal]
    }
}

}  // namespace sdgo::map_catalog_server
