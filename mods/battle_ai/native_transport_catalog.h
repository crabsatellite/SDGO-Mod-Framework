#pragma once
#include "native_map_transport.h"
#include <algorithm>
#include <vector>

namespace battle_ai::native {

inline constexpr uint32_t kTransportRequestAssociationWindowMs=200;

struct TransportRequestStatus {
    bool available=false,outstanding=false;
    uint32_t generation=0,outstandingGeneration=0;
    uint32_t requestCount=0,noticeCount=0,timeoutCount=0;
    uint32_t sendFailureCount=0,ownershipLossCount=0;
    // When a V2 bot record is available, association is fail-closed: overlap
    // is no longer evidence for a different or stale payload.
    bool exactWitness=false,exactPayload=false;
    bool exactFailure=false;
    uint32_t snapshotTick=0,requestTick=0;
    bool payloadValid=false,padObjectValid=false;
    uint32_t padObjectId=0,selectedPadObjectId=0,requestPilotId=0;
    float requestSpeed=0,requestDurationUpdates=0;
    Vec3 requestDirection{};
};

struct TransportRequestLease {
    bool baseline=false,committed=false,acknowledged=false;
    bool selectedOverlapObserved=false;
    uint32_t generation=0;
    uint32_t requestBaseline=0,noticeBaseline=0,timeoutBaseline=0;
    uint32_t sendFailureBaseline=0,ownershipLossBaseline=0;
    uint32_t requestCount=0,requestTick=0;
    uint64_t selectedOverlapAt=0,committedAt=0;
};

enum class TransportRequestUpdate : uint8_t {
    Unavailable,
    Idle,
    Committed,
    Acknowledged,
    Failed,
    GenerationChanged
};

inline void BeginTransportRequestLease(
    TransportRequestLease& lease,
    const TransportRequestStatus& status) noexcept {
    lease={};
    if(!status.available)return;
    lease.baseline=true;
    lease.generation=status.generation;
    lease.requestBaseline=status.requestCount;
    lease.noticeBaseline=status.noticeCount;
    lease.timeoutBaseline=status.timeoutCount;
    lease.sendFailureBaseline=status.sendFailureCount;
    lease.ownershipLossBaseline=status.ownershipLossCount;
}

inline bool TransportRequestAdmissionOpen(
    const TransportRequestLease& lease,uint64_t now) noexcept {
    return lease.selectedOverlapObserved&&
        now>=lease.selectedOverlapAt&&
        now-lease.selectedOverlapAt<=kTransportRequestAssociationWindowMs;
}

// A request belongs to a planned route only when a new successful native
// submission appears while the bot is overlapping that route's selected pad.
// The bridge permits one outstanding request per bot generation, so later
// notice/timeout counters remain tied to that admitted submission.
inline TransportRequestUpdate ObserveTransportRequestLease(
    TransportRequestLease& lease,
    const TransportRequestStatus& status,
    bool selectedPadOverlap,uint64_t now) noexcept {
    if(!status.available)return TransportRequestUpdate::Unavailable;
    if(!lease.baseline) {
        BeginTransportRequestLease(lease,status);
        return TransportRequestUpdate::Idle;
    }
    if(status.generation!=lease.generation)
        return TransportRequestUpdate::GenerationChanged;
    if(selectedPadOverlap&&!lease.selectedOverlapObserved) {
        lease.selectedOverlapObserved=true;
        lease.selectedOverlapAt=now;
    }
    const bool failed=status.timeoutCount>lease.timeoutBaseline||
        status.sendFailureCount>lease.sendFailureBaseline||
        status.ownershipLossCount>lease.ownershipLossBaseline;
    const bool exactPayload=status.exactWitness&&status.exactPayload&&
        uint32_t(status.snapshotTick-status.requestTick)<=
            kMapTransportRequestTimeoutMs;
    if(!lease.committed) {
        if(failed)return TransportRequestUpdate::Idle;
        const bool exactOutstanding=status.outstanding&&
            status.outstandingGeneration==status.generation;
        const bool acknowledged=status.noticeCount>lease.noticeBaseline;
        if(exactPayload&&status.exactFailure&&
           status.requestCount>lease.requestBaseline)
            return TransportRequestUpdate::Failed;
        const bool selected=status.exactWitness?exactPayload:
            (selectedPadOverlap||
             TransportRequestAdmissionOpen(lease,now));
        if(!selected||status.requestCount<=lease.requestBaseline||
           (!exactOutstanding&&!acknowledged))
            return TransportRequestUpdate::Idle;
        lease.committed=true;
        lease.acknowledged=acknowledged;
        lease.requestCount=status.requestCount;
        lease.requestTick=status.requestTick;
        lease.committedAt=now;
    }
    if(status.exactWitness&&
       (!exactPayload||
        status.requestCount!=lease.requestCount||
        status.requestTick!=lease.requestTick))
        return lease.acknowledged?
            TransportRequestUpdate::Acknowledged:
            TransportRequestUpdate::Committed;
    if(failed||status.exactFailure)return TransportRequestUpdate::Failed;
    if(status.noticeCount>lease.noticeBaseline) {
        lease.acknowledged=true;
        return TransportRequestUpdate::Acknowledged;
    }
    return lease.acknowledged?TransportRequestUpdate::Acknowledged:
        TransportRequestUpdate::Committed;
}

// Public loaded map geometry only. This reader does not inspect combatants,
// submit a packet, change a coordinate or grant native movement authority.
struct TransportVolume {
    uint32_t id=0;
    uint8_t* object=nullptr;
    uint8_t* scene=nullptr;
    Aabb bounds{};
    Vec3 position{},direction{};
    float speed=0,updates=0;
    static bool Read(uint8_t* scene,TransportVolume& output) noexcept {
        using namespace map_transport_detail;
        TransportVolume result{};result.scene=scene;
        uint8_t removed=1;uint8_t* registered=nullptr;
        if(!scene||!IsKind(scene,5,kCollisionSceneKind)||
           !detail::ReadPointer(scene,kSceneObject,result.object)||
           !IsKind(result.object,1,kForcedMoveObjectKind)||
           !detail::Read(result.object,0x1C,removed)||removed||
           !detail::ReadPointer(result.object,kObjectScene,registered)||registered!=scene||
           !detail::Read(scene,kSceneBounds,result.bounds)||!detail::ValidAabb(result.bounds)||
           !detail::Read(scene,0x88,result.position)||!Finite(result.position)||
           !detail::Read(scene,kSceneForward,result.direction)||
           !detail::Read(result.object,kPadSpeed,result.speed)||
           !detail::Read(result.object,kPadDuration,result.updates)||
           !ValidPayload(kAuthoredMapTransportBounds,result.speed,result.updates,result.direction))return false;
        output=result;return true;
    }
    bool Live()const noexcept {
        TransportVolume now{};
        return Read(scene,now)&&now.object==object&&Length(now.position-position)<0.01&&
            Length(now.direction-direction)<0.0001&&std::abs(now.speed-speed)<0.0001&&std::abs(now.updates-updates)<0.0001&&
            Length(now.bounds.min-bounds.min)<0.01&&Length(now.bounds.max-bounds.max)<0.01;
    }
};

// Planning estimate for authored ForceMove volumes. Entry 6DD010 and
// Update 6D8860 use direction*speed; companion JumpFSM ForceUpdate6D4980
// applies its Y component while the movement owns the force state. Surface damping in 6D79F0 is
// 0.9. The terminal update queues Wait and damps the last velocity once more.
// Trigger-edge overlap, surface modifiers and collision can change the final
// pose. The controller always resumes from the observed native position.
inline bool PredictTransportExit(const TransportVolume& volume,Vec3 entry,Vec3& exit) noexcept {
    if(!Finite(entry)||!map_transport_detail::ValidPayload(kAuthoredMapTransportBounds,volume.speed,volume.updates,volume.direction))return false;
    const double distance=double(volume.speed)*(std::ceil(volume.updates)*0.9+0.81);
    exit={entry.x+static_cast<float>(volume.direction.x*distance),entry.y+static_cast<float>(volume.direction.y*distance),
          entry.z+static_cast<float>(volume.direction.z*distance)};
    return Finite(exit)&&Length(exit-entry)>100;
}
inline uint32_t TransportEntranceWaitMs(bool nativeRequestOutstanding) noexcept {
    return nativeRequestOutstanding?kMapTransportRequestTimeoutMs:
        kTransportRequestAssociationWindowMs;
}
inline bool TransportMadeProgress(
    Vec3 activated,Vec3 arrived,Vec3 direction,
    double minimumAlong=100.0) noexcept {
    if(!Finite(activated)||!Finite(arrived)||!Finite(direction)||
       !std::isfinite(minimumAlong)||minimumAlong<0)return false;
    const double directionLength=Length(direction);
    if(!std::isfinite(directionLength)||
       directionLength<kAuthoredMapTransportBounds.minimumDirectionLength)return false;
    return Dot(arrived-activated,direction*static_cast<float>(1.0/directionLength))>minimumAlong;
}
inline double TransportRouteTolerance(
    const TransportVolume& volume,float bodyRadius) noexcept {
    if(!std::isfinite(bodyRadius)||bodyRadius<=0||
       !map_transport_detail::ValidPayload(
           kAuthoredMapTransportBounds,volume.speed,volume.updates,volume.direction)||
       !detail::ValidAabb(volume.bounds))return NAN;
    const double directionLength=Length(volume.direction);
    if(!std::isfinite(directionLength)||
       directionLength<kAuthoredMapTransportBounds.minimumDirectionLength)return NAN;
    const Vec3 direction=volume.direction*static_cast<float>(1.0/directionLength);
    const Vec3 half=(volume.bounds.max-volume.bounds.min)*0.5f;
    // Activation can occur anywhere in the trigger/body overlap. One authored
    // speed step also bounds the terminal frame where the Force state queues
    // Wait. These are native geometric uncertainties, not a percentage of the
    // forecasted route.
    return bodyRadius+std::abs(direction.x)*half.x+
        std::abs(direction.y)*half.y+std::abs(direction.z)*half.z+volume.speed;
}
inline bool TransportReachedRouteProgress(
    Vec3 activated,Vec3 arrived,Vec3 plannedExit,
    const TransportVolume& volume,float bodyRadius) noexcept {
    if(!Finite(plannedExit)||!TransportMadeProgress(
           activated,arrived,volume.direction))return false;
    const double tolerance=TransportRouteTolerance(volume,bodyRadius);
    const double directionLength=Length(volume.direction);
    if(!std::isfinite(tolerance)||!std::isfinite(directionLength)||
       directionLength<kAuthoredMapTransportBounds.minimumDirectionLength)return false;
    const Vec3 direction=volume.direction*static_cast<float>(1.0/directionLength);
    const double actualAlong=Dot(arrived-activated,direction);
    const double requiredAlong=(std::max)(0.0,Dot(plannedExit-activated,direction));
    return actualAlong+tolerance>=requiredAlong;
}
inline bool TransportAtRouteExit(
    Vec3 actual,Vec3 exit,float bodyRadius) noexcept {
    if(!Finite(actual)||!Finite(exit)||!std::isfinite(bodyRadius)||bodyRadius<=0)return false;
    const double tolerance=(std::max)(25.0,double(bodyRadius));
    return std::hypot(double(actual.x-exit.x),double(actual.z-exit.z))<=tolerance&&
        std::abs(double(actual.y-exit.y))<=tolerance;
}
inline bool TransportAboveRouteExit(
    Vec3 actual,Vec3 exit,float bodyRadius) noexcept {
    if(!Finite(actual)||!Finite(exit)||!std::isfinite(bodyRadius)||bodyRadius<=0)
        return false;
    const double tolerance=(std::max)(25.0,double(bodyRadius));
    return std::hypot(double(actual.x-exit.x),double(actual.z-exit.z))<=
        tolerance&&actual.y>=exit.y-tolerance;
}
inline bool TransportGroundExitUseful(
    const TransportVolume& volume,Vec3 entry,Vec3 predicted,Vec3 exit,
    float bodyRadius) noexcept {
    if(!Finite(entry)||!Finite(predicted)||!Finite(exit)||
       !std::isfinite(bodyRadius)||bodyRadius<=0||
       !Finite(volume.direction)||!detail::ValidAabb(volume.bounds))
        return false;
    const Vec3 half=(volume.bounds.max-volume.bounds.min)*0.5f;
    const double horizontalTravel=std::hypot(
        double(predicted.x-entry.x),double(predicted.z-entry.z));
    const double activationReach=std::hypot(
        double(half.x),double(half.z))+bodyRadius;
    // A strongly upward pad may still be an authored cross-map shortcut. Only
    // a forecast whose horizontal gain remains inside the pad/body activation
    // footprint is "nearly vertical" and needs a higher support to be useful.
    if(volume.direction.y<=0.5f||horizontalTravel>activationReach)return true;
    return exit.y>=entry.y+(std::max)(100.0f,bodyRadius*2);
}

inline bool TransportPayloadMatches(
    const TransportVolume& volume,uint32_t volumeObjectId,
    bool payloadValid,bool padObjectValid,uint32_t padObjectId,
    uint32_t requestPilotId,uint32_t expectedPilotId,
    float speed,float durationUpdates,Vec3 direction) noexcept {
    if(!payloadValid||!expectedPilotId||
       requestPilotId!=expectedPilotId||
       (padObjectValid&&
        (!volumeObjectId||padObjectId!=volumeObjectId)))
        return false;
    return speed==volume.speed&&
        durationUpdates==volume.updates&&
        direction.x==volume.direction.x&&
        direction.y==volume.direction.y&&
        direction.z==volume.direction.z;
}

// CUnitMoveFSM::RunUpdate calls 6D79F0 with a non-zero command direction:
// existing X/Z velocity is multiplied by 0.6000000238418579 and the live
// unit+B2C run step is added in the normalized command direction.  The
// post-Force CUnitJumpFSM None update calls 6D4CC0: Y velocity is multiplied
// by 0.9800000190734863, then the admitted default branch subtracts
// unit+D10*0.20000000298023224 before applying the root displacement.
// This model is used only after the adapter has proved that exact native
// state/profile branch; it never writes a coordinate or changes gravity.
inline constexpr double kTransportRunVelocityRetention=
    0.6000000238418579;
inline constexpr double kTransportGravityVelocityRetention=
    0.9800000190734863;
inline constexpr double kTransportDefaultGravityFactor=
    0.20000000298023224;
inline constexpr double kTransportIdleVelocityRetention=
    0.8999999761581421;
inline constexpr double kTransportForceTerminalMoveRetention=
    kTransportIdleVelocityRetention*kTransportIdleVelocityRetention;
// npc.set_move_goal(...,1) is the ordinary 3A goal installed by battle_ai.
// CUnitMoveFSM::RunUpdate passes that mode to 6D79F0, whose mode-1 branch adds
// the complete live B2C run step. Other modes include the native 0.8 branch
// and are deliberately outside this forecast.
inline constexpr uint32_t kTransportOrdinaryGoalMode=1;

inline bool TransportOrdinaryGoalModeAllowed(
    uint32_t goalMode) noexcept {
    return goalMode==kTransportOrdinaryGoalMode;
}

struct TransportOrdinaryAirState {
    Vec3 position{},velocity{};
};

struct TransportOrdinaryMotionProfile {
    float runStep=0,gravityField=0;
    TransportOrdinaryAirState state{};
};

inline TransportOrdinaryAirState TransportForceTerminalAirState(
    const TransportVolume& volume,Vec3 position) noexcept {
    // MoveFSM resets Force velocity to direction*speed on authored updates and
    // applies 0.9 damping.  Its terminal update damps the retained X/Z once
    // more. JumpFSM Force keeps the preceding 0.9 Y velocity when it queues
    // None, which is the first ordinary-gravity input on the next update.
    const double horizontal=double(volume.speed)*
        kTransportForceTerminalMoveRetention;
    const double vertical=double(volume.speed)*
        kTransportIdleVelocityRetention;
    return {
        position,
        {
            static_cast<float>(double(volume.direction.x)*horizontal),
            static_cast<float>(double(volume.direction.y)*vertical),
            static_cast<float>(double(volume.direction.z)*horizontal)
        }
    };
}

inline bool TransportOrdinaryGravityProfileAllowed(
    uint32_t definitionId,uint8_t definitionGravityProfile,
    uint8_t jumpSpecialGravity,uint8_t jumpHeld,uint8_t jumpSuppressRoot,
    uint32_t attachedGround,uint8_t attackGravityOverride,
    uint32_t unitMotionFlags,uint32_t contextGravityOverride,
    float runStep,float gravityField) noexcept {
    const bool specialDefinition=
        definitionId==0x4290||definitionId==0x42B1||
        definitionId==0x5658||definitionId==0x2F08;
    return !specialDefinition&&definitionGravityProfile==0&&
        jumpSpecialGravity==0&&jumpHeld==0&&jumpSuppressRoot==0&&
        attachedGround==0&&attackGravityOverride==0&&
        (unitMotionFlags&0x1500)==0&&contextGravityOverride==0&&
        std::isfinite(runStep)&&runStep>0&&
        std::isfinite(gravityField)&&gravityField>0;
}

inline bool AdvanceTransportOrdinaryAir(
    TransportOrdinaryAirState& state,Vec3 horizontalDirection,
    float runStep,float gravityField,
    uint32_t goalMode=kTransportOrdinaryGoalMode) noexcept {
    if(!Finite(state.position)||!Finite(state.velocity)||
       !Finite(horizontalDirection)||!std::isfinite(runStep)||
       !std::isfinite(gravityField)||runStep<0||gravityField<=0||
       !TransportOrdinaryGoalModeAllowed(goalMode))
        return false;
    horizontalDirection.y=0;
    const double directionLength=Length(horizontalDirection);
    if(!std::isfinite(directionLength)||directionLength<0.5)return false;
    horizontalDirection=horizontalDirection*
        static_cast<float>(1.0/directionLength);
    state.velocity.x=static_cast<float>(
        double(state.velocity.x)*kTransportRunVelocityRetention+
        double(horizontalDirection.x)*runStep);
    state.velocity.z=static_cast<float>(
        double(state.velocity.z)*kTransportRunVelocityRetention+
        double(horizontalDirection.z)*runStep);
    state.velocity.y=static_cast<float>(
        double(state.velocity.y)*kTransportGravityVelocityRetention-
        double(gravityField)*kTransportDefaultGravityFactor);
    state.position=state.position+state.velocity;
    return Finite(state.position)&&Finite(state.velocity);
}

struct TransportOrdinaryAirArrival {
    bool reachable=false;
    uint32_t updates=0,arrivalUpdate=0;
    float peakY=-INFINITY;
    double horizontalTravel=0;
    std::vector<Vec3> path;
};

inline uint32_t TransportOrdinaryAirUpdateBound(
    TransportOrdinaryAirState state,Vec3 landing,
    float gravityField) noexcept {
    if(!Finite(state.position)||!Finite(state.velocity)||
       !Finite(landing)||!std::isfinite(gravityField)||
       gravityField<=0)
        return 0;
    const double gravity=gravityField;
    const double halfTerminal=5.0*gravity;
    const double terminal=10.0*gravity;
    uint64_t convergence=0;
    if(double(state.velocity.y)>-halfTerminal) {
        const double adjusted=double(state.velocity.y)+terminal;
        if(adjusted>halfTerminal) {
            const double ratio=halfTerminal/adjusted;
            const double raw=std::ceil(
                std::log(ratio)/
                std::log(kTransportGravityVelocityRetention));
            if(!std::isfinite(raw)||raw<0||
               raw>double(UINT32_MAX-2))
                return 0;
            convergence=static_cast<uint64_t>(raw);
        }
    }
    if(convergence) {
        const double retained=std::pow(
            kTransportGravityVelocityRetention,
            static_cast<double>(convergence));
        const double velocityOffset=
            double(state.velocity.y)+terminal;
        const double displacement=
            velocityOffset*kTransportGravityVelocityRetention*
                (1.0-retained)/
                (1.0-kTransportGravityVelocityRetention)-
            terminal*static_cast<double>(convergence);
        state.velocity.y=static_cast<float>(
            velocityOffset*retained-terminal);
        state.position.y=static_cast<float>(
            double(state.position.y)+displacement);
        if(!Finite(state.position)||!Finite(state.velocity))
            return 0;
    }
    const double remaining=(std::max)(
        0.0,double(state.position.y)-double(landing.y));
    const double rawExtra=std::ceil(remaining/halfTerminal)+2.0;
    if(!std::isfinite(rawExtra)||rawExtra<0||
       rawExtra>double(UINT32_MAX-convergence))
        return 0;
    return static_cast<uint32_t>(
        convergence+static_cast<uint64_t>(rawExtra));
}

inline bool TransportLandingCrossing(
    Vec3 from,Vec3 to,float landingY,Vec3& crossing) noexcept {
    crossing={};
    if(!Finite(from)||!Finite(to)||!std::isfinite(landingY)||
       from.y<landingY||to.y>landingY||from.y==to.y)
        return false;
    const double fraction=
        (double(from.y)-landingY)/
        (double(from.y)-double(to.y));
    if(!std::isfinite(fraction)||fraction<0||fraction>1)
        return false;
    crossing=from+(to-from)*static_cast<float>(fraction);
    crossing.y=landingY;
    return Finite(crossing);
}

inline TransportOrdinaryAirArrival PredictTransportOrdinaryAirArrival(
    TransportOrdinaryAirState state,Vec3 landing,float runStep,
    float gravityField,bool collectPath=false) noexcept {
    TransportOrdinaryAirArrival result{};
    if(!Finite(landing)||!std::isfinite(runStep)||runStep<=0||
       !std::isfinite(gravityField)||gravityField<=0)
        return result;
    const uint32_t maximumUpdates=
        TransportOrdinaryAirUpdateBound(
            state,landing,gravityField);
    if(!maximumUpdates)return result;
    result.peakY=state.position.y;
    if(collectPath)result.path.push_back(state.position);
    const double arrivalRadius=double(runStep)*10.0;
    double previousY=state.position.y;
    bool descending=false;
    for(uint32_t update=1;update<=maximumUpdates;++update) {
        const Vec3 before=state.position;
        const Vec3 command={
            landing.x-state.position.x,0,
            landing.z-state.position.z};
        const double remaining=Length(command);
        if(!std::isfinite(remaining))return {};
        if(!result.arrivalUpdate&&remaining<=arrivalRadius)
            result.arrivalUpdate=update;
        if(!result.arrivalUpdate) {
            if(!AdvanceTransportOrdinaryAir(
                   state,command,runStep,gravityField))return {};
        } else {
            // Native 3A has reached its B2C*10 stop radius.  Its zero-command
            // helper applies the ordinary 0.9 airborne damping to inherited
            // X/Z velocity while JumpFSM None continues exact gravity.
            state.velocity.x=static_cast<float>(
                double(state.velocity.x)*
                    kTransportIdleVelocityRetention);
            state.velocity.z=static_cast<float>(
                double(state.velocity.z)*
                    kTransportIdleVelocityRetention);
            state.velocity.y=static_cast<float>(
                double(state.velocity.y)*
                    kTransportGravityVelocityRetention-
                double(gravityField)*
                    kTransportDefaultGravityFactor);
            state.position=state.position+state.velocity;
            if(!Finite(state.position)||!Finite(state.velocity))return {};
        }
        const double fullHorizontalStep=std::hypot(
            double(state.position.x-before.x),
            double(state.position.z-before.z));
        result.horizontalTravel+=fullHorizontalStep;
        if(collectPath)result.path.push_back(state.position);
        result.peakY=(std::max)(result.peakY,state.position.y);
        descending=descending||state.velocity.y<=0;
        if(descending&&previousY>=landing.y&&state.position.y<=landing.y) {
            Vec3 crossing{};
            if(!TransportLandingCrossing(
                   before,state.position,landing.y,crossing))
                return {};
            result.horizontalTravel-=fullHorizontalStep;
            result.horizontalTravel+=std::hypot(
                double(crossing.x-before.x),
                double(crossing.z-before.z));
            if(collectPath)result.path.back()=crossing;
            result.updates=update;
            result.reachable=result.arrivalUpdate!=0&&
                result.arrivalUpdate<=update&&
                std::hypot(
                    double(landing.x-crossing.x),
                    double(landing.z-crossing.z))<=arrivalRadius;
            return result;
        }
        previousY=state.position.y;
    }
    return result;
}

// A full query is subdivided, never treated as a complete partial inventory.
// One region per engine interval bounds startup cost even on dense maps.
class TransportCatalog {
    uint8_t* document_=nullptr;
    uint8_t* tree_=nullptr;
    Aabb bounds_{};
    std::vector<Aabb> pending_;
    std::vector<TransportVolume> volumes_;
    uint64_t nextPump_=0,revision_=0;
    uint32_t queries_=0,saturated_=0;
    void Reset(uint8_t* document,uint8_t* tree,Aabb bounds) {
        document_=document;tree_=tree;bounds_=bounds;pending_.clear();pending_.push_back(bounds);
        volumes_.clear();nextPump_=0;queries_=saturated_=0;++revision_;
    }
public:
    void Clear() {document_=tree_=nullptr;pending_.clear();volumes_.clear();nextPump_=0;++revision_;}
    void Pump(QueryFn query,uint8_t* document,const Aabb& bounds,uint64_t now) {
        uint8_t* tree=nullptr;
        if(!query||!document||!detail::ValidAabb(bounds)||
           !detail::ReadPointer(document,detail::kDocumentQuadTree,tree))return;
        if(document!=document_||tree!=tree_||Length(bounds.min-bounds_.min)>0.01||Length(bounds.max-bounds_.max)>0.01)
            Reset(document,tree,bounds);
        if(pending_.empty()||now<nextPump_)return;
        nextPump_=now+16;
        const Aabb region=pending_.back();pending_.pop_back();
        constexpr int capacity=128;void* scenes[capacity]{};int count=0;
        query(tree,scenes,&count,capacity,&region);++queries_;
        if(count<0) {++saturated_;return;}
        if(count>=capacity) {
            const float x=(region.min.x+region.max.x)*0.5f,z=(region.min.z+region.max.z)*0.5f;
            if(region.max.x-region.min.x<=64||region.max.z-region.min.z<=64||pending_.size()>1020) {++saturated_;return;}
            for(unsigned i=0;i<4;++i)pending_.push_back({
                {i&1?x:region.min.x,region.min.y,i&2?z:region.min.z},
                {i&1?region.max.x:x,region.max.y,i&2?region.max.z:z}});
            return;
        }
        for(int i=0;i<count;++i) {
            TransportVolume volume{};
            if(!TransportVolume::Read(static_cast<uint8_t*>(scenes[i]),volume))continue;
            const auto known=std::find_if(volumes_.begin(),volumes_.end(),[&](const auto& old){return old.object==volume.object;});
            if(known!=volumes_.end())continue;
            if(volumes_.size()>=64) {++saturated_;continue;}
            volume.id=static_cast<uint32_t>(volumes_.size()+1);volumes_.push_back(volume);++revision_;
        }
    }
    const std::vector<TransportVolume>& Volumes()const {return volumes_;}
    uint64_t Revision()const {return revision_;}
    bool Complete()const {return document_&&pending_.empty()&&!saturated_;}
    uint32_t Queries()const {return queries_;}
    uint32_t Saturated()const {return saturated_;}
};
}
