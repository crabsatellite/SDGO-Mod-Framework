#pragma once
#include <cstdint>

namespace match_ai::transform {
struct LocalActor {
    bool registered=false,network=false,roomMaster=false,observer=false;
    bool localControl=false,remoteCopy=true,masterNpc=false,transforming=false;
    std::uint32_t pilot=0,object=0,livePilot=0,liveObject=0;
    std::uint32_t lifeState=0,hurtState=0,latchedTarget=0;
};

// NTF_TRANSFORM_UNIT (0x0A44) is 19 packed bytes. Its byte14 is copied to
// native Msg1D+18 by 0x004CFE00. A value1 asks the receiver to enter D4.
// An already-running local transition consumes its confirmation through the
// native local combat-event receiver. The generic NPC receiver re-enters D4
// for both replay values, releasing the form and sending another request.
// Keep unrepresented server events, native deferred/respawn handling, and
// every other receiver on the original path. A pending-only D4 has no fresh
// Entry latch and is not proof that this transition already owns control.
inline bool ReusesLocalTransform(const LocalActor& actor,std::uint32_t pilot,
        std::uint32_t target,std::uint8_t replay) noexcept {
    return replay<=1&&target&&actor.registered&&actor.network&&actor.roomMaster&&
        !actor.observer&&actor.localControl&&!actor.remoteCopy&&actor.masterNpc&&
        actor.object&&actor.object==actor.liveObject&&actor.pilot==pilot&&
        actor.pilot==actor.livePilot&&(pilot&0x80000000u)&&actor.lifeState&&
        actor.hurtState!=3&&actor.hurtState!=4&&actor.hurtState!=6&&
        actor.transforming&&actor.latchedTarget==target;
}
} // namespace match_ai::transform
