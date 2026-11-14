#pragma once
#include "sdgo/surface_decal.h"
#include <array>
#include <cmath>

namespace sdgo::surface_decal::render {
inline bool OpaqueNode(bool hidden,unsigned flags){
    // 0x10 enables the native cubemap reflection, not transparency. Preserve
    // the depth, blend and alpha-override exclusions independently of it.
    return !hidden&&!(flags&0x4d);
}
// Native group numbers refer to the selected rigid node, not material IDs.
// A transparent lens elsewhere on that node must not veto an opaque patch.
template<class ReadAlpha>
bool OpaqueCoverage(const Patch& patch,const std::vector<std::uint32_t>& triangleGroups,
                    unsigned groupCount,ReadAlpha readAlpha) {
    if(patch.vertices.empty()||patch.vertices.size()%3||!groupCount||groupCount>256)return false;
    std::array<bool,256> covered{};
    for(const auto& vertex:patch.vertices){
        if(vertex.triangle>=triangleGroups.size())return false;
        const auto group=triangleGroups[vertex.triangle];
        if(group>=groupCount)return false;
        covered[group]=true;
    }
    for(unsigned group=0;group<groupCount;++group){
        if(!covered[group])continue;
        float alpha=0;
        if(!readAlpha(group,alpha)||!std::isfinite(alpha)||alpha<0.999f)return false;
    }
    return true;
}
}
