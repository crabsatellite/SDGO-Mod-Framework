#include "sdgo/battle_human_unit_observer_provider.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <float.h>
#include <set>
#include <thread>
#include <vector>

using sdgo::unit_forms::HumanUnitObserverProvider;
namespace {
struct State { std::atomic<unsigned> count{0};std::uint64_t tokens[512]{};bool changeFp = false, fail = false; };
void __cdecl Observe(const SdgoBattleHumanUnitEventV1* event, void* user) {
    auto& state = *static_cast<State*>(user);
    assert(event && event->size == sizeof(*event) && event->version == 1 && event->reserved == 0);
    assert(event->thread_id == GetCurrentThreadId());
    const auto i = state.count.fetch_add(1);assert(i < 512);state.tokens[i] = event->sequence;
    if (state.changeFp) { unsigned ignored = 0;_controlfp_s(&ignored, _RC_UP, _MCW_RC);_mm_setcsr(0x5f80);SetLastError(9191); }
    if (state.fail) RaiseException(0xe0770001, 0, 0, nullptr);
}
SdgoBattleHumanUnitObserverSinkV1 Sink(State& state) { return {sizeof(SdgoBattleHumanUnitObserverSinkV1),1,&Observe,&state}; }
}
int main() {
    HumanUnitObserverProvider provider;State state;auto sink = Sink(state);
    assert(!provider.Register(nullptr));assert(!provider.Register(reinterpret_cast<const SdgoBattleHumanUnitObserverSinkV1*>(1)));
    auto invalid = sink;invalid.version = 2;assert(!provider.Register(&invalid));
    assert(provider.Register(&sink) && provider.Register(&sink));
    SdgoBattleHumanUnitObserverStatusV1 status{sizeof(status),1};
    assert(provider.ReadStatus(&status) && status.registration_state == 2);
    State another;auto other = Sink(another);assert(!provider.Register(&other));
    const auto token = provider.Next();assert(token);
    state.changeFp = true;unsigned oldCw = 0, expectedCw = 0, actualCw = 0;_controlfp_s(&oldCw,0,0);const auto oldMx = _mm_getcsr();
    _controlfp_s(&expectedCw,_RC_DOWN,_MCW_RC);_mm_setcsr(0x7f80);
    _controlfp_s(&expectedCw,0,0);SetLastError(8080);
    assert(provider.Publish(SDGO_BATTLE_HUMAN_UNIT_BEFORE,token,&state,&sink,&provider,2,1,0) == 0);
    const auto afterError=GetLastError();_controlfp_s(&actualCw,0,0);
    assert(actualCw == expectedCw && _mm_getcsr() == 0x7f80 && afterError == 8080);
    assert(provider.Publish(SDGO_BATTLE_HUMAN_UNIT_AFTER,token,&state,&sink,&provider,2,1,0) == 0);
    assert(state.count == 2 && state.tokens[0] == state.tokens[1]);
    _controlfp_s(&actualCw,oldCw,_MCW_RC);_mm_setcsr(oldMx);
    HumanUnitObserverProvider concurrent;State concurrentState;auto concurrentSink = Sink(concurrentState);
    assert(concurrent.Register(&concurrentSink));std::vector<std::thread> workers;
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back([&] {
        for (unsigned n = 0; n < 100; ++n)
            assert(concurrent.Publish(SDGO_BATTLE_HUMAN_UNIT_ROOM_RELEASED,concurrent.Next(),&concurrent,nullptr,nullptr,0,0,0) == 0);
    });
    for (auto& worker : workers) worker.join();
    assert(concurrentState.count == 400);
    assert(std::set<std::uint64_t>(concurrentState.tokens,concurrentState.tokens+400).size() == 400);
    HumanUnitObserverProvider faulting;State bad;bad.fail = true;bad.changeFp = true;auto badSink = Sink(bad);assert(faulting.Register(&badSink));
    _controlfp_s(&expectedCw,0,0);const auto expectedMx = _mm_getcsr();SetLastError(7070);
    assert(faulting.Publish(SDGO_BATTLE_HUMAN_UNIT_BEFORE,faulting.Next(),nullptr,nullptr,nullptr,0,0,0) == 0xe0770001);
    _controlfp_s(&actualCw,0,0);assert(actualCw == expectedCw && _mm_getcsr() == expectedMx && GetLastError() == 7070);
    assert(faulting.Publish(SDGO_BATTLE_HUMAN_UNIT_AFTER,faulting.Next(),nullptr,nullptr,nullptr,0,0,0) == 0);
    assert(bad.count == 1 && !faulting.Register(&badSink));
    assert(faulting.ReadStatus(&status) && status.registration_state == 3);
    return 0;
}
