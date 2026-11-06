#pragma once
#include "combat_math.h"
#include <cstdint>
#include <limits>
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
    // Directed links are authored native map actions, not walking shortcuts.
    // The adapter validates the loaded trigger, trajectory and landing first.
    struct Transport {uint32_t id=0;Vec3 entry{},exit{};double cost=0;};
    struct Point {Vec3 position{};uint32_t transport=0;};
private:
    struct Key {int x,z,y;uint32_t transport=0,side=0;bool operator==(const Key& k)const {
        return x==k.x&&z==k.z&&y==k.y&&transport==k.transport&&side==k.side;}};
    struct Hash {size_t operator()(const Key& k)const {
        return size_t(uint32_t(k.x)*73856093u)^size_t(uint32_t(k.z)*19349663u)^size_t(uint32_t(k.y)*83492791u)^
            size_t(k.transport*2654435761u)^size_t(k.side*16777619u);
    }};
    struct Node {Vec3 point;int x,z;size_t parent;double cost;bool closed=false;uint32_t transport=0;};
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
    std::vector<Point> steps_,best_;
    std::vector<Transport> transports_;
    std::vector<double> tails_;
    double bestCost_=std::numeric_limits<double>::infinity();
    static double Distance(Vec3 a,Vec3 b) {return Length(a-b);}
    Key MakeKey(int x,int z,float y)const {return {x,z,int(std::floor(double(y)/32))};}
    std::vector<Point> TraceSteps(size_t at)const {
        std::vector<Point> path;
        while(at) {path.push_back({nodes_[at].point,nodes_[at].transport});at=nodes_[at].parent;}
        std::reverse(path.begin(),path.end());return path;
    }
    static std::vector<Vec3> Positions(const std::vector<Point>& steps) {
        std::vector<Vec3> result;result.reserve(steps.size());
        for(const auto& step:steps)result.push_back(step.position);
        return result;
    }
    double Heuristic(Vec3 point)const {
        double result=Distance(point,goal_);
        for(size_t i=0;i<transports_.size();++i)
            result=(std::min)(result,Distance(point,transports_[i].entry)+transports_[i].cost+tails_[i]);
        return result;
    }
    size_t Admit(Key key,Vec3 point,int x,int z,size_t parent,double cost,uint32_t transport=0) {
        auto found=indices_.find(key);
        if(found==indices_.end()) {
            if(nodes_.size()>=limit_)return SIZE_MAX;
            const size_t index=nodes_.size();nodes_.push_back({point,x,z,parent,cost,false,transport});
            indices_.emplace(key,index);open_.push({cost+Heuristic(point),cost,index});return index;
        }
        auto& node=nodes_[found->second];
        if(cost<node.cost) {
            node.point=point;node.parent=parent;node.cost=cost;node.closed=false;node.transport=transport;
            open_.push({cost+Heuristic(point),cost,found->second});
        }
        return found->second;
    }
    void Finish() {steps_=best_;path_=Positions(steps_);status_=Status::Found;}
    void Prune() {
        while(!open_.empty()&&(nodes_[open_.top().index].closed||nodes_[open_.top().index].cost!=open_.top().cost))open_.pop();
    }
public:
    void Begin(Vec3 start,Vec3 goal,float cell=100,size_t limit=8192,const std::vector<Transport>& transports={}) {
        *this=MapPathSearch{};
        if(!Finite(start)||!Finite(goal)||!std::isfinite(cell)||cell<25||cell>1000||limit<2||limit>65536)return;
        if(transports.size()>64)return;
        for(size_t i=0;i<transports.size();++i) {
            const auto& link=transports[i];
            if(!link.id||!Finite(link.entry)||!Finite(link.exit)||!std::isfinite(link.cost)||link.cost<=0)return;
            for(size_t j=0;j<i;++j)if(link.id==transports[j].id)return;
        }
        start_=start;goal_=goal;cell_=cell;limit_=limit;status_=Status::Pending;transports_=transports;
        // Euclidean distance alone overestimates routes using fast map links.
        // Relax their exit-to-goal lower bounds, including subsequent links.
        for(const auto& link:transports_)tails_.push_back(Distance(link.exit,goal));
        for(size_t pass=0;pass<transports_.size();++pass)
            for(size_t i=0;i<transports_.size();++i)
                for(size_t j=0;j<transports_.size();++j)
                    tails_[i]=(std::min)(tails_[i],Distance(transports_[i].exit,transports_[j].entry)+transports_[j].cost+tails_[j]);
        nodes_.push_back({start,0,0,0,0,false});indices_.emplace(MakeKey(0,0,start.y),0);
        open_.push({Heuristic(start),0,0});
    }
    Status Pump(StepFn step,void* context,size_t expansions=4,StepFn transportEntry=nullptr) {
        if(status_!=Status::Pending||!step)return status_;
        constexpr int offsets[8][2]={{1,0},{0,1},{-1,0},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        constexpr int longOffsets[8][2]={{2,0},{0,2},{-2,0},{0,-2},{3,0},{0,3},{-3,0},{0,-3}};
        for(size_t expanded=0;expanded<expansions;) {
            Prune();if(open_.empty()) {if(!best_.empty())Finish();else status_=Status::Unreachable;break;}
            if(!best_.empty()&&bestCost_<=open_.top().score+1e-6) {Finish();break;}
            const Entry entry=open_.top();open_.pop();
            nodes_[entry.index].closed=true;++expanded;
            const Node current=nodes_[entry.index];
            Vec3 end{};
            // Bound a shortcut's native query cost. Longer routes are built
            // through the same checked graph rather than bridging unseen ground.
            if(Distance(current.point,goal_)<=cell_*4&&step(context,current.point,goal_,end)&&Finite(end)&&
               std::abs(end.y-goal_.y)<=5) {
                const double cost=current.cost+Distance(current.point,end);
                if(cost<bestCost_) {bestCost_=cost;best_=TraceSteps(entry.index);best_.push_back({end,0});}
                if(cost<=entry.score+1e-6) {Finish();break;}
            }
            size_t admitted=0;
            const auto neighbor=[&](const int* offset) {
                const int x=current.x+offset[0],z=current.z+offset[1];
                Vec3 desired={start_.x+float(x)*cell_,current.point.y,start_.z+float(z)*cell_},point{};
                if(!Finite(desired)||!step(context,current.point,desired,point)||!Finite(point))return;
                ++admitted;
                const Key key=MakeKey(x,z,point.y);
                const double cost=current.cost+Length(point-current.point);
                Admit(key,point,x,z,entry.index,cost);
            };
            for(const auto& offset:offsets)neighbor(offset);
            // A ledge may need a longer checked edge so the whole native body
            // clears its support before falling. These are ordinary graph edges.
            if(admitted<8)for(const auto& offset:longOffsets)neighbor(offset);
            for(const auto& link:transports_) {
                const double approach=Distance(current.point,link.entry);
                if(approach>cell_*4)continue;
                Vec3 grounded=link.entry;
                const auto approachStep=transportEntry?transportEntry:step;
                if(approach>1&&(!approachStep(context,current.point,link.entry,grounded)||!Finite(grounded)||
                               Distance(grounded,link.entry)>5))continue;
                const int x=static_cast<int>(std::round((link.entry.x-start_.x)/cell_));
                const int z=static_cast<int>(std::round((link.entry.z-start_.z)/cell_));
                auto key=MakeKey(x,z,link.entry.y);key.transport=link.id;key.side=1;
                size_t parent=entry.index;
                if(approach>1)parent=Admit(key,link.entry,x,z,entry.index,current.cost+approach);
                if(parent==SIZE_MAX)continue;
                const int ex=static_cast<int>(std::round((link.exit.x-start_.x)/cell_));
                const int ez=static_cast<int>(std::round((link.exit.z-start_.z)/cell_));
                key=MakeKey(ex,ez,link.exit.y);key.transport=link.id;key.side=2;
                Admit(key,link.exit,ex,ez,parent,nodes_[parent].cost+link.cost,link.id);
            }
        }
        Prune();if(status_==Status::Pending&&!best_.empty()&&(open_.empty()||bestCost_<=open_.top().score+1e-6))Finish();
        return status_;
    }
    Status State()const {return status_;}
    const std::vector<Vec3>& Path()const {return path_;}
    const std::vector<Point>& Steps()const {return steps_;}
    // A validated frontier prefix lets the native motor make useful progress
    // while later expansions continue. It is never a fabricated displacement.
    std::vector<Vec3> Prefix() {
        return Positions(PrefixSteps());
    }
    std::vector<Point> PrefixSteps() {Prune();return open_.empty()?std::vector<Point>{}:TraceSteps(open_.top().index);}
    const std::vector<Transport>& Transports()const {return transports_;}
    size_t Nodes()const {return nodes_.size();}
    Vec3 Goal()const {return goal_;}
    Vec3 Start()const {return start_;}
};
}
