#pragma once
#include <windows.h>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/map_boundary_format.h"
#include "sdgo/map_boundary_contact.h"
#include "sdgo/sdgo_api.h"

namespace sdgo::map_catalog_server {

class BoundaryContact {
    using Handler = void(__thiscall*)(void*, uint32_t, const uint8_t*);
    using FindPilot = int(__thiscall*)(void*, const uint32_t*, uint32_t*);
    using LockRoom = uint8_t*(__thiscall*)(void*, uint32_t, uint16_t);
    using UnlockRoom = void(__thiscall*)(void*, uint32_t);
    using Broadcast = int(__thiscall*)(uint8_t*, uint8_t*, uint32_t, uint32_t*);
    inline static BoundaryContact* active_ = nullptr;
    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    Handler original_ = nullptr;
    FindPilot find_ = nullptr;
    LockRoom lock_ = nullptr;
    UnlockRoom unlock_ = nullptr;
    Broadcast broadcast_ = nullptr;
    void* proc_ = nullptr;
    std::vector<map_boundary::Entry> rules_;
    bool enabled_ = false;
    SRWLOCK cooldownLock_ = SRWLOCK_INIT;
    struct Cooldown {
        uint8_t* room = nullptr;
        uint32_t start = 0, mission = 0, phase = 0, target = 0;
        uint64_t at = 0, until = 0;
    };
    std::array<Cooldown, 16384> cooldowns_{};

    template<class T> static bool Read(const void* base, size_t offset, T& value) noexcept {
        if (!base) return false;
        __try { std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(T)); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    static bool WriteHealth(uint8_t* unit, uint32_t health) noexcept {
        __try { std::memcpy(unit + 0x34, &health, sizeof(health)); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool SpendCooldown(uint8_t* room, uint32_t start, uint32_t mission, uint32_t phase,
                       uint32_t target, uint32_t duration) {
        const auto now = GetTickCount64();
        AcquireSRWLockExclusive(&cooldownLock_);
        Cooldown* selected = nullptr;
        for (auto& entry : cooldowns_) {
            if (entry.room == room && entry.start == start && entry.mission == mission &&
                entry.phase == phase && entry.target == target) {
                if (now < entry.until) { ReleaseSRWLockExclusive(&cooldownLock_); return false; }
                selected = &entry; break;
            }
            if ((!entry.at || now >= entry.until) && (!selected || entry.at < selected->at)) selected = &entry;
        }
        if (!selected) { ReleaseSRWLockExclusive(&cooldownLock_); return false; }
        *selected = {room, start, mission, phase, target, now, now + duration};
        ReleaseSRWLockExclusive(&cooldownLock_);
        return true;
    }
    bool Apply(uint8_t* room, uint32_t source, uint32_t marked, uint32_t actualTarget) {
        using namespace map_boundary_contact;
        uint32_t running = 0, state = 0, mission = 0, host = 0, start = 0;
        uint8_t mode = 0, phase = 0;
        if (!Read(room, 0x21, running) || !running || !Read(room, 5, mode) ||
            (mode != 5 && mode != 8 && mode != 10) ||
            !Read(room, 0x316A4, state) || state != 2 ||
            !Read(room, 0x1B, mission) || !Read(room, 0x316F0, phase) ||
            !Read(room, 0x4CE, host) || !Read(room, 0x2EEB4, start)) return false;
        const auto* rule = map_boundary::Find(rules_, mission, phase);
        if (!rule || !(rule->flags & map_boundary::kDamageEnabled)) return false;
        const auto target = ResolveRequest(source, host, marked, actualTarget);
        if (!target) return false;
        uint8_t* pilot = nullptr;
        bool sourcePresent = false;
        bool noticeCollision = false;
        for (uint32_t slot = 0; slot < 22; ++slot) {
            auto* candidate = room + 0x3CE4 + slot * 0x1F54;
            uint32_t id = 0;
            if (!Read(candidate, 0, id)) return false;
            if (id == source && slot < 12 && !(id & kNpc)) sourcePresent = true;
            if (id == target) pilot = candidate;
            if (id == (target | kMarker)) noticeCollision = true;
        }
        if (!sourcePresent || !pilot || noticeCollision) return false;
        uint8_t form = 0, immune = 0;
        uint32_t pilotState = 0;
        if (!Read(pilot, 0x33C, form) || form >= 4 || !Read(pilot, 9, pilotState) ||
            (pilotState != 1 && pilotState != 2) || !Read(pilot, 0x75, immune) || immune) return false;
        auto* unit = pilot + 0x340 + form * 0x658;
        uint32_t health = 0, maximum = 0;
        if (!Read(unit, 4, maximum) || !Read(unit, 0x34, health) || !health ||
            health > maximum || maximum > kMaximumHealth) return false;
        const auto after = ShockHealth(health, maximum, rule->damagePermille);
        std::array<uint8_t, 13> notice{};
        const uint16_t opcode = 0x0793;
        const uint32_t noticeTarget = target | kMarker;
        std::memcpy(notice.data(), &opcode, sizeof(opcode));
        std::memcpy(notice.data() + 6, &noticeTarget, sizeof(noticeTarget));
        if (!EncodeHealth(after, notice.data() + 10) ||
            !SpendCooldown(room, start, mission, phase, target, rule->damageCooldownMs)) return false;
        // This is an explicit nonlethal environment operation, not a fabricated
        // attacker/weapon. No native death, kill-credit, life or reward path runs.
        if (!WriteHealth(unit, after)) return false;
        uint32_t recipients = 0;
        const int broadcast = broadcast_(room, notice.data(), static_cast<uint32_t>(notice.size()), &recipients);
        uint32_t committed = 0;
        if (!Read(unit, 0x34, committed) || committed != after) return false;
        api_->Log(mod_, broadcast && recipients ? SDGO_LOG_INFO : SDGO_LOG_ERROR,
            "native boundary shock: mission=%u phase=%u source=%u target=%u hp=%u->%u max=%u recipients=%u sent=%d",
            mission, static_cast<uint32_t>(phase), source, target, health, after, maximum, recipients, broadcast);
        return broadcast != 0 && recipients != 0;
    }
    void Handle(void* self, uint32_t route, const uint8_t* packet) {
        uint16_t opcode = 0;
        uint32_t source = 0, marked = 0;
        if (!Read(packet, 0, opcode) || !Read(packet, 2, source) || !Read(packet, 6, marked) ||
            opcode != 0x0492 || !map_boundary_contact::IsRequest(source, marked)) {
            original_(self, route, packet); return;
        }
        // Never forward a recognised extension into the ordinary death path,
        // including rejected, stale and terminal-state requests.
        if (self != proc_) return;
        uint32_t actualTarget = 0;
        if (!Read(packet, 10, actualTarget) || !actualTarget) return;
        uint32_t index = 0;
        auto* bytes = static_cast<uint8_t*>(proc_);
        auto* mutex = reinterpret_cast<CRITICAL_SECTION*>(bytes + 0x40);
        EnterCriticalSection(mutex);
        const int found = find_(bytes + 0x1C, &source, &index);
        LeaveCriticalSection(mutex);
        if (!found || !index) return;
        auto* room = lock_(proc_, index, opcode);
        if (!room) return;
        try { Apply(room, source, marked, actualTarget); }
        catch (...) { api_->Log(mod_, SDGO_LOG_ERROR, "boundary contact failed; native death path was not entered"); }
        unlock_(proc_, index);
    }
    static void __fastcall Detour(void* self, void*, uint32_t route, const uint8_t* packet) {
        active_->Handle(self, route, packet);
    }
public:
    bool Enabled() const { return enabled_; }
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        api_ = api; mod_ = mod;
        std::wstring path = api->ModDirectory(mod);
        path += L"\\boundaries.bin";
        FILE* stream = nullptr;
        const auto opened = _wfopen_s(&stream, path.c_str(), L"rb");
        if (opened == ENOENT) return true;
        if (opened || !stream) return false;
        std::vector<uint8_t> data;
        std::array<uint8_t, 4096> buffer{};
        while (const auto count = std::fread(buffer.data(), 1, buffer.size(), stream)) {
            data.insert(data.end(), buffer.begin(), buffer.begin() + count);
            if (data.size() > map_boundary::kHeaderBytes + map_boundary::kMaxRows * map_boundary::kRowBytes) break;
        }
        const bool fileError = std::ferror(stream) != 0;
        std::fclose(stream);
        std::string error;
        if (fileError || !map_boundary::Parse(data.data(), data.size(), rules_, error)) {
            api->Log(mod, SDGO_LOG_ERROR, "invalid boundary declaration: %s", error.c_str()); return false;
        }
        for (const auto& entry : rules_) enabled_ |= (entry.flags & map_boundary::kDamageEnabled) != 0;
        if (!enabled_) return true;
        proc_ = api->Symbol(mod, "battle.waitroom_proc");
        find_ = reinterpret_cast<FindPilot>(api->Symbol(mod, "items.pilot_map_find"));
        lock_ = reinterpret_cast<LockRoom>(api->Symbol(mod, "items.lock_room"));
        unlock_ = reinterpret_cast<UnlockRoom>(api->Symbol(mod, "items.unlock_room"));
        broadcast_ = reinterpret_cast<Broadcast>(api->Symbol(mod, "battle.room_random_broadcast"));
        if (!proc_ || !find_ || !lock_ || !unlock_ || !broadcast_) return false;
        active_ = this;
        return api->HookCode(mod, "map_catalog_server.map_contact_request", reinterpret_cast<void*>(&Detour),
                             reinterpret_cast<void**>(&original_)) != 0;
    }
};
}
