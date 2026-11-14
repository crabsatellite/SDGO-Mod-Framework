#include "sdgo/unit_form_models.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <set>

using namespace sdgo::unit_form_models;
int main(){
    const std::vector<Pair> deck{{1,2},{20,0}};
    // Repeated N-form hops, forward/cyclic edges, and a respawn into either
    // original orientation must each retain exactly one owner per model.
    for(auto live:std::vector<Pair>{{1,2},{2,1},{2,3},{3,4},{4,5},{5,1},{3,5}}){
        auto plan=Plan(deck,0,live,{3,4,5});assert(plan.result!=Result::refused);
        std::set<std::uintptr_t> freed{live.base,live.alternate};
        for(auto model:plan.detached)assert(freed.insert(model).second);
        assert((freed==std::set<std::uintptr_t>{1,2,3,4,5}));
        assert(!freed.count(20)); // The other deck keeps its original owner.
    }
    assert(Plan(deck,0,{1,2},{}).result==Result::unchanged);
    assert(Plan(deck,0,{3,4},{3,4}).result==Result::handed_over);
    assert(Plan(deck,0,{1,20},{3}).result==Result::refused);
    assert(Plan(deck,0,{3,3},{3}).result==Result::refused);
    assert(Plan(deck,0,{3,4},{3,3,4}).result==Result::refused);
    assert(Plan(deck,0,{1,3},{1,3}).result==Result::refused);
    assert(Plan({{1,2},{2,3}},0,{1,2},{}).result==Result::refused);
    assert(Plan(deck,2,{1,2},{}).result==Result::refused);
    assert(Plan({},0,{1,2},{}).result==Result::refused);
    assert(Plan(deck,0,{0,2},{}).result==Result::refused);
    assert(Plan({{1,2},{0,0}},0,{2,3},{3}).result==Result::handed_over);
    assert(Plan({{0,0},{1,2}},1,{3,1},{3}).result==Result::handed_over);
    assert(Plan({{0,2}},0,{1,2},{1}).result==Result::refused);
    assert(Plan(deck,0,{1,2},std::vector<std::uintptr_t>(kMaxCachedModels+1,3)).result==Result::refused);
    for(std::uintptr_t count:{3u,4u,8u,64u}){
        std::vector<std::uintptr_t> cached;
        for(std::uintptr_t model=3;model<=count;++model)cached.push_back(model);
        for(std::uintptr_t current=1;current<=count;++current){
            const auto alternate=current%count+1;
            auto plan=Plan({{1,2},{0,0}},0,{current,alternate},cached);
            assert(plan.result!=Result::refused);assert(plan.detached.size()==count-2);
            std::set<std::uintptr_t> freed{current,alternate};
            for(auto model:plan.detached)assert(freed.insert(model).second);
            assert(freed.size()==count);
            for(std::uintptr_t model=1;model<=count;++model)assert(freed.count(model));
        }
    }
    return 0;
}
