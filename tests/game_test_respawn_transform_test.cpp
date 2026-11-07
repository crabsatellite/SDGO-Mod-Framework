#include "../mods/game_test_client/native_respawn_transform_probe.h"
#include <cstdio>
int main() {
    int failures=0;
    const auto check=[&](bool value,const char* message) {
        if(!value){std::printf("FAIL %s\n",message);++failures;}
    };
    check(NativeRespawnBlockedByTransform(6,0,true,0xD4,0),
          "a ready respawn cannot remain hidden behind control-blocked transform ownership");
    check(NativeRespawnBlockedByTransform(6,-1,true,0x20,0xD4),
          "a queued transform must not replace ready respawn recovery");
    check(!NativeRespawnBlockedByTransform(6,0,true,0xD4,0x20),
          "native queued Neutral is allowed to release the transform");
    check(!NativeRespawnBlockedByTransform(6,0,true,0xD4,0x110),
          "another queued native exit remains owned by Release");
    check(!NativeRespawnBlockedByTransform(6,0,true,0x20,0),
          "ready ordinary respawn recovery is permitted");
    check(!NativeRespawnBlockedByTransform(6,180,false,0x20,0),
          "normal native respawn countdown is permitted");
    check(!NativeRespawnBlockedByTransform(6,1,true,0xD4,0),
          "an incomplete native countdown is not a wall-clock timeout");
    check(!NativeRespawnBlockedByTransform(6,0,false,0xD4,0),
          "the native ready witness is required");
    check(!NativeRespawnBlockedByTransform(1,0,true,0xD4,0),
          "a legal normal-state transformation is independent of reversibility");
    check(!NativeRespawnBlockedByTransform(4,0,true,0xD4,0),
          "special victim control is distinct from native respawn");
    std::printf("RESPAWN TRANSFORM PROBE %s\n",failures?"FAILED":"PASSED");
    return failures?1:0;
}
