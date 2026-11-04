#pragma once

#include <array>

namespace sdgo::friend_unit_gift::client::paging {

// Number buttons address the visible ten-page group. In a gift picker the
// single arrows browse adjacent pages, including the end of a partial group.
inline int TargetPage(int current,int count,int button) noexcept {
    if (count<1 || current<0 || current>=count) return -1;
    if (button==10) return current>0 ? current-1:current;
    if (button==11) return current+1<count ? current+1:current;
    if (button<0 || button>=10) return -1;
    const int target=current-current%10+button;
    return target<count ? target:-1;
}

struct NativeSteps {
    std::array<int,2> index{};
    std::array<unsigned char,2> forward{};
    unsigned count=0;
};

// Native -1 moves to the first page of the neighbouring group; use a second
// native index action when going back from page 11 to page 10, for example.
inline NativeSteps Steps(int current,int target) noexcept {
    NativeSteps result;
    if (current<0 || target<0 || current==target) return result;
    if (current/10!=target/10) {
        if (current/10+1!=target/10 && current/10-1!=target/10) return result;
        result.index[0]=-1;result.forward[0]=target>current ? 1u:0u;result.count=1;
        if (target%10==0) return result;
    }
    result.index[result.count]=target%10;result.forward[result.count]=0;++result.count;
    return result;
}

} // namespace sdgo::friend_unit_gift::client::paging
