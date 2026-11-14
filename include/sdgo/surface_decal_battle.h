#pragma once
#include "sdgo/surface_decal.h"
#include <map>

namespace sdgo::surface_decal {
// The native human converter retains the inventory identity before projecting
// the packed record into the smaller battle appearance. No model-ID lookup or
// nearest-match selection is permitted: a participant and exact deck slot are
// required, including when two owned units have identical cosmetics.
class BattleBindings {
public:
    void Clear(){pilots_.clear();styles_.clear();}
    void ForgetStyle(std::uintptr_t colours){styles_.erase(colours);}
    void Replace(std::uint32_t owner,const std::array<Appearance,4>& slots,unsigned count){
        for(auto i=styles_.begin();i!=styles_.end();)if(i->second.owner==owner)i=styles_.erase(i);else ++i;
        pilots_.erase(owner);
        if(!owner||(owner&0x80000000u)||(count!=1&&count!=4))return;
        Pilot p;p.slots=slots;p.count=count;
        for(unsigned i=0;i<count;++i)if(slots[i].unitSerial&&!Valid(slots[i]))return;
        if(pilots_.size()>=12)Clear();
        pilots_[owner]=p;
    }
    bool Prepare(std::uint32_t owner,unsigned slot,std::uint32_t type,
                 const std::array<std::uint32_t,3>& stickers,std::uintptr_t colours){
        styles_.erase(colours);
        const auto p=pilots_.find(owner);
        if(!colours||p==pilots_.end()||slot>=p->second.count)return false;
        const auto& a=p->second.slots[slot];if(!Valid(a)||a.unitType!=type)return false;
        for(unsigned i=0;i<3;++i)if(a.slots[i].sticker!=stickers[i])return false;
        if(styles_.size()>=64)styles_.clear();
        styles_[colours]={owner,a};return true;
    }
    bool Resolve(std::uintptr_t colours,Appearance& out)const{
        const auto found=styles_.find(colours);if(found==styles_.end())return false;
        out=found->second.appearance;return true;
    }
private:
    struct Pilot{std::array<Appearance,4> slots{};unsigned count=0;};
    struct Style{std::uint32_t owner;Appearance appearance;};
    std::map<std::uint32_t,Pilot> pilots_;
    std::map<std::uintptr_t,Style> styles_;
};
}
