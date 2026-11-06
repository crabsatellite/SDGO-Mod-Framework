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
std::array<uint8_t, 0x40> lookupDocument{}, lookupObject{};
int lookupCalls=0, resolverCalls=0;
uint8_t* __fastcall NativeLookup(uint8_t* document,void*,uint32_t id) {
    CHECK(document==lookupDocument.data());
    ++lookupCalls;
    return id==1234 ? lookupObject.data() : nullptr;
}
void* ResolveLookup(SdgoMod*,const char*) {++resolverCalls;return reinterpret_cast<void*>(&NativeLookup);}
struct Storage {
    std::array<uint8_t, 0x3300> unit{};
    std::array<uint8_t, 0xC00> attack{};
    std::array<uint8_t, 0x400> movement{};
    std::array<uint8_t, 0x200> jump{};
    std::array<uint8_t, 0x300> definition{};
    std::array<uint8_t, 0x100> node{};
};
struct Link { Link* next; Link* previous; uint8_t* unit; };
int sightQueries = 0;
int rayCalls = 0;
bool invalidRay = false;
bool blocked = false;
bool allySightOnly = false;
bool ownerAttackable = true;
std::array<uint8_t, 0x180> wall{};
std::array<void*, 16> unitVtable{}, sceneVtable{};
// These fixtures use point-target aiming (kind 0x15); the controlled bodies
// also answer the NPC ownership query without requiring model-sphere data.
bool __fastcall IsA(void*, void*, int kind) { return kind == 0x15 || kind == 10; }
bool __fastcall Attackable(void*, void*, uint8_t* observer) {
    return ownerAttackable||*reinterpret_cast<uint32_t*>(observer+4)!=1001;
}
bool __fastcall Terrain(void*, void*, int kind) { return kind == 3; }
battle_ai::native::Ray3* __fastcall Ray(uint8_t*, void*, Ray3* ray) {
    ++rayCalls;
    if(invalidRay) return nullptr;
    *ray = {{0, 0, 0}, {0, 0, 1}};
    return ray;
}
void __fastcall Query(void*, void*, void** scenes, int* count, int, const battle_ai::native::Aabb* bounds) {
    ++sightQueries;
    const bool hit=blocked&&(!allySightOnly||bounds->min.z<1000);
    *count = hit ? 1 : 0;
    if (hit) scenes[0] = wall.data();
}
bool __cdecl Segment(const Vec3*, const Vec3*, const battle_ai::native::Aabb*) { return true; }
bool __fastcall TerrainRay(void*, void*, const Ray3*, float* distance) { *distance = 10; return true; }
bool __cdecl ShapeRay(const Ray3*, const void*, float*) { return false; }
std::array<uint8_t,0x600> publicNavigation{};
std::array<uint8_t,0x190> publicNode{};
std::array<uint8_t,0x40> publicSpawn{};
Vec3 routePoint{},submittedGoal{};
int nodeQueries=0,routeQueries=0,routeFrees=0,goalSubmissions=0;
int navigationStops=0;
int phaseCalls=0;
uint32_t lastPhaseObject=0;
uint8_t* phaseLookupUnit=nullptr;
void __fastcall StopNavigation(uint8_t*,void*) {++navigationStops;}
void __fastcall PhaseGeometry(uint8_t*,void*,uint32_t objectId) {
    ++phaseCalls;lastPhaseObject=objectId;
}
uint8_t* __fastcall PhaseLookup(uint8_t*,void*,uint32_t id) {
    return id==1001?phaseLookupUnit:nullptr;
}
uint8_t* __fastcall PublicSpawn(uint8_t*,void*,int colour,int index) {
    CHECK(colour==2&&index==0);return publicSpawn.data();
}
uint8_t* __fastcall PublicNodeForPoint(uint8_t* nav,void*,Vec3 point) {
    CHECK(nav==publicNavigation.data()&&point.x==2000&&point.y==600&&point.z==0);
    ++nodeQueries;return publicNode.data();
}
uint8_t* __fastcall PublicNodeAt(uint8_t* nav,void*,int index) {
    CHECK(nav==publicNavigation.data()&&index==0);++nodeQueries;return publicNode.data();
}
Vec3* __fastcall PublicRoute(uint8_t* nav,void*,uint32_t,Vec3,Vec3 destination,Vec3*,uint32_t* count) {
    CHECK(nav==publicNavigation.data());++routeQueries;routePoint=destination;*count=1;return &routePoint;
}
void __cdecl ReleasePublicRoute(void* point) {CHECK(point==&routePoint);++routeFrees;}
int __fastcall SubmitPublicGoal(uint8_t*,void*,const Vec3* goal,int mode) {
    CHECK(mode==1);submittedGoal=*goal;++goalSubmissions;return 1;
}

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
        auto movement = storage.movement.data();
        auto jump = storage.jump.data();
        auto definition = storage.definition.data();
        Put(unit, 0, unitVtable.data()); Put(unit, 4, id); Put(unit, 0x20, team);
        Put(unit, 0x38, int32_t(1000)); Put(unit, 0x3C, int32_t(1000));
        Put(unit, 8, storage.node.data()); Put(unit, 0x34, storage.node.data()); Put(unit, 0x9C4, definition);
        Put(unit, 0x9CC, attack); Put(unit, 0x9C8, movement);
        Put(unit, 0x9D4, jump);
        Put(unit, 0x9D8, uint32_t(1)); Put(unit, 0xEC4, uint32_t(1)); Put(unit, 0x3238, uint32_t(3604001)); unit[0x326C] = 1;
        Put(storage.node.data(), 0x88, Vec3{0, 0, z});
        Put(storage.node.data(), 0x6C, Vec3{0, 0, 1});
        Put(movement, 4, movement + 0x20);
        Put(jump, 4, jump + 0x20);
        Put(jump, 8, uint32_t(0));
        Put(jump, 0xD0, unit);
        Put(jump, 0xD4, movement);
        Put(jump, 0xD8, attack);
        Put(unit, 0xB2C, 2.0f);
        Put(unit, 0xD10, 1.0f);
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
        allySightOnly=false;ownerAttackable=true;
        invalidRay=false;rayCalls=0;
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
        add(target.teamVisible);add(target.teamSource);
        add(uint64_t(target.distance * 1000));
    }
    return value;
}
}

namespace ownership_fixture {
uintptr_t guardedBegin=0,guardedEnd=0;
LONG accessEvents=0;
LONG CALLBACK ObserveAccess(EXCEPTION_POINTERS* event) {
    const auto record=event->ExceptionRecord;
    if(record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION&&record->NumberParameters>=2) {
        const auto address=static_cast<uintptr_t>(record->ExceptionInformation[1]);
        if(address>=guardedBegin&&address<guardedEnd)InterlockedIncrement(&accessEvents);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
bool __fastcall ItemKind(void*,void*,int kind) {return kind==0x20;}
bool __fastcall NpcKind(void*,void*,int kind) {return kind==10;}
void VerifyClassificationBeforeNpcFields() {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const auto page=static_cast<size_t>(info.dwPageSize),bytes=page*5;
    auto memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS));
    CHECK(memory!=nullptr);if(!memory)return;
    DWORD old=0;CHECK(VirtualProtect(memory,page,PAGE_READWRITE,&old)!=0);
    auto small=memory+page-128;
    std::array<void*,2> table{};table[1]=reinterpret_cast<void*>(&ItemKind);
    ai_test::Put(small,0,table.data());
    guardedBegin=reinterpret_cast<uintptr_t>(memory+page);
    guardedEnd=reinterpret_cast<uintptr_t>(memory+bytes);accessEvents=0;
    const auto handler=AddVectoredExceptionHandler(1,&ObserveAccess);CHECK(handler!=nullptr);
    CHECK(!Owned(small));
    CHECK(accessEvents==0); // Classification must reject before CNPC-only offsets.
    table[1]=reinterpret_cast<void*>(&NpcKind);
    CHECK(!Owned(small));
    CHECK(accessEvents==0); // An incomplete claimed NPC is checked without faulting.
    CHECK(!Owned(nullptr)&&!Owned(reinterpret_cast<uint8_t*>(1)));
    if(handler)CHECK(RemoveVectoredExceptionHandler(handler)!=0);
    guardedBegin=guardedEnd=0;
    CHECK(VirtualFree(memory,0,MEM_RELEASE)!=0);
    std::array<uint8_t,0x3300> npc{};
    ai_test::Put(npc.data(),0,table.data());
    ai_test::Put(npc.data(),0x3238,uint32_t(3604001));npc[0x326C]=1;
    CHECK(Owned(npc.data()));
    npc[0x2B65]=1;CHECK(!Owned(npc.data()));npc[0x2B65]=0;
    npc[0x326C]=0;CHECK(!Owned(npc.data()));
}
}

int main(int argc, char**) {
    ownership_fixture::VerifyClassificationBeforeNpcFields();
    using namespace observation_fixture;
    for (size_t count : {size_t(1), size_t(16), size_t(64), size_t(128)}) {
        auto world = std::make_unique<World>(count);
        Observation observed{};
        memoryQueries = 0; sightQueries = 0;
        CHECK(Capture(observed));
        const auto queries = memoryQueries;
        Observation withoutCache{};
        memoryQueries=0;
        CHECK(GuardedCapture(withoutCache));
        CHECK(Digest(observed)==Digest(withoutCache));
        CHECK(queries<memoryQueries);
        CHECK(observed.count == count && sightQueries == int(count)*2);
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
    {
        auto world=std::make_unique<World>(1);
        Put(world->owner.attack.data(),0x240,int32_t(0));
        Put(world->owner.attack.data(),0x248,world->weapons[0].data());
        Put(world->enemies[0].node.data(),0x88,Vec3{0,0,5000});
        invalidRay=true;
        Observation melee{};
        CHECK(Capture(melee)&&rayCalls==0&&melee.count==1);
        CHECK(melee.targets[0].perceptionVisible&&melee.targets[0].visible&&
              !melee.targets[0].rangedVisible&&!melee.targets[0].meleeVisible);
        CHECK(melee.targets[0].distance==5000);
        Put(world->owner.attack.data(),0x240,int32_t(1));
        Put(world->owner.attack.data(),0x248,world->weapons[1].data());
        Observation missingGunOrigin{};
        CHECK(!Capture(missingGunOrigin)&&rayCalls==1);
    }
    {
        auto world=std::make_unique<World>(2);
        Put(world->enemies[1].unit.data(),0x20,int32_t(1));
        Put(world->enemies[1].node.data(),0x88,Vec3{0,0,1500});
        blocked=true;allySightOnly=true;
        Observation shared{};
        CHECK(Capture(shared)&&shared.count==1&&shared.allyCount==1);
        CHECK(!shared.targets[0].visible&&!shared.targets[0].rangedVisible&&!shared.targets[0].meleeVisible);
        CHECK(shared.targets[0].teamVisible&&shared.targets[0].teamSource==2001);
        Observation uncached{};
        CHECK(GuardedCapture(uncached)&&Digest(shared)==Digest(uncached));
        Put(world->enemies[1].node.data(),0x88,Vec3{0,0,100});
        Observation occluded{};
        CHECK(Capture(occluded)&&!occluded.targets[0].teamVisible);
        Put(world->enemies[1].node.data(),0x88,Vec3{0,0,1500});
        Put(world->enemies[1].unit.data(),0x38,int32_t(0));
        Put(world->enemies[1].unit.data(),0xEC4,int32_t(0));
        Observation deadAlly{};
        CHECK(Capture(deadAlly)&&deadAlly.allyCount==0&&!deadAlly.targets[0].teamVisible);
        Put(world->enemies[1].unit.data(),0x38,int32_t(1000));
        Put(world->enemies[1].unit.data(),0xEC4,int32_t(1));ownerAttackable=false;
        Observation differentPermission{};
        CHECK(Capture(differentPermission)&&differentPermission.count==1);
        CHECK(!differentPermission.targets[0].attackable&&!differentPermission.targets[0].visible&&
              differentPermission.targets[0].teamVisible);
    }
    {
        publicNavigation={};uint32_t count=0;
        CHECK(!NavigationNodeCount(publicNavigation.data(),count));
        Put(publicNavigation.data(),0x580,int32_t(1));
        CHECK(NavigationNodeCount(publicNavigation.data(),count)&&count==1);
        Put(publicNavigation.data(),0x580,int32_t(-1));
        CHECK(!NavigationNodeCount(publicNavigation.data(),count));
        std::array<uint32_t,2> nodes{};
        Put(publicNavigation.data(),0x560,reinterpret_cast<uint8_t*>(nodes.data()));
        Put(publicNavigation.data(),0x564,reinterpret_cast<uint8_t*>(nodes.data()+2));
        CHECK(NavigationNodeCount(publicNavigation.data(),count)&&count==2);
        Put(publicNavigation.data(),0x564,reinterpret_cast<uint8_t*>(nodes.data())+1);
        CHECK(!NavigationNodeCount(publicNavigation.data(),count));

        auto world=std::make_unique<World>(1);
        publicNavigation={};Put(publicNavigation.data(),0x580,int32_t(1));
        Put(world->document.data(),0x8364,publicNavigation.data());
        Put(publicSpawn.data(),0x24,Vec3{2000,600,0});
        Put(publicNode.data(),0x180,Vec3{2000,0,0});
        g_startPoint=reinterpret_cast<StartPointFn>(&PublicSpawn);
        g_navigationNode=reinterpret_cast<NavigationNodeFn>(&PublicNodeForPoint);
        g_navigationNodeAt=reinterpret_cast<NavigationNodeAtFn>(&PublicNodeAt);
        g_route=reinterpret_cast<RouteFn>(&PublicRoute);g_delete=&ReleasePublicRoute;
        g_setGoal=reinterpret_cast<GoalFn>(&SubmitPublicGoal);
        nodeQueries=routeQueries=routeFrees=goalSubmissions=0;
        const bool explored=ExploreMapUnsafe();
        std::printf("PUBLIC NAV admitted=%d nodes=%d routes=%d frees=%d submissions=%d available=%d cursor=%u\n",
            explored,nodeQueries,routeQueries,routeFrees,goalSubmissions,MovementAvailable(g_current,false),ActorFor(g_current).searchCursor);
        CHECK(explored);
        CHECK(nodeQueries==1&&routeQueries==1&&routeFrees==1&&goalSubmissions==1);
        CHECK(submittedGoal.x==2000&&submittedGoal.y==0&&submittedGoal.z==0);
        CHECK(ActorFor(g_current).explorationOwned&&ActorFor(g_current).explorationRoutes==1);
        Put(world->owner.movement.data(),8,world->owner.movement.data()+0x124);
        CHECK(!MovementAvailable(g_current,true)&&!ExploreMapUnsafe());
        CHECK(goalSubmissions==1);
    }
    {
        auto world=std::make_unique<World>(1);
        g_stop=reinterpret_cast<decltype(g_stop)>(&StopNavigation);navigationStops=0;
        auto& actor=ActorFor(g_current);actor.defenceMoveOwned=true;actor.path={{1000,0,0}};
        actor.geometryOwned=true;actor.geometryFinal=true;
        Put(world->owner.movement.data(),4,world->owner.movement.data()+0x20);
        Put(world->owner.movement.data(),8,world->owner.movement.data()+0x124);
        ApplyDefence(g_current,actor);
        CHECK(navigationStops==0&&!actor.defenceMoveOwned&&actor.path.empty());
        actor.path={{1000,0,0}};actor.geometryOwned=true;actor.geometryFinal=true;
        CHECK(!FollowPath(g_current,actor)&&actor.path.empty()&&!actor.geometryOwned&&!actor.geometryFinal);
        Put(world->owner.movement.data(),8,uint32_t(0));
        Put(world->owner.attack.data(),4,world->owner.attack.data()+0x48);
        actor.defenceMoveOwned=true;actor.path={{1000,0,0}};
        ApplyDefence(g_current,actor);CHECK(navigationStops==0&&!actor.defenceMoveOwned);
        Put(world->owner.attack.data(),4,world->owner.attack.data()+0x20);
        actor.defenceMoveOwned=true;actor.path={{1000,0,0}};
        ApplyDefence(g_current,actor);CHECK(navigationStops==1&&!actor.defenceMoveOwned);
    }
    {
        auto world=std::make_unique<World>(1);
        auto movement=world->owner.movement.data();
        auto jump=world->owner.jump.data();
        Put(movement,0x19C,1.5f);
        Put(movement,0x1A4,-0.5f);
        Put(jump,0xAC,12.0f);
        Put(world->owner.node.data(),0x88,Vec3{10,100,20});
        battle_ai::native::TransportOrdinaryMotionProfile profile{};
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
                  TransportOrdinaryProfileState::Ready&&
              profile.runStep==2.0f&&
              profile.gravityField==1.0f&&
              profile.state.position.x==10&&
              profile.state.position.y==100&&
              profile.state.velocity.x==1.5f&&
              profile.state.velocity.y==12&&
              profile.state.velocity.z==-0.5f);
        Put(jump,8,jump+0x98);
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
              TransportOrdinaryProfileState::Forced);
        Put(jump,8,uint32_t(0));
        for(size_t state:{size_t(0x48),size_t(0x5C),size_t(0x70)}) {
            Put(movement,4,movement+state);
            CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
                  TransportOrdinaryProfileState::Unsupported);
        }
        Put(movement,4,movement+0x20);
        Put(movement,8,movement+0x48);
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
              TransportOrdinaryProfileState::Unsupported);
        Put(movement,8,movement+0x124);
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
              TransportOrdinaryProfileState::Forced);
        Put(movement,8,uint32_t(0));
        world->owner.definition[0x131]=1;
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
              TransportOrdinaryProfileState::Unsupported);
        world->owner.definition[0x131]=0;
        Put(world->owner.node.data(),0xF8,uint32_t(1));
        CHECK(ReadTransportOrdinaryProfile(g_current,profile)==
              TransportOrdinaryProfileState::Unsupported);
        Put(world->owner.node.data(),0xF8,uint32_t(0));
        Actor::TransportRoute trip{};
        trip.exit={40,80,20};
        g_setGoal=reinterpret_cast<GoalFn>(&SubmitPublicGoal);
        goalSubmissions=0;
        const auto before=Position(g_current);
        CHECK(EnsureTransportLandingGoal(g_current,trip)&&
              goalSubmissions==1&&
              submittedGoal.x==40&&submittedGoal.y==80&&
              submittedGoal.z==20&&
              Position(g_current).x==before.x&&
              Position(g_current).y==before.y&&
              Position(g_current).z==before.z);
        Put(g_current,0x3078,trip.exit);
        Put(g_current,0xDC4,uint32_t(1));
        g_current[0x30DC]=1;
        CHECK(TransportLandingGoalCurrent(g_current,trip)&&
              EnsureTransportLandingGoal(g_current,trip)&&
              goalSubmissions==1);
        CHECK(!TransportLandingObservationExpired(11,10)&&
              TransportLandingObservationExpired(12,10));
    }
    {
        auto world=std::make_unique<World>(1);
        g_stop=reinterpret_cast<decltype(g_stop)>(&StopNavigation);
        auto& actor=ActorFor(g_current);
        const auto now=GetTickCount64();
        actor.transport.active=true;actor.transport.phase=1;
        actor.transport.waitingAt=now;
        CHECK(TransportMotorReserved(actor.transport,now));
        CancelOwnedRoute(g_current,actor,false);
        CHECK(actor.transport.active&&actor.transport.phase==1);

        actor.transport.request.committed=true;
        actor.transport.request.committedAt=now;
        actor.transport.waitingAt=0;
        CancelOwnedRoute(g_current,actor,false);
        CHECK(actor.transport.active&&actor.transport.request.committed);
        battle_ai::native::MapGeometry unavailable{};
        BeginGeometrySearch(g_current,actor,unavailable,{5000,0,0});
        CHECK(actor.transport.active&&actor.transport.request.committed);

        actor.transport.request.acknowledged=true;
        CancelOwnedRoute(g_current,actor,true);
        CHECK(actor.transport.active&&actor.transport.request.acknowledged);

        battle_ai::native::TransportRequestStatus status{};
        status.available=true;status.generation=9;
        status.requestCount=4;status.noticeCount=7;
        battle_ai::native::BeginTransportRequestLease(
            actor.transport.request,status);
        status.requestCount=5;status.outstanding=true;
        status.outstandingGeneration=9;
        CHECK(ReconcileTransportRequestStatus(
                  actor,status,true,now)==
              battle_ai::native::TransportRequestUpdate::Committed);
        CancelOwnedRoute(g_current,actor,false);
        BeginGeometrySearch(g_current,actor,unavailable,{7000,0,0});
        status.outstanding=false;++status.noticeCount;
        CHECK(ReconcileTransportRequestStatus(
                  actor,status,false,now+16)==
                  battle_ai::native::TransportRequestUpdate::Acknowledged&&
              actor.transport.active&&actor.transport.request.acknowledged);
        CancelOwnedRoute(g_current,actor,true);
        CHECK(actor.transport.active&&actor.transport.request.acknowledged);
        ++status.timeoutCount;
        CHECK(ReconcileTransportRequestStatus(
                  actor,status,false,now+32)==
                  battle_ai::native::TransportRequestUpdate::Failed&&
              !actor.transport.active&&actor.transport.phase==4);

        actor.transport.active=true;actor.transport.phase=1;
        status={};status.available=true;status.generation=9;
        status.requestCount=5;status.noticeCount=8;
        battle_ai::native::BeginTransportRequestLease(
            actor.transport.request,status);
        status={};status.available=true;status.generation=10;
        CHECK(ReconcileTransportRequestStatus(
                  actor,status,
                  true,now+48)==
                  battle_ai::native::TransportRequestUpdate::GenerationChanged&&
              !actor.transport.active&&actor.transport.phase==4);

        actor.transport.active=true;actor.transport.phase=1;
        actor.transport.waitingAt=now-
            battle_ai::native::TransportEntranceWaitMs(false)-1;
        CancelOwnedRoute(g_current,actor,false);
        CHECK(!actor.transport.active&&actor.transport.phase==0);
    }
    {
        auto world=std::make_unique<World>(1);
        g_stop=reinterpret_cast<decltype(g_stop)>(&StopNavigation);
        g_phaseHide=reinterpret_cast<PhaseGeometryFn>(&PhaseGeometry);
        g_phaseUnhide=reinterpret_cast<PhaseGeometryFn>(&PhaseGeometry);
        g_phaseGeometryRevision.store(10);
        g_phaseHideCount.store(0);g_phaseUnhideCount.store(0);
        g_phaseEventSequence.store(0);g_phaseLastObject.store(0);
        g_phaseLastAction.store(0);
        phaseCalls=0;lastPhaseObject=0;navigationStops=0;
        auto& actor=ActorFor(g_current);
        actor.phaseGeometryRevision=10;actor.path={{1000,0,0}};
        actor.geometryOwned=true;actor.geometryFinal=true;actor.pathGeometry=true;
        actor.explorationOwned=true;actor.defenceMoveOwned=true;actor.hasIssuedGoal=true;
        actor.direct.owned=true;actor.transport.active=true;actor.transport.phase=1;
        actor.transportRevision=4;actor.cachedTransports.resize(1);
        actor.searchTransports.resize(1);actor.searchVolumes.resize(1);
        actor.transportBlockers.resize(1);
        PhaseHide(nullptr,nullptr,77);
        CHECK(phaseCalls==1&&lastPhaseObject==77&&g_phaseGeometryRevision.load()==11&&
              g_phaseHideCount.load()==1&&g_phaseUnhideCount.load()==0&&
              g_phaseEventSequence.load()==1&&g_phaseLastObject.load()==77&&
              g_phaseLastAction.load()==1);
        SynchronizePhaseGeometry(g_current,actor);
        CHECK(actor.phaseGeometryRevision==11&&actor.path.empty()&&!actor.geometryOwned&&
              !actor.geometryFinal&&!actor.pathGeometry&&!actor.explorationOwned&&
              !actor.defenceMoveOwned&&!actor.hasIssuedGoal&&!actor.direct.owned&&
              !actor.transport.active&&actor.transport.phase==0&&
              actor.cachedTransports.empty()&&actor.searchTransports.empty()&&
              actor.searchVolumes.empty()&&actor.transportBlockers.empty()&&
              actor.transportRevision==UINT64_MAX&&navigationStops==1&&
              actor.phaseInvalidations==1&&actor.phaseRebuildPending);
        actor.path={{500,0,0}};RecordRouteInstall(actor);
        CHECK(actor.pathGeneration==1&&actor.phaseRouteRebuilds==1&&
              !actor.phaseRebuildPending);
        phaseLookupUnit=g_current;
        g_findObject=reinterpret_cast<FindObjectFn>(&PhaseLookup);
        SdgoBattleAiPhaseGeometrySnapshotV1 phase{};
        phase.size=sizeof(phase);phase.version=1;
        CHECK(SdgoBattleAiGetPhaseGeometrySnapshotV1(1001,&phase)==1&&
              phase.global_revision==11&&phase.hide_count==1&&
              phase.unhide_count==0&&phase.last_object_id==77&&
              phase.last_action==1&&phase.last_event_sequence==1&&
              phase.actor_valid&&phase.actor_generation==actor.generation&&
              phase.actor_seen_revision==11&&
              phase.actor_invalidation_count==1&&
              phase.actor_route_rebuild_count==1&&
              phase.actor_path_generation==1&&phase.route_active);
        actor.phaseGeometryRevision=11;actor.transport.active=true;actor.transport.native=true;
        actor.transport.phase=2;actor.path={{1000,0,0}};navigationStops=0;
        PhaseUnhide(nullptr,nullptr,91);
        SynchronizePhaseGeometry(g_current,actor);
        CHECK(phaseCalls==2&&lastPhaseObject==91&&g_phaseGeometryRevision.load()==12&&
              g_phaseHideCount.load()==1&&g_phaseUnhideCount.load()==1&&
              g_phaseEventSequence.load()==2&&g_phaseLastObject.load()==91&&
              g_phaseLastAction.load()==2&&
              actor.transport.active&&actor.transport.native&&actor.transport.phase==2&&
              actor.path.empty()&&navigationStops==1&&
              actor.phaseInvalidations==2&&actor.phaseRebuildPending);
    }
    {
        SdgoMissionAiClientSnapshotV1 mission{};
        mission.size=sizeof(mission);mission.version=1;
        mission.active=1;mission.exact_plain_mission=1;
        mission.mode_class=SDGO_MISSION_AI_MODE_CLASS_PLAIN_MISSION;
        mission.local_is_host=1;mission.network_game=1;
        mission.room_records_valid=1;mission.find_object_available=1;
        mission.room_mode=8;mission.mode_object=0x11223344;
        mission.unit_manager_object=0x55667788;mission.ai_record_count=1;
        auto& wingman=mission.participants[0];
        wingman.pilot_id=0x80000051;wingman.unit_server_id=wingman.pilot_id;
        wingman.object_id=1001;wingman.spawned=1;
        wingman.valid_registered_object=1;wingman.mapping_valid=1;
        wingman.local_control=1;wingman.master_npc=1;wingman.ai_enabled=1;
        CHECK(battle_ai::mission::AdmitsWingman(
            mission,0x11223344,0x55667788,8,1001,0x80000051));
        wingman.remote_copy=1;
        CHECK(!battle_ai::mission::AdmitsWingman(
            mission,0x11223344,0x55667788,8,1001,0x80000051));
        wingman.remote_copy=0;mission.local_is_host=0;
        CHECK(!battle_ai::mission::AdmitsWingman(
            mission,0x11223344,0x55667788,8,1001,0x80000051));
        mission.local_is_host=1;
        CHECK(!battle_ai::mission::AdmitsWingman(
            mission,0x11223344,0x55667788,8,1001,51));
        CHECK(!battle_ai::mission::AdmitsWingman(
            mission,0x11223344,0x55667788,5,1001,0x80000051));
    }
    ai_test::api.Symbol=&ResolveLookup;
    auto document=lookupDocument.data();g_document=&document;
    g_findObject=reinterpret_cast<FindObjectFn>(&NativeLookup);
    CHECK(FindObject(1234)==lookupObject.data() && FindObject(9999)==nullptr);
    CHECK(lookupCalls==2 && resolverCalls==0);
    document=nullptr;
    CHECK(FindObject(1234)==nullptr && lookupCalls==2 && resolverCalls==0);
    document=lookupDocument.data();g_findObject=nullptr;
    CHECK(FindObject(1234)==nullptr && lookupCalls==2 && resolverCalls==0);
    return ai_test::Finish();
}
