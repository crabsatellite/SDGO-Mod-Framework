#include <array>
#include <cstdio>
#include <cstring>
#include "../mods/room_random/loadout_sync.h"
#include "../mods/room_random_client/cache_sync.h"
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
template<class T> void Put(uint8_t* p,size_t n,T v){std::memcpy(p+n,&v,sizeof(v));}
}
int main() {
    using namespace room_random_client;
    std::array<uint8_t,0x5DA8> profile{};std::array<uint8_t,0x4000> room{};
    Context c{profile.data(),room.data(),1001};CacheSync sync;
    Put<uint32_t>(room.data(),0,7);room[4]=2;room[5]=8;
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    std::array<uint8_t,0x99> unit{};Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x123456789ABCuLL);
    auto notice=room_random_sync::Packet(0x0861,1001,0,7,unit);
    const auto before=room;
    auto wrong=notice;Put<uint32_t>(wrong.data(),6,1002);
    Check(sync.Process(c,0x0861,wrong.data()).reason==4 && room==before,"wrong owner is ignored without touching another participant");
    wrong=notice;Put<uint32_t>(wrong.data(),10,12);
    Check(sync.Process(c,0x0861,wrong.data()).reason==3 && room==before,"out-of-range slot is rejected");
    wrong=notice;Put<uint32_t>(wrong.data(),6,0x80000001u);
    Check(sync.Process(c,0x0861,wrong.data()).reason==3 && room==before,"AI is not rewritten as a human");
    Check(sync.Process(c,0x0861,notice.data()).applied==1,"valid full notification applies immediately");
    Check(Read<uint32_t>(profile.data(),0x1A9)==11032 && Read<uint64_t>(profile.data(),0x1B1)==0x123456789ABCuLL,"own profile model and 64-bit UID agree");
    std::array<uint8_t,0xCD4> start{};Put<uint16_t>(start.data(),0,0x06C3);
    Put<uint32_t>(start.data(),0x20,1001);Put<uint32_t>(start.data(),0x2E,11032);
    Put<uint32_t>(room.data(),0x57C,15019); // a pending old UI command ran late
    auto result=sync.Process(c,0x06C3,start.data());
    Check(result.applied==1 && !result.mismatched && Read<uint32_t>(room.data(),0x57C)==11032,"start boundary reapplies the full pending record before native construction");
    room[5]=7;std::array<uint8_t,0x2BC> ship{};
    Put<uint64_t>(ship.data(),0x1C,0x123456789ABCuLL);Put<uint64_t>(ship.data(),0x34,0x123456789ABCuLL);
    Put<uint32_t>(ship.data(),0x58,11032);
    auto deck=room_random_sync::Packet(0x098B,1001,0,7,ship);
    Check(sync.Process(c,0x098B,deck.data()).applied==1,"complete temporary ship applies");
    Check(std::memcmp(room.data()+0x679,ship.data(),ship.size())==0 && std::memcmp(profile.data()+0x2A6,ship.data(),ship.size())==0,"room and own profile retain the full ship header and records");
    Put<uint32_t>(room.data(),0,8);Put<uint32_t>(room.data(),0x57C,15019);
    result=sync.Process(c,0x06C3,start.data());
    Check(result.applied==0 && result.mismatched==1 && Read<uint32_t>(room.data(),0x57C)==15019,"a new room never reuses an old pending choice or fabricates a partial record");
    Put<uint32_t>(unit.data(),0,15019);Put<uint64_t>(unit.data(),8,777);
    auto restore=room_random_sync::Packet(0x0861,1001,0,8,unit);
    Check(sync.Process(c,0x0861,restore.data()).applied==1 && Read<uint32_t>(profile.data(),0x1A9)==15019,"restore notification returns the original selection");
    // A frozen pre-start RoomPilot is independent of the visible cache. This
    // was the missing regression: the visible cache alone already passed.
    std::array<uint8_t,kStride> frozen{};
    for(size_t i=0;i<frozen.size();++i)frozen[i]=static_cast<uint8_t>(i*17);
    Put<uint32_t>(frozen.data(),kOwner,1001);Put<uint32_t>(frozen.data(),0x49,15019);
    const auto original=frozen;
    CacheSync construction;
    Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x100000001ULL);
    Put<uint32_t>(unit.data(),0x52,765432); // full-width optional skill part
    notice=room_random_sync::Packet(0x0861,1001,0,8,unit);room[5]=8;
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0).applied,"no notice/start never rewrites a construction source");
    construction.Process(c,0x0861,notice.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0).applied,"a notice alone cannot authorize a later construction");
    construction.Process(c,0x06C3,start.data());
    auto prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0);
    Check(prepared.applied && prepared.before==15019 && prepared.after==11032,"frozen human source receives the authoritative model");
    auto expectedPilot=original;std::memcpy(expectedPilot.data()+0x49,unit.data(),unit.size());
    Check(prepared.pilot==expectedPilot,"whole active record including UID, skills and appearance is preserved; all other pilot bytes stay unchanged");
    Check(frozen==original,"frozen native source is never mutated");
    auto wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x20,1002);
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"another owner's source is rejected");
    wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x20,0x80000001u);
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"AI construction is unchanged");
    wrongEntry=start;wrongEntry[0x20+5]^=1;
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"changed start-entry metadata cannot reuse the previous authority");
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"deck semantics must match the room mode");
    // A second human has their own pending full record and UID.
    Put<uint32_t>(room.data(),kSlotBase+kStride+kOwner,1002);
    Put<uint32_t>(room.data(),kSlotBase+kStride+0x49,15019);
    auto secondUnit=unit;Put<uint64_t>(secondUnit.data(),8,0x200000002ULL);
    auto second=room_random_sync::Packet(0x0861,1002,1,8,secondUnit);
    construction.Process(c,0x0861,second.data());
    Put<uint32_t>(start.data(),0x20+0x10F,1002);Put<uint32_t>(start.data(),0x2E+0x10F,11032);
    construction.Process(c,0x06C3,start.data());
    Put<uint32_t>(frozen.data(),kOwner,1002);
    prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20+0x10F,0);
    Check(prepared.applied && Read<uint64_t>(prepared.pilot.data(),0x51)==0x200000002ULL,"two humans using the same model retain distinct inventory UIDs");
    Put<uint32_t>(frozen.data(),kOwner,1001);
    room[5]=7;ship={};
    for(size_t i=0;i<4;++i) {
        const auto id=static_cast<uint32_t>(11032+i);
        Put<uint32_t>(ship.data(),0x58+i*0x99,id);
        Put<uint64_t>(ship.data(),0x60+i*0x99,0x300000000ULL+i);
        Put<uint64_t>(ship.data(),0x34+i*8,0x300000000ULL+i);
        Put<uint32_t>(start.data(),0x2E+i*0x32,id);
    }
    Put<uint64_t>(ship.data(),0x1C,0x300000000ULL);
    construction.Process(c,0x06C3,start.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"deck construction requires the complete ship notice");
    deck=room_random_sync::Packet(0x098B,1001,0,8,ship);
    construction.Process(c,0x098B,deck.data());construction.Process(c,0x06C3,start.data());
    prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1);
    expectedPilot=frozen;std::memcpy(expectedPilot.data()+0x49,unit.data(),unit.size());std::memcpy(expectedPilot.data()+0x146,ship.data(),ship.size());
    Check(prepared.applied && prepared.pilot==expectedPilot,"complete deck keeps leader, four UIDs and all per-unit data");
    wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x2E+0x32,99999);
    construction.Process(c,0x06C3,wrongEntry.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,1).applied,"deck model mismatch is not corrected with a partial record");
    construction.Process(c,0x06C3,start.data());construction.Process(c,0x0861,restore.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"restoration invalidates the previous construction authorization");
    construction.Process(c,0x06C3,start.data());Put<uint32_t>(room.data(),0,9);
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"room identity change rejects a previous construction snapshot");
    {
        std::array<uint8_t,0x5DA8> lateProfile{};
        std::array<uint8_t,0x4000> lateRoom{};
        Context lateContext{lateProfile.data(),lateRoom.data(),2001};
        CacheSync late;
        Put<uint32_t>(lateRoom.data(),0,20);lateRoom[5]=8;
        std::array<std::array<uint8_t,0x99>,4> units{};
        std::array<std::array<uint8_t,kStride>,4> sources{};
        std::array<std::array<uint8_t,room_random_sync::kPayload+room_random_sync::kUnitSize>,4> notices{};
        std::array<uint8_t,0xCD4> lateStart{};Put<uint16_t>(lateStart.data(),0,0x06C3);
        for(size_t slot=0;slot<4;++slot) {
            const auto owner=static_cast<uint32_t>(2001+slot);
            const auto oldModel=static_cast<uint32_t>(15001+slot);
            const auto model=static_cast<uint32_t>(12001+slot);
            Put<uint32_t>(lateRoom.data(),kSlotBase+slot*kStride+kOwner,owner);
            Put<uint32_t>(lateRoom.data(),kSlotBase+slot*kStride+0x49,oldModel);
            Put<uint32_t>(units[slot].data(),0,model);
            Put<uint64_t>(units[slot].data(),8,0x400000000ULL+slot);
            notices[slot]=room_random_sync::Packet(0x0861,owner,static_cast<uint32_t>(slot),20,units[slot]);
            Put<uint32_t>(lateStart.data(),0x20+slot*0x10F,owner);
            Put<uint32_t>(lateStart.data(),0x2E+slot*0x10F,model);
            Put<uint32_t>(sources[slot].data(),kOwner,owner);
            Put<uint32_t>(sources[slot].data(),0x49,oldModel);
        }
        for(size_t slot=0;slot<3;++slot)
            Check(late.Process(lateContext,0x0861,notices[slot].data()).applied==1,
                  "three on-time human notices apply before the start");
        auto lateResult=late.Process(lateContext,0x06C3,lateStart.data());
        Check(lateResult.applied==3 && lateResult.mismatched==1,
              "start records one missing human notice without discarding the other three");
        auto rejected=notices[3];Put<uint32_t>(rejected.data(),6,9999);
        Check(late.Process(lateContext,0x0861,rejected.data()).reason==4,
              "wrong-owner notice is rejected after start");
        auto outOfRange=notices[3];Put<uint32_t>(outOfRange.data(),10,12);
        Check(late.Process(lateContext,0x0861,outOfRange.data()).reason==3,
              "out-of-range notice is rejected after start");
        for(size_t slot=0;slot<3;++slot)
            Check(late.PrepareConstruction(lateContext,sources[slot].data(),
                                           lateStart.data()+0x20+slot*0x10F,0).applied,
                  "rejected notice preserves every already-authorized human slot");
        Check(!late.PrepareConstruction(lateContext,sources[3].data(),
                                        lateStart.data()+0x20+3*0x10F,0).applied,
              "missing fourth notice cannot authorize a partial frozen source");
        Check(late.Process(lateContext,0x0861,notices[3].data()).applied==1,
              "matching fourth notice is accepted after start");
        for(size_t slot=0;slot<4;++slot) {
            const auto fixed=late.PrepareConstruction(
                lateContext,sources[slot].data(),lateStart.data()+0x20+slot*0x10F,0);
            Check(fixed.applied && fixed.after==12001+slot,
                  "late fourth notice completes its slot without revoking the first three");
        }
        auto nextUnit=units[1];Put<uint32_t>(nextUnit.data(),0,15002);
        const auto next=room_random_sync::Packet(0x0861,2002,1,20,nextUnit);
        Check(late.Process(lateContext,0x0861,next.data()).applied==1,
              "a later selection still updates its native slot");
        Check(!late.PrepareConstruction(lateContext,sources[1].data(),
                                        lateStart.data()+0x20+0x10F,0).applied,
              "a later selection invalidates only its own start authority");
        for(size_t slot : {size_t(0),size_t(2),size_t(3)})
            Check(late.PrepareConstruction(lateContext,sources[slot].data(),
                                           lateStart.data()+0x20+slot*0x10F,0).applied,
                  "one player's later selection preserves the other start authorities");
        Put<uint32_t>(lateRoom.data(),0,21);
        Check(late.Process(lateContext,0x0861,notices[0].data()).applied==1,
              "a notice in the next room generation still updates its cache");
        for(size_t slot=0;slot<4;++slot)
            Check(!late.PrepareConstruction(lateContext,sources[slot].data(),
                                            lateStart.data()+0x20+slot*0x10F,0).applied,
                  "room generation change rejects every previous start authority");
    }
    {
        std::array<uint8_t,0x5DA8> deckProfile{};
        std::array<uint8_t,0x4000> deckRoom{};
        Context deckContext{deckProfile.data(),deckRoom.data(),3001};
        Put<uint32_t>(deckRoom.data(),0,30);deckRoom[5]=7;
        Put<uint32_t>(deckRoom.data(),kSlotBase+kOwner,3001);
        Put<uint32_t>(deckRoom.data(),kSlotBase+0x49,16001);
        std::array<uint8_t,0x99> deckUnit{};
        Put<uint32_t>(deckUnit.data(),0,13001);Put<uint64_t>(deckUnit.data(),8,0x500000001ULL);
        std::array<uint8_t,0x2BC> deckNotice{};
        std::array<uint8_t,0xCD4> deckStart{};Put<uint16_t>(deckStart.data(),0,0x06C3);
        Put<uint32_t>(deckStart.data(),0x20,3001);
        for(size_t i=0;i<4;++i) {
            Put<uint32_t>(deckNotice.data(),0x58+i*0x99,static_cast<uint32_t>(13001+i));
            Put<uint64_t>(deckNotice.data(),0x60+i*0x99,0x500000001ULL+i);
            Put<uint32_t>(deckStart.data(),0x2E+i*0x32,static_cast<uint32_t>(13001+i));
        }
        Put<uint64_t>(deckNotice.data(),0x1C,0x500000001ULL);
        const auto unitPacket=room_random_sync::Packet(0x0861,3001,0,30,deckUnit);
        const auto shipPacket=room_random_sync::Packet(0x098B,3001,0,30,deckNotice);
        std::array<uint8_t,kStride> deckSource{};
        Put<uint32_t>(deckSource.data(),kOwner,3001);Put<uint32_t>(deckSource.data(),0x19E,16001);
        CacheSync shipFirst;
        auto deckResult=shipFirst.Process(deckContext,0x06C3,deckStart.data());
        Check(deckResult.applied==0 && deckResult.mismatched==1,
              "deck start waits when both full notices are late");
        Check(shipFirst.Process(deckContext,0x098B,shipPacket.data()).applied==1 &&
              !shipFirst.PrepareConstruction(deckContext,deckSource.data(),deckStart.data()+0x20,1).applied,
              "late ship notice alone keeps deck construction closed");
        Check(shipFirst.Process(deckContext,0x0861,unitPacket.data()).applied==1,
              "late unit notice completes a ship-first deck");
        auto deckPrepared=shipFirst.PrepareConstruction(
            deckContext,deckSource.data(),deckStart.data()+0x20,1);
        auto expectedDeck=deckSource;
        std::memcpy(expectedDeck.data()+0x49,deckUnit.data(),deckUnit.size());
        std::memcpy(expectedDeck.data()+0x146,deckNotice.data(),deckNotice.size());
        Check(deckPrepared.applied && deckPrepared.pilot==expectedDeck,
              "ship-first late notices preserve the complete deck source");
        Put<uint32_t>(deckRoom.data(),0,31);Put<uint32_t>(deckRoom.data(),kSlotBase+0x49,16001);
        CacheSync unitFirst;
        unitFirst.Process(deckContext,0x06C3,deckStart.data());
        Check(unitFirst.Process(deckContext,0x0861,unitPacket.data()).applied==1 &&
              !unitFirst.PrepareConstruction(deckContext,deckSource.data(),deckStart.data()+0x20,1).applied,
              "late unit notice alone keeps deck construction closed");
        Check(unitFirst.Process(deckContext,0x098B,shipPacket.data()).applied==1 &&
              unitFirst.PrepareConstruction(deckContext,deckSource.data(),deckStart.data()+0x20,1).applied,
              "unit-first and ship-first late deck notices both complete the same start");
    }
    std::printf("%d CLIENT CACHE POLICY CHECKS, %d failures\n",checks,failures);return failures?1:0;
}
