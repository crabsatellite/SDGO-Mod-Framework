#pragma once

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include "sdgo/sdgo_api.h"
#include "sdgo/game_test.h"
#include "sdgo/battle_ai_observer.h"
#include "../battle_ai/native_navigation.h"

// Read-only, opt-in native spatial-query evidence. It never moves a block,
// writes HP, supplies collision results or changes the mission's outcome.
class MapGateProbe {
    using IsAFn = bool(__thiscall*)(void*, int);
    using Vec3 = battle_ai::Vec3;
    using Aabb = battle_ai::native::Aabb;
    using Ray3 = battle_ai::native::Ray3;
    bool enabled_ = false;
    uint32_t block_ = 0;
    Vec3 from_{}, to_{};
    uint8_t** document_ = nullptr;
    battle_ai::native::QueryFn query_ = nullptr;
    battle_ai::native::RayObbFn rayObb_ = nullptr;
    battle_ai::native::RaySphereFn raySphere_ = nullptr;
    SdgoBattleAiGetPhaseGeometrySnapshotV1Fn phase_ = nullptr;
    uint64_t last_ = 0;
    uint64_t first_ = 0;

    static bool Readable(const void* ptr, size_t size) {
        if (!ptr) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(ptr, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const auto begin = reinterpret_cast<uintptr_t>(ptr);
        const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        return begin <= end && size <= end - begin;
    }
    static bool ReadVector(const SdgoApi* api, SdgoMod* mod, const char* key, Vec3& value) {
        char text[128]{};
        api->SettingString(mod, key, "", text, sizeof(text));
        return sscanf_s(text, "%f,%f,%f", &value.x, &value.y, &value.z) == 3 &&
               std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }
    struct Sample {int status = 0;int scenes = 0;int found = 0;int hits = 0;int hp = 0;int obbs = 0;int spheres = 0;};
    Sample QueryUnsafe() const {
        using namespace battle_ai::native;
        Sample sample{};
        if (!Readable(document_, sizeof(void*)) || !*document_) return sample;
        auto doc = *document_;
        if (!Readable(doc + detail::kDocumentQuadTree, sizeof(void*))) return sample;
        auto tree = *reinterpret_cast<void**>(doc + detail::kDocumentQuadTree);
        if (!tree) return sample;
        const Aabb bounds = {{(std::min)(from_.x, to_.x)-1, (std::min)(from_.y, to_.y)-1, (std::min)(from_.z, to_.z)-1},
                             {(std::max)(from_.x, to_.x)+1, (std::max)(from_.y, to_.y)+1, (std::max)(from_.z, to_.z)+1}};
        void* scenes[detail::kSceneCapacity]{};
        query_(tree, scenes, &sample.scenes, detail::kSceneCapacity, &bounds);
        if (sample.scenes < 0 || sample.scenes >= detail::kSceneCapacity) return sample;
        const Vec3 delta = to_ - from_;
        const double length = Length(delta);
        const Ray3 ray{from_, Normalize(delta)};
        for (int i=0; i<sample.scenes; ++i) {
            auto scene = static_cast<uint8_t*>(scenes[i]);
            if (!Readable(scene, 0x116)) return sample;
            auto object = *reinterpret_cast<uint8_t**>(scene + detail::kSceneBackPointer);
            if (!Readable(object, 0x3C)) continue;
            auto table = *reinterpret_cast<void***>(object);
            if (!Readable(table, 2*sizeof(void*)) || !table[1]) return sample;
            if (!reinterpret_cast<IsAFn>(table[1])(object, 0x12) ||
                *reinterpret_cast<uint32_t*>(object + detail::kObjectId) != block_) continue;
            ++sample.found;
            sample.hp = *reinterpret_cast<int32_t*>(object + 0x38);
            const int obbs = *reinterpret_cast<int*>(scene + detail::kObbCount);
            const int spheres = scene[detail::kSphereEnabled] ? *reinterpret_cast<int*>(scene + detail::kSphereCount) : 0;
            if (obbs < 0 || obbs > 4096 || spheres < 0 || spheres > 4096) return sample;
            auto boxes = *reinterpret_cast<uint8_t**>(scene + detail::kObbArray);
            auto balls = *reinterpret_cast<uint8_t**>(scene + detail::kSphereArray);
            sample.obbs += obbs;sample.spheres += spheres;
            for (int j=0; j<obbs; ++j) {
                auto shape = boxes + static_cast<size_t>(j)*detail::kObbStride;
                if (!Readable(shape, detail::kObbStride)) return sample;
                float distance = -1;
                if (rayObb_(&ray, shape, &distance) && std::isfinite(distance) && distance >= 0 && distance <= length) ++sample.hits;
            }
            for (int j=0; j<spheres; ++j) {
                auto shape = balls + static_cast<size_t>(j)*detail::kSphereStride;
                if (!Readable(shape, detail::kSphereStride)) return sample;
                float distance = -1;
                if (raySphere_(&ray, shape, &distance) && std::isfinite(distance) && distance >= 0 && distance <= length) ++sample.hits;
            }
        }
        sample.status = 1;
        return sample;
    }
    Sample Query() const {
        __try {return QueryUnsafe();}
        __except (EXCEPTION_EXECUTE_HANDLER) {return {};}
    }
public:
    bool Enabled() const { return enabled_; }
    bool Install(const SdgoApi* api, SdgoMod* mod) {
        enabled_ = api->SettingInt(mod, "MapGateProbe", 0) != 0;
        if (!enabled_) return true;
        block_ = static_cast<uint32_t>(api->SettingInt(mod, "MapGateProbeBlock", 0));
        document_ = reinterpret_cast<uint8_t**>(api->Symbol(mod, "game.document"));
        query_ = reinterpret_cast<battle_ai::native::QueryFn>(api->Symbol(mod, "collision.query"));
        rayObb_ = reinterpret_cast<battle_ai::native::RayObbFn>(api->Symbol(mod, "collision.ray_obb"));
        raySphere_ = reinterpret_cast<battle_ai::native::RaySphereFn>(api->Symbol(mod, "collision.ray_sphere"));
        if (const auto library = GetModuleHandleW(L"battle_ai.dll"))
            phase_ = reinterpret_cast<SdgoBattleAiGetPhaseGeometrySnapshotV1Fn>(
                GetProcAddress(library, SDGO_BATTLE_AI_PHASE_GEOMETRY_OBSERVER_EXPORT));
        if (!block_ || !document_ || !query_ || !rayObb_ || !raySphere_ ||
            !ReadVector(api, mod, "MapGateProbeFrom", from_) || !ReadVector(api, mod, "MapGateProbeTo", to_)) return false;
        const auto length = battle_ai::Length(to_ - from_);
        return std::isfinite(length) && length > 0 && length <= 100000;
    }
    void Poll(const SdgoGameTestContextV1* context, uint32_t gameMode, uint32_t appState) {
        if (!enabled_ || !gameMode || appState != 22) return;
        const auto now = GetTickCount64();
        if (last_ && now-last_ < 1000) return;
        last_ = now;if (!first_) first_ = now;
        const Sample sample = Query();
        SdgoBattleAiPhaseGeometrySnapshotV1 phase{};
        phase.size=sizeof(phase);phase.version=1;
        const int phaseAvailable=phase_ ? phase_(0,&phase) : 0;
        char text[768];
        sprintf_s(text, "native-map-gate {\"elapsed_ms\":%llu,\"game_mode\":%u,\"app_state\":%u,\"block\":%u,\"query_valid\":%d,\"scenes\":%d,\"found\":%d,\"ray_hits\":%d,\"hp\":%d,\"obbs\":%d,\"spheres\":%d,\"phase_observer\":%d,\"hide_count\":%llu,\"last_action\":%u,\"last_object\":%u}",
                  static_cast<unsigned long long>(now-first_), gameMode, appState, block_, sample.status, sample.scenes,
                  sample.found, sample.hits, sample.hp, sample.obbs, sample.spheres,phaseAvailable,
                  static_cast<unsigned long long>(phase.hide_count),phase.last_action,phase.last_object_id);
        context->Observe(text);
    }
};
