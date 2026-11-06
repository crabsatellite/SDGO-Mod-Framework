#include <windows.h>
#include "sdgo/human_unit_capture.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstring>
#include <thread>

using namespace sdgo::unit_forms;
namespace {
void Put32(std::uint8_t* p,std::size_t at,std::uint32_t value) { std::memcpy(p+at,&value,4); }
void Put64(std::uint8_t* p,std::size_t at,std::uint64_t value) { std::memcpy(p+at,&value,8); }
class Authority final : public GraphAuthority {
public:
    GraphOwner owner;
    Authority() { owner.owner="Example";owner.revisionSha256=std::string(64,'a');
        for (unsigned i=0;i<4;++i) { const auto id=15001+i*10;owner.roots.push_back(id);
            owner.nodes.push_back({id,0,NodeOrigin::owned,std::string(64,'b'),std::string(64,'c')}); } }
    std::string revision(const std::string& name)const override { return name==owner.owner?owner.revisionSha256:""; }
    bool entry(const std::string& name,unsigned root)const override {
        for(auto id:owner.roots)if(name==owner.owner&&root==id)return true;return false;
    }
    bool unit(const std::string& name,const GraphNode& node)const override { return name==owner.owner&&entry(name,node.id)&&node.target==0; }
};
std::shared_ptr<const GraphBindings> Graphs() { Authority authority;std::string error;
    auto graph=GraphBindings::Build({authority.owner},authority,error);assert(graph);return graph; }
struct Fixture {
    std::array<std::uint8_t,0x476> source{};
    std::array<std::uint8_t,0x1f54> player{};
    std::array<std::array<std::uint8_t,0x100>,4> definitions{};
    std::array<std::uint8_t,4> room{};
    std::uint8_t mode=1;unsigned selected=2;
    std::uint64_t serial=0x1234567800000001ull;
    Fixture(std::uint8_t value=1):mode(value) {
        source[0x1f]=13;Put32(source.data(),0x429,9229);Put32(player.data(),0,9229);player[0x33c]=static_cast<std::uint8_t>(selected);
        for(unsigned i=0;i<4;++i) {
            Put32(definitions[i].data(),0,15001+i*10);
            const auto at=(mode==6||mode==7)?0x19e+i*0x99:0x49;
            Put32(source.data(),at,15001+i*10);Put64(source.data(),at+8,serial+i);
            if(mode!=6&&mode!=7)break;
        }
    }
    void native() {
        for(unsigned i=0;i<((mode==6||mode==7)?4u:1u);++i) {
            const auto index=(mode==6||mode==7)?i:selected;
            auto* slot=player.data()+0x340+index*0x658;
            Put32(slot,0,15001+i*10);Put32(slot,0x6c,reinterpret_cast<std::uintptr_t>(definitions[i].data()));
            const auto deck=(mode==6||mode==7)?i+1:3;
            player[0x1d93]=static_cast<std::uint8_t>(deck);Put32(player.data(),0x1dac+(deck-1)*0x68,17+i);
        }
    }
    SdgoBattleHumanUnitEventV1 event(unsigned type,std::uint64_t sequence,int result=0) {
        SdgoBattleHumanUnitEventV1 e{};e.size=sizeof(e);e.version=1;e.type=type;e.sequence=sequence;e.thread_id=GetCurrentThreadId();e.room=room.data();
        if(type<3) { e.source=source.data();e.player=player.data();e.rule=2;e.mode=mode;e.native_result=result; }return e;
    }
    void resetPlayer() { player.fill(0);Put32(player.data(),0,9229);player[0x33c]=static_cast<std::uint8_t>(selected); }
};
}

int main() {
    HumanUnitCapture capture(Graphs());Fixture f;
    capture.observe(f.event(4,1));const auto beforeSource=f.source;const auto beforePlayer=f.player;
    capture.observe(f.event(1,2));assert(f.source==beforeSource&&f.player==beforePlayer);
    f.native();const auto initialized=f.player;capture.observe(f.event(2,2,1));assert(f.player==initialized);
    HumanUnitCaptureSnapshot output;output.generation=99;
    assert(!capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output)&&output.generation==99);
    capture.observe(f.event(5,3));assert(capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output));
    assert(output.unit.owner==9229&&output.unit.definition==f.definitions[0].data()&&output.unit.composition.part_deck==3);
    assert(output.unit.composition.boost_bonus==17&&output.unit.composition.source_rank==13&&capture.current(output));
    assert(!capture.snapshot(f.room.data(),f.player.data(),1,15001,f.serial,output));
    const auto old=output;capture.observe(f.event(4,4));assert(!capture.current(old));
    f.resetPlayer();capture.observe(f.event(1,5));f.native();capture.observe(f.event(2,5,1));capture.observe(f.event(5,6));
    assert(capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output));
    assert(output.generation!=old.generation&&output.roomEpoch!=old.roomEpoch&&!capture.current(old));
    auto wrongGeneration=output;++wrongGeneration.generation;assert(!capture.current(wrongGeneration));
    capture.observe(f.event(3,7));capture.observe(f.event(3,8));assert(!capture.current(output)&&capture.status().healthy&&capture.status().players==0);
    // A real rematch can reuse every native address and the inventory UID.
    // Release must retire the token, while the next accepted start works.
    {
        HumanUnitCapture rematch(Graphs());Fixture same;
        auto round=[&](unsigned sequence) {
            same.resetPlayer();rematch.observe(same.event(4,sequence));
            rematch.observe(same.event(1,sequence+1));same.native();
            rematch.observe(same.event(2,sequence+1,1));rematch.observe(same.event(5,sequence+2));
            HumanUnitCaptureSnapshot result;
            assert(rematch.snapshot(same.room.data(),same.player.data(),same.selected,15001,same.serial,result));
            return result;
        };
        const auto firstRound=round(1);
        rematch.observe(same.event(3,4));rematch.observe(same.event(3,5));
        assert(!rematch.current(firstRound)&&rematch.status().players==0);
        const auto secondRound=round(6);
        assert(secondRound.room==firstRound.room&&secondRound.player==firstRound.player&&
               secondRound.unit.serial==firstRound.unit.serial);
        assert(secondRound.roomEpoch!=firstRound.roomEpoch&&secondRound.generation!=firstRound.generation);
        assert(rematch.current(secondRound)&&!rematch.current(firstRound));
    }
    // A direct build cannot invent an accepted room or resurrect a released one.
    f.resetPlayer();capture.observe(f.event(1,9));f.native();capture.observe(f.event(2,9,1));assert(!capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output));
    // A rollback between BEFORE and AFTER makes the old completion stale.
    f.resetPlayer();capture.observe(f.event(4,10));capture.observe(f.event(1,11));capture.observe(f.event(4,12));f.native();capture.observe(f.event(2,11,1));capture.observe(f.event(5,13));
    assert(!capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output));
    // Failed construction and invalid native pointers never publish a seed.
    f.resetPlayer();capture.observe(f.event(4,14));capture.observe(f.event(1,15));capture.observe(f.event(2,15,0));assert(capture.status().pending==0);
    f.resetPlayer();capture.observe(f.event(4,16));capture.observe(f.event(1,17));f.native();Put32(f.player.data(),0x340+2*0x658+0x6c,1);capture.observe(f.event(2,17,1));
    assert(capture.status().healthy&&!capture.snapshot(f.room.data(),f.player.data(),2,15001,f.serial,output));
    for(auto mode:{6,7}) {
        HumanUnitCapture deckCapture(Graphs());Fixture deck(static_cast<std::uint8_t>(mode));
        deckCapture.observe(deck.event(4,1));deckCapture.observe(deck.event(1,2));deck.native();deckCapture.observe(deck.event(2,2,1));deckCapture.observe(deck.event(5,3));
        for(unsigned i=0;i<4;++i) { assert(deckCapture.snapshot(deck.room.data(),deck.player.data(),i,15001+i*10,deck.serial+i,output));
            assert(output.unit.composition.part_deck==i+1&&output.unit.composition.boost_bonus==static_cast<int>(17+i)); }
    }
    // Nested construction preserves independent invocation matches.
    HumanUnitCapture nested(Graphs());Fixture first,second;
    nested.observe(first.event(4,1));nested.observe(second.event(4,2));nested.observe(first.event(1,3));nested.observe(second.event(1,4));
    second.native();nested.observe(second.event(2,4,1));first.native();nested.observe(first.event(2,3,1));nested.observe(first.event(5,5));nested.observe(second.event(5,6));
    assert(nested.snapshot(first.room.data(),first.player.data(),2,15001,first.serial,output));
    assert(nested.snapshot(second.room.data(),second.player.data(),2,15001,second.serial,output));
    // A missing BEFORE or a new build without reset must retire old accepted data.
    nested.observe(second.event(2,77,1));assert(!nested.current(output));
    HumanUnitCapture promotion(Graphs());Fixture promoted;
    promotion.observe(promoted.event(4,1));promotion.observe(promoted.event(1,2));promoted.native();promotion.observe(promoted.event(2,2,1));promotion.observe(promoted.event(5,3));
    assert(promotion.snapshot(promoted.room.data(),promoted.player.data(),2,15001,promoted.serial,output));
    promoted.resetPlayer();promotion.observe(promoted.event(1,4));promoted.native();promotion.observe(promoted.event(2,4,1));promotion.observe(promoted.event(5,5));
    assert(!promotion.current(output));
    // Deferred bookkeeping uses only captured POD, not expired native buffers.
    HumanUnitCapture copied(Graphs());Fixture expired;
    copied.observe(expired.event(4,1));copied.observe(expired.event(1,2));expired.native();copied.observe(expired.event(2,2,1));
    expired.source.fill(0);expired.player.fill(0);expired.definitions[0].fill(0);copied.observe(expired.event(5,3));
    assert(copied.snapshot(expired.room.data(),expired.player.data(),2,15001,expired.serial,output));
    // Malformed provider events disable cached data rather than preserving it.
    auto invalid=second.event(1,7);invalid.size=0;nested.observe(invalid);assert(!nested.status().healthy&&!nested.current(output));
    HumanUnitCapture concurrent(Graphs());
    Fixture one,two;
    auto work=[&](Fixture& local,unsigned n) { concurrent.observe(local.event(4,100+n*10));concurrent.observe(local.event(1,101+n*10));
        local.native();concurrent.observe(local.event(2,101+n*10,1));concurrent.observe(local.event(5,102+n*10)); };
    std::thread a([&]{work(one,1);}),b([&]{work(two,2);});a.join();b.join();
    for(auto* local:{&one,&two}) { assert(concurrent.snapshot(local->room.data(),local->player.data(),2,15001,local->serial,output));concurrent.observe(local->event(3,200)); }
    assert(concurrent.status().healthy&&concurrent.status().players==0);
    // Queue overflow is bounded and makes all old snapshots unusable.
    HumanUnitCapture bounded(Graphs());Fixture flood;
    for(unsigned i=1;i<=129;++i)bounded.observe(flood.event(4,i));
    assert(!bounded.status().healthy);
    // Two interleaved constructors of the same identity cannot publish the older one last.
    HumanUnitCapture interleaved(Graphs());Fixture shared;interleaved.observe(shared.event(4,1));
    HANDLE firstBefore=CreateEventW(nullptr,TRUE,FALSE,nullptr),secondBefore=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    HANDLE firstAfter=CreateEventW(nullptr,TRUE,FALSE,nullptr),secondAfter=CreateEventW(nullptr,TRUE,FALSE,nullptr),done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread firstThread([&]{interleaved.observe(shared.event(1,2));SetEvent(firstBefore);WaitForSingleObject(firstAfter,INFINITE);interleaved.observe(shared.event(2,2,1));});
    WaitForSingleObject(firstBefore,INFINITE);
    std::thread secondThread([&]{interleaved.observe(shared.event(1,3));SetEvent(secondBefore);WaitForSingleObject(secondAfter,INFINITE);interleaved.observe(shared.event(2,3,1));SetEvent(done);});
    WaitForSingleObject(secondBefore,INFINITE);shared.native();SetEvent(secondAfter);WaitForSingleObject(done,INFINITE);SetEvent(firstAfter);
    firstThread.join();secondThread.join();interleaved.observe(shared.event(5,4));
    assert(interleaved.snapshot(shared.room.data(),shared.player.data(),2,15001,shared.serial,output)&&output.invocation==3);
    for(auto handle:{firstBefore,secondBefore,firstAfter,secondAfter,done})CloseHandle(handle);
    return 0;
}
