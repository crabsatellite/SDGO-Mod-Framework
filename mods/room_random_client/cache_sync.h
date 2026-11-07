#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace room_random_client {
constexpr size_t kSlots=12,kStride=0x48E,kSlotBase=0x533,kOwner=0x429;
template<class T> T Read(const uint8_t* p,size_t at) {T v;std::memcpy(&v,p+at,sizeof(v));return v;}
struct SlotAccess {
    using Owner=uint32_t(__thiscall*)(uint8_t*,int);
    using Get=uint8_t*(__thiscall*)(uint8_t*,int);
    using Set=void(__thiscall*)(uint8_t*,int,const uint8_t*);
    Owner owner=nullptr;Get active=nullptr,ship=nullptr;Set setActive=nullptr,setShip=nullptr;
};
struct Context {
    uint8_t* profile=nullptr;uint8_t* room=nullptr;uint32_t local=0;unsigned source=0;
    uint8_t* model=nullptr;SlotAccess slots;uint8_t* manager=nullptr;
    uint32_t Owner(size_t slot) const {return model ? slots.owner(model,static_cast<int>(slot)) : Read<uint32_t>(room,kSlotBase+slot*kStride+kOwner);}
    uint8_t* Active(size_t slot) const {return model ? slots.active(model,static_cast<int>(slot)) : room+kSlotBase+slot*kStride+0x49;}
};
struct Result {unsigned applied=0,mismatched=0,reason=0,source=0;uint32_t owner=0,model=0,cached=0;};
struct Construction {
    std::array<uint8_t,kStride> pilot{};
    uint32_t owner=0,before=0,after=0;
    unsigned reason=0;
    bool applied=false;
};
class CacheSync {
    struct Pending {
        uint32_t owner=0;
        bool unit=false,ship=false;
        std::array<uint8_t,0x99> active{};
        std::array<uint8_t,0x2BC> deck{};
    };
    uint8_t* room_=nullptr;
    uint32_t key_=0;
    uint8_t* model_=nullptr;
    uint8_t* manager_=nullptr;
    std::array<Pending,kSlots> pending_{};
    std::array<uint32_t,kSlots> expected_{};
    std::array<std::array<uint8_t,0x10F>,kSlots> starts_{};
    std::array<bool,kSlots> constructing_{};
    std::array<bool,kSlots> humans_{};
    std::array<bool,kSlots> invalid_{};
    std::array<uint8_t,kSlots> awaiting_{};
    bool startSeen_=false;
    static constexpr uint8_t kAwaitUnit=1,kAwaitShip=2;
    bool Deck(const Context& c) const {return c.room[5]==6 || c.room[5]==7;}
    bool SameContext(const Context& c) const {
        return c.profile && c.room && c.local && c.room==room_ &&
            c.model==model_ && c.manager==manager_ &&
            Read<uint32_t>(c.room,0)==key_;
    }
    bool UnitMatches(size_t slot,const Pending& p) const {
        return expected_[slot] && p.unit &&
            p.owner==Read<uint32_t>(starts_[slot].data(),0) &&
            Read<uint32_t>(p.active.data(),0)==expected_[slot];
    }
    bool ShipMatches(size_t slot,const Pending& p) const {
        if(!p.ship)return false;
        for(size_t i=0;i<4;++i)
            if(Read<uint32_t>(p.deck.data(),0x58+i*0x99)!=
               Read<uint32_t>(starts_[slot].data(),0x0E+i*0x32))return false;
        return true;
    }
    bool PacketMatches(size_t slot,uint16_t opcode,const uint8_t* packet,
                       const Context& c) const {
        if(!expected_[slot] ||
           Read<uint32_t>(starts_[slot].data(),0)!=Read<uint32_t>(packet,6))return false;
        if(opcode==0x0861)return Read<uint32_t>(packet,0x12)==expected_[slot];
        if(!Deck(c))return true;
        for(size_t i=0;i<4;++i)
            if(Read<uint32_t>(packet,0x12+0x58+i*0x99)!=
               Read<uint32_t>(starts_[slot].data(),0x0E+i*0x32))return false;
        return true;
    }
    void Invalidate(size_t slot) {
        expected_[slot]=0;constructing_[slot]=false;awaiting_[slot]=0;
        if(humans_[slot])invalid_[slot]=true;
    }
    void Refresh(const Context& c,size_t slot,const Pending& p) {
        constructing_[slot]=expected_[slot] && !awaiting_[slot] &&
            UnitMatches(slot,p) && (!Deck(c) || ShipMatches(slot,p));
    }
    void Unit(const Context& c,size_t slot,Pending& p) {
        if(c.model) c.slots.setActive(c.model,static_cast<int>(slot),p.active.data());
        else std::memcpy(c.room+kSlotBase+slot*kStride+0x49,p.active.data(),p.active.size());
        if(p.owner==c.local) std::memcpy(c.profile+0x1A9,p.active.data(),p.active.size());
    }
    void Ship(const Context& c,size_t slot,Pending& p) {
        if(c.model) c.slots.setShip(c.model,static_cast<int>(slot),p.deck.data());
        else std::memcpy(c.room+kSlotBase+slot*kStride+0x146,p.deck.data(),p.deck.size());
        if(p.owner==c.local) std::memcpy(c.profile+0x2A6,p.deck.data(),p.deck.size());
    }
public:
    // This is only a candidate captured from a verified native packet context.
    // The caller must resolve its complete live vtable/room identity again.
    uint8_t* ObservedModel() const {return model_;}
    uint8_t* ObservedManager() const {return manager_;}
    Result Process(const Context& c,uint16_t opcode,const uint8_t* packet,
                   bool orderedStart=false) {
        Result result;result.source=c.source;
        if(!c.profile || !c.room || !c.local || !packet) {result.reason=1;return result;}
        if(Read<uint16_t>(packet,0)!=opcode) {result.reason=2;return result;}
        const uint32_t key=Read<uint32_t>(c.room,0);
        if(c.room!=room_ || key!=key_ || c.model!=model_ ||
           c.manager!=manager_) {
            room_=c.room;key_=key;model_=c.model;manager_=c.manager;
            pending_={};expected_={};
            starts_={};constructing_={};humans_={};invalid_={};
            awaiting_={};startSeen_=false;
        }
        if(opcode==0x0861 || opcode==0x098B) {
            const uint32_t slot=Read<uint32_t>(packet,10),owner=Read<uint32_t>(packet,6);
            if(slot>=kSlots || !owner || (owner&0x80000000u)) {result.reason=3;return result;}
            if(c.Owner(slot)!=owner || !c.Active(slot)) {result.reason=4;return result;}
            const uint8_t component=opcode==0x0861 ? kAwaitUnit : kAwaitShip;
            auto& p=pending_[slot];
            if(expected_[slot]) {
                const bool duplicate=opcode==0x0861 ?
                    (p.owner==owner && p.unit &&
                     std::memcmp(p.active.data(),packet+0x12,p.active.size())==0) :
                    (p.owner==owner && p.ship &&
                     std::memcmp(p.deck.data(),packet+0x12,p.deck.size())==0);
                if(!PacketMatches(slot,opcode,packet,c) ||
                   (!(awaiting_[slot]&component) && !duplicate))
                    Invalidate(slot);
                else
                    awaiting_[slot]&=static_cast<uint8_t>(~component);
            }
            if(p.owner!=owner) p={};
            p.owner=owner;
            if(opcode==0x0861) {
                std::memcpy(p.active.data(),packet+0x12,p.active.size());p.unit=true;
                Unit(c,slot,p);result.model=Read<uint32_t>(p.active.data(),0);
            } else {
                std::memcpy(p.deck.data(),packet+0x12,p.deck.size());p.ship=true;
                Ship(c,slot,p);result.model=Read<uint32_t>(p.deck.data(),0x58);
            }
            Refresh(c,slot,p);
            result.applied=1;result.owner=owner;
        } else if(opcode==0x06C3) {
            expected_={};starts_={};constructing_={};humans_={};
            invalid_={};awaiting_={};startSeen_=true;
            for(size_t slot=0;slot<kSlots;++slot) {
                const auto* entry=packet+0x20+slot*0x10F;
                const uint32_t owner=Read<uint32_t>(entry,0),model=Read<uint32_t>(entry,0x0E);
                if(!owner || (owner&0x80000000u)) continue;
                humans_[slot]=true;
                std::memcpy(starts_[slot].data(),entry,starts_[slot].size());
                if(orderedStart) {
                    // Server 0.1.8 submits 06C3 before this round's records.
                    // A matching model from the previous round is therefore
                    // stale even when its owner and model ID are unchanged.
                    // Retain the slot owner/context, but require fresh full
                    // unit/ship candidates following this exact start.
                    auto& stale=pending_[slot];
                    stale.unit=false;stale.ship=false;
                    stale.active={};stale.deck={};
                }
                if(!model || c.Owner(slot)!=owner || !c.Active(slot)) {
                    invalid_[slot]=true;++result.mismatched;
                    result.owner=owner;result.model=model;
                    continue;
                }
                expected_[slot]=model;
                auto& p=pending_[slot];
                const bool unit=UnitMatches(slot,p);
                const bool ship=!Deck(c) || ShipMatches(slot,p);
                if(!unit)awaiting_[slot]|=kAwaitUnit;
                if(!ship)awaiting_[slot]|=kAwaitShip;
                if(unit) {
                    if(ship && Deck(c)) Ship(c,slot,p);
                    Unit(c,slot,p);++result.applied;
                }
                Refresh(c,slot,p);
                const auto cached=Read<uint32_t>(c.Active(slot),0);
                if(cached!=model) {++result.mismatched;result.owner=owner;result.model=model;result.cached=cached;}
                else if(!result.mismatched) {result.owner=owner;result.model=model;result.cached=cached;}
            }
        }
        return result;
    }
    bool ReadyForStart(const Context& c,const uint8_t* packet) const {
        if(!packet || !startSeen_ || !SameContext(c) ||
           Read<uint16_t>(packet,0)!=0x06C3) return false;
        bool anyHuman=false;
        for(size_t slot=0;slot<kSlots;++slot) {
            const auto* entry=packet+0x20+slot*0x10F;
            const uint32_t owner=Read<uint32_t>(entry,0);
            if(!owner || (owner&0x80000000u)) {
                if(humans_[slot])return false;
                continue;
            }
            anyHuman=true;
            const auto& p=pending_[slot];
            if(!humans_[slot] || invalid_[slot] || !constructing_[slot] ||
               !expected_[slot] || p.owner!=owner || c.Owner(slot)!=owner ||
               !c.Active(slot) ||
               std::memcmp(entry,starts_[slot].data(),starts_[slot].size())!=0 ||
               !UnitMatches(slot,p) || (Deck(c) && !ShipMatches(slot,p)))
                return false;
        }
        return anyHuman;
    }
    // Mission construction reads a normalized snapshot captured before start,
    // independently of the visible room cache. Give the native converter a
    // complete private source; never edit its snapshot or a global factory ID.
    Construction PrepareConstruction(const Context& c,const uint8_t* source,
                                     const uint8_t* entry,uint8_t deck) const {
        Construction out;
        if(!source || !entry) {out.reason=1;return out;}
        out.owner=Read<uint32_t>(entry,0);
        if(!out.owner || (out.owner&0x80000000u)) return out;
        if(!c.profile || !c.room || c.room!=room_ ||
           c.model!=model_ || c.manager!=manager_ ||
           Read<uint32_t>(c.room,0)!=key_) {out.reason=1;return out;}
        if(Read<uint32_t>(source,kOwner)!=out.owner) {out.reason=2;return out;}
        if(deck>1 || bool(deck)!=(c.room[5]==6 || c.room[5]==7)) {out.reason=3;return out;}
        for(size_t slot=0;slot<kSlots;++slot) {
            const auto& p=pending_[slot];
            if(!constructing_[slot] || p.owner!=out.owner || c.Owner(slot)!=out.owner)continue;
            if(!p.unit || expected_[slot]!=Read<uint32_t>(p.active.data(),0) ||
               std::memcmp(entry,starts_[slot].data(),starts_[slot].size())!=0) {out.reason=4;return out;}
            if(deck) {
                if(!p.ship) {out.reason=5;return out;}
                for(size_t i=0;i<4;++i) if(Read<uint32_t>(p.deck.data(),0x58+i*0x99)!=
                    Read<uint32_t>(entry,0x0E+i*0x32)) {out.reason=6;return out;}
            }
            std::memcpy(out.pilot.data(),source,out.pilot.size());
            out.before=Read<uint32_t>(source,deck ? 0x19E : 0x49);
            std::memcpy(out.pilot.data()+0x49,p.active.data(),p.active.size());
            if(deck)std::memcpy(out.pilot.data()+0x146,p.deck.data(),p.deck.size());
            out.after=Read<uint32_t>(out.pilot.data(),deck ? 0x19E : 0x49);
            out.applied=true;return out;
        }
        return out;
    }
};
}
