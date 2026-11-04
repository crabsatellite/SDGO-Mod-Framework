#pragma once
#include "combat_math.h"
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <vector>

namespace battle_ai {

// Incremental A* over grounded, clearance-checked edges. Geometry is supplied
// by the native game-thread adapter; combatants are not a search input.
class MapPathSearch {
public:
    enum class Status { Pending, Found, Unreachable };
    using StepFn=bool(*)(void*,Vec3 from,Vec3 desired,Vec3& grounded);
private:
    struct Key {int x,z,y;bool operator==(const Key& k)const {return x==k.x&&z==k.z&&y==k.y;}};
    struct Hash {size_t operator()(const Key& k)const {
        return size_t(uint32_t(k.x)*73856093u)^size_t(uint32_t(k.z)*19349663u)^size_t(uint32_t(k.y)*83492791u);
    }};
    struct Node {Vec3 point;int x,z;size_t parent;double cost;bool closed=false;};
    struct Entry {double score,cost;size_t index;bool operator<(const Entry& e)const {
        return score!=e.score?score>e.score:index>e.index;
    }};
    std::vector<Node> nodes_;
    std::unordered_map<Key,size_t,Hash> indices_;
    std::priority_queue<Entry> open_;
    Vec3 start_{},goal_{};
    float cell_=100;
    size_t limit_=8192;
    Status status_=Status::Unreachable;
    std::vector<Vec3> path_;
    static double Distance(Vec3 a,Vec3 b) {const double x=a.x-b.x,z=a.z-b.z;return std::sqrt(x*x+z*z);}
    Key MakeKey(int x,int z,float y)const {return {x,z,int(std::floor(double(y)/32))};}
    std::vector<Vec3> Trace(size_t at)const {
        std::vector<Vec3> path;
        while(at) {path.push_back(nodes_[at].point);at=nodes_[at].parent;}
        std::reverse(path.begin(),path.end());return path;
    }
    void Prune() {
        while(!open_.empty()&&(nodes_[open_.top().index].closed||nodes_[open_.top().index].cost!=open_.top().cost))open_.pop();
    }
public:
    void Begin(Vec3 start,Vec3 goal,float cell=100,size_t limit=8192) {
        *this=MapPathSearch{};
        if(!Finite(start)||!Finite(goal)||!std::isfinite(cell)||cell<25||cell>1000||limit<2||limit>65536)return;
        start_=start;goal_=goal;cell_=cell;limit_=limit;status_=Status::Pending;
        nodes_.push_back({start,0,0,0,0,false});indices_.emplace(MakeKey(0,0,start.y),0);
        open_.push({Distance(start,goal),0,0});
    }
    Status Pump(StepFn step,void* context,size_t expansions=4) {
        if(status_!=Status::Pending||!step)return status_;
        constexpr int offsets[8][2]={{1,0},{0,1},{-1,0},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        constexpr int longOffsets[8][2]={{2,0},{0,2},{-2,0},{0,-2},{3,0},{0,3},{-3,0},{0,-3}};
        for(size_t expanded=0;expanded<expansions;) {
            Prune();if(open_.empty()) {status_=Status::Unreachable;break;}
            const Entry entry=open_.top();open_.pop();
            nodes_[entry.index].closed=true;++expanded;
            const Node current=nodes_[entry.index];
            Vec3 end{};
            // Bound a shortcut's native query cost. Longer routes are built
            // through the same checked graph rather than bridging unseen ground.
            if(Distance(current.point,goal_)<=cell_*4&&step(context,current.point,goal_,end)&&Finite(end)) {
                path_=Trace(entry.index);path_.push_back(end);status_=Status::Found;break;
            }
            size_t admitted=0;
            const auto neighbor=[&](const int* offset) {
                const int x=current.x+offset[0],z=current.z+offset[1];
                Vec3 desired={start_.x+float(x)*cell_,current.point.y,start_.z+float(z)*cell_},point{};
                if(!Finite(desired)||!step(context,current.point,desired,point)||!Finite(point))return;
                ++admitted;
                const Key key=MakeKey(x,z,point.y);
                const double cost=current.cost+Length(point-current.point);
                auto found=indices_.find(key);
                if(found==indices_.end()) {
                    if(nodes_.size()>=limit_)return;
                    const size_t index=nodes_.size();nodes_.push_back({point,x,z,entry.index,cost,false});
                    indices_.emplace(key,index);open_.push({cost+Distance(point,goal_),cost,index});
                } else {
                    auto& node=nodes_[found->second];
                    if(cost>=node.cost)return;
                    node.point=point;node.parent=entry.index;node.cost=cost;node.closed=false;
                    open_.push({cost+Distance(point,goal_),cost,found->second});
                }
            };
            for(const auto& offset:offsets)neighbor(offset);
            // A ledge may need a longer checked edge so the whole native body
            // clears its support before falling. These are ordinary graph edges.
            if(admitted<8)for(const auto& offset:longOffsets)neighbor(offset);
        }
        return status_;
    }
    Status State()const {return status_;}
    const std::vector<Vec3>& Path()const {return path_;}
    // A validated frontier prefix lets the native motor make useful progress
    // while later expansions continue. It is never a fabricated displacement.
    std::vector<Vec3> Prefix() {
        Prune();return open_.empty()?std::vector<Vec3>{}:Trace(open_.top().index);
    }
    size_t Nodes()const {return nodes_.size();}
    Vec3 Goal()const {return goal_;}
    Vec3 Start()const {return start_;}
};
}
