#pragma once
#include <cstdint>

// Native state6 is RESPAWN (SyncUnitHurtState 0x006D84B0). Its 180-update
// counter/ready flag are maintained by 0x006D7210; recovery is 0x006D8220.
// Transform's D4 Update (0x006DD900) does not call that recovery. Msg1D's
// state6 branch (0x006894FC) queues Neutral instead of entering D4.
// This checks that native invariant, without timing out legitimate animations
// or requiring an irreversible form to return to its previous definition.
inline bool NativeRespawnBlockedByTransform(std::uint32_t hurtState,
        std::int32_t remainingUpdates,bool ready,std::uint32_t movement,
        std::uint32_t pending) noexcept {
    if(hurtState!=6||remainingUpdates>0||!ready)return false;
    // An already queued transition out of D4 still belongs to native Release.
    if(pending&&pending!=0xD4)return false;
    return movement==0xD4||pending==0xD4;
}
