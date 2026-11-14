#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace sdgo::unit_form_models {
constexpr std::size_t kMaxDeckEntries=64,kMaxCachedModels=512;
struct Pair { std::uintptr_t base=0,alternate=0; };
enum class Result { refused,unchanged,handed_over };
struct Cleanup { Result result=Result::refused;std::vector<std::uintptr_t> detached; };

// The original destructor skips the selected deck entry when its base is a live
// model, then releases the two live models itself. Detach every remaining owner
// only after controllers and the original unit have completed destruction.
inline Cleanup Plan(const std::vector<Pair>& deck,std::size_t selected,Pair live,
                    const std::vector<std::uintptr_t>& cached){
    if(deck.empty()||deck.size()>kMaxDeckEntries||selected>=deck.size()||
       cached.size()>kMaxCachedModels||!live.base||!live.alternate||live.base==live.alternate)return {};
    std::set<std::uintptr_t> owners,active;
    for(std::size_t i=0;i<deck.size();++i){
        // An unfilled battleship slot can have no models. It has no owner to
        // release; the active slot must still name its original base model.
        if(!deck[i].base&&(deck[i].alternate||i==selected))return {};
        for(auto model:{deck[i].base,deck[i].alternate}){
            if(!model)continue;
            if(!owners.insert(model).second)return {};
            if(i==selected)active.insert(model);
        }
    }
    for(auto model:cached){
        if(!model||!owners.insert(model).second)return {};
        active.insert(model);
    }
    if(!active.count(live.base)||!active.count(live.alternate))return {};
    Cleanup output;
    output.result=(deck[selected].base==live.base&&deck[selected].alternate==live.alternate)||
                  (deck[selected].alternate==live.base&&deck[selected].base==live.alternate)
                  ?Result::unchanged:Result::handed_over;
    for(auto model:active)if(model!=live.base&&model!=live.alternate)output.detached.push_back(model);
    return output;
}
} // namespace sdgo::unit_form_models
