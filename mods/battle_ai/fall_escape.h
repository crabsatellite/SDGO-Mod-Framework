#pragma once

#include "native_map_geometry.h"

namespace battle_ai::native {

// The shortest search spans the width of one machine. The longest uses the
// same local steering range as ordinary AdvancePoint.
inline bool FindFallEscape(MapGeometry& geometry, Vec3 here, Vec3 formerSupport,
                           Vec3& landing, Vec3& steering, bool* supportLost=nullptr) {
    constexpr float depth = 4096;
    constexpr float stepHeight = 60;
    if (supportLost) *supportLost = false;
    Vec3 support{};
    geometry.lastReject = 0;
    if (geometry.Ground(formerSupport, formerSupport.y + 12, formerSupport.y - 12, support) ||
        geometry.lastReject != 3)
        return false;
    if (supportLost) *supportLost = true;

    Vec3 below{here.x, here.y - depth, here.z};
    geometry.lastReject = 0;
    if (geometry.Ground(here, here.y + 1, below.y, support))
        below.y = support.y;
    else if (geometry.lastReject != 3)
        return false;
    geometry.lastReject = 0;
    if (geometry.ClearBody(here, below, true) || geometry.lastReject != 5)
        return false;

    constexpr float diagonal = 0.70710678f;
    constexpr Vec3 directions[] = {
        {1,0,0},{-1,0,0},{0,0,1},{0,0,-1},
        {diagonal,0,diagonal},{diagonal,0,-diagonal},
        {-diagonal,0,diagonal},{-diagonal,0,-diagonal}
    };
    const float nearestDistance = std::clamp(geometry.bodySphere.radius * 2, 40.0f, 130.0f);
    const float ranges[] = {nearestDistance, 210.0f, 390.0f};
    for (float distance : ranges) {
        for (Vec3 direction : directions) {
            const Vec3 point = here + direction * distance;
            if (geometry.spaceMap && !geometry.InsideSpace(point))
                continue;
            Vec3 ground{};
            if (!geometry.Ground(point, here.y + stepHeight, below.y, ground))
                continue;
            const float side = geometry.bodySphere.radius * 0.5f;
            Vec3 left{}, right{};
            const Vec3 footprint{direction.z * side, 0, -direction.x * side};
            if (!geometry.Ground(ground+footprint, ground.y+16, ground.y-16, left) ||
                !geometry.Ground(ground-footprint, ground.y+16, ground.y-16, right))
                continue;
            const float height = (std::max)({here.y, ground.y + 16, left.y + 8, right.y + 8});
            const Vec3 lift{here.x,height,here.z}, goal{point.x,height,point.z};
            if (!geometry.ClearBody(here,lift) || !geometry.ClearBody(lift,goal) ||
                !geometry.ClearBody(goal,ground))
                continue;
            landing = ground;
            steering = goal;
            return true;
        }
    }
    return false;
}

}
