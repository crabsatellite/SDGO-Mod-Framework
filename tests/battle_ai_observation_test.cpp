#include "ai_test_support.h"
#include <memory>

namespace {
size_t memoryQueries = 0;
SIZE_T WINAPI CountVirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size) {
    ++memoryQueries;
    return ::VirtualQuery(address, info, size);
}
}
#define VirtualQuery CountVirtualQuery
#include "../mods/battle_ai/battle_ai.cpp"
#undef VirtualQuery

namespace observation_fixture {
using ai_test::Put;
struct Storage {
    std::array<uint8_t, 0x3300> unit{};
    std::array<uint8_t, 0xC00> attack{};
    std::array<uint8_t, 0x400> movement{};
    std::array<uint8_t, 0x300> definition{};
    std::array<uint8_t, 0x100> node{};
};
struct Link { Link* next; Link* previous; uint8_t* unit; };
int sightQueries = 0;
bool blocked = false;
std::array<uint8_t, 0x180> wall{};
std::array<void*, 16> unitVtable{}, sceneVtable{};
bool __fastcall IsA(void*, void*, int kind) { return kind == 0x15; }
bool __fastcall Attackable(void*, void*, uint8_t*) { return true; }
bool __fastcall Terrain(void*, void*, int kind) { return kind == 3; }
battle_ai::native::Ray3* __fastcall Ray(uint8_t*, void*, Ray3* ray) {
    *ray = {{0, 0, 0}, {0, 0, 1}};
    return ray;
}
void __fastcall Query(void*, void*, void** scenes, int* count, int, const battle_ai::native::Aabb*) {
    ++sightQueries;
    *count = blocked ? 1 : 0;
    if (blocked) scenes[0] = wall.data();
}
bool __cdecl Segment(const Vec3*, const Vec3*, const battle_ai::native::Aabb*) { return true; }
bool __fastcall TerrainRay(void*, void*, const Ray3*, float* distance) { *distance = 10; return true; }
bool __cdecl ShapeRay(const Ray3*, const void*, float*) { return false; }

struct World {
    Storage owner;
    std::array<Storage, 128> enemies;
    std::array<std::array<uint8_t, 0x700>, 3> weapons{};
    std::array<uint8_t, 0x8500> document{};
    uint8_t* documentPointer = document.data();
    Link head{};
    std::array<Link, 128> entries{};

    void Configure(Storage& storage, uint32_t id, int team, float z) {
        auto unit = storage.unit.data(), attack = storage.attack.data();
        auto movement = storage.movement.data(), definition = storage.definition.data();
        Put(unit, 0, unitVtable.data()); Put(unit, 4, id); Put(unit, 0x20, team);
        Put(unit, 0x38, int32_t(1000)); Put(unit, 0x3C, int32_t(1000));
        Put(unit, 8, storage.node.data()); Put(unit, 0x34, storage.node.data()); Put(unit, 0x9C4, definition);
        Put(unit, 0x9CC, attack); Put(unit, 0x9C8, movement);
        Put(unit, 0x9D8, uint32_t(1)); Put(unit, 0x3238, uint32_t(3604001)); unit[0x326C] = 1;
        Put(storage.node.data(), 0x88, Vec3{0, 0, z});
        Put(storage.node.data(), 0x6C, Vec3{0, 0, 1});
        Put(movement, 4, movement + 0x20);
        Put(attack, 4, attack + 0x20); Put(attack, 0x240, int32_t(1));
        Put(attack, 0x248, weapons[1].data());
        Put(definition, 0, uint32_t(10001)); definition[0x247] = 3;
        for (int slot = 0; slot < 3; ++slot) {
            Put(definition, 0x249 + slot * 4, uint32_t(41001 + slot));
            for (size_t bank : {size_t(0x24C), size_t(0x56C)}) {
                Put(attack, bank + slot * 0xC8, int16_t(10));
                Put(attack, bank + slot * 0xC8 + 0xC, weapons[slot].data());
            }
        }
        // Native protected boost float: zero key and sequential byte permutation.
        for (auto offsets : {std::array<size_t, 2>{0xBCC, 0xBD0}, {0xBE1, 0xBE5}}) {
            for (uint8_t i = 0; i < 4; ++i) unit[offsets[0] + i] = i;
            Put(unit, offsets[1], 100.0f);
        }
    }
    explicit World(size_t count) {
        blocked = false;
        unitVtable[1] = reinterpret_cast<void*>(&IsA);
        unitVtable[10] = reinterpret_cast<void*>(&Attackable);
        sceneVtable[5] = reinterpret_cast<void*>(&Terrain);
        Put(wall.data(), 0, sceneVtable.data());
        Put(wall.data(), 0xBC, battle_ai::native::Aabb{{-100, -100, -100}, {10000, 10000, 10000}});
        for (int slot = 0; slot < 3; ++slot) {
            auto record = weapons[slot].data();
            Put(record, 0, uint32_t(41001 + slot)); record[0x69] = 1;
            record[0x6A] = slot == 0 ? 1 : 2;
            Put(record, 0x6C, int32_t(1000)); Put(record, 0x72, uint32_t(slot == 0 ? 200 : 2800));
            Put(record, 0x7C, int16_t(slot == 0 ? 0 : 10));
        }
        Configure(owner, 1001, 1, 0);
        for (size_t i = 0; i < count; ++i) {
            Configure(enemies[i], uint32_t(2000 + i), 2, 1200 + float(i));
            entries[i] = {i + 1 < count ? &entries[i + 1] : &head,
                          i ? &entries[i - 1] : &head, enemies[i].unit.data()};
        }
        head = {count ? &entries[0] : &head, count ? &entries[count - 1] : &head, nullptr};
        Put(document.data(), 0x48, &head);
        // The same unit list appears through a second native collection.
        Put(document.data(), 0x70, &head);
        Put(document.data(), 0x82C4, document.data());
        g_api = &ai_test::api; g_current = owner.unit.data(); g_document = &documentPointer;
        g_gameThread = GetCurrentThreadId(); g_runtime = RuntimeSettings{};
        g_ray = reinterpret_cast<RayFn>(&Ray);
        g_navigation = {reinterpret_cast<battle_ai::native::QueryFn>(&Query), &Segment,
                        reinterpret_cast<battle_ai::native::TerrainRayFn>(&TerrainRay), &ShapeRay, &ShapeRay};
        g_actors.clear(); g_generations.clear(); g_motion.clear(); g_incoming.clear();
    }
};
uint64_t Digest(const Observation& o) {
    uint64_t value = 1469598103934665603ull;
    auto add = [&](uint64_t field) { value = (value ^ field) * 1099511628211ull; };
    add(o.id); add(o.hp); add(o.count); add(o.hurt); add(o.caps.unitId); add(o.caps.selectedSlot);
    for (size_t i = 0; i < o.count; ++i) {
        const auto& target = o.targets[i];
        add(target.id); add(target.hp); add(target.visible); add(target.rangedVisible);
        add(target.meleeVisible); add(target.hurt); add(target.damageProtected);
        add(uint64_t(target.distance * 1000));
    }
    return value;
}
}

int main(int argc, char**) {
    using namespace observation_fixture;
    for (size_t count : {size_t(1), size_t(16), size_t(64), size_t(128)}) {
        auto world = std::make_unique<World>(count);
        Observation observed{};
        memoryQueries = 0; sightQueries = 0;
        CHECK(Capture(observed));
        const auto queries = memoryQueries;
        CHECK(observed.count == count && sightQueries == int(count));
        for (size_t i = 0; i < count; ++i)
            CHECK(observed.targets[i].id == 2000 + i && observed.targets[i].visible);
        std::printf("OBS count=%zu memory_queries=%zu sight_queries=%d digest=%llu\n",
                    count, queries, sightQueries, static_cast<unsigned long long>(Digest(observed)));
        if (argc > 1) {
            LARGE_INTEGER frequency{}, begin{}, end{}; QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&begin);
            constexpr int repetitions = 300;
            for (int iteration = 0; iteration < repetitions; ++iteration) {
                Observation sample{};
                CHECK(Capture(sample) && sample.count == count);
            }
            QueryPerformanceCounter(&end);
            const double micros = double(end.QuadPart - begin.QuadPart) * 1e6 / double(frequency.QuadPart) / repetitions;
            std::printf("TIME count=%zu us=%.3f\n", count, micros);
        }
        Put(world->enemies[0].unit.data(), 0xEB4, uint8_t(1));
        Put(world->enemies[0].unit.data(), 0x38, int32_t(777));
        Put(world->enemies[0].movement.data(), 4, world->enemies[0].movement.data() + 0xE8);
        Put(world->enemies[0].node.data(), 0x88, Vec3{0, 0, 2500});
        Observation changed{};
        CHECK(Capture(changed) && changed.targets[0].hp == 777 && changed.targets[0].hurt &&
              changed.targets[0].damageProtected && changed.targets[0].distance == 2500);
        blocked = true; sightQueries = 0;
        Observation hidden{};
        CHECK(Capture(hidden) && hidden.count == count && sightQueries == int(count));
        for (size_t i = 0; i < count; ++i) CHECK(!hidden.targets[i].visible);
        std::printf("CHANGED count=%zu digest=%llu hidden_digest=%llu\n", count,
                    static_cast<unsigned long long>(Digest(changed)), static_cast<unsigned long long>(Digest(hidden)));
        CHECK(battle_ai::native::detail::g_readableRegionCache == nullptr);
        auto valid = world->entries[0].unit;
        world->entries[0].unit = reinterpret_cast<uint8_t*>(0x20);
        Observation unavailable{};
        CHECK(!Capture(unavailable));
        CHECK(battle_ai::native::detail::g_readableRegionCache == nullptr);
        world->entries[0].unit = valid;
    }
    return ai_test::Finish();
}
