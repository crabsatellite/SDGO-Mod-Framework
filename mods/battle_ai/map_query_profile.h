#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace battle_ai::native::profile {
enum class Work : std::size_t { Ground, Body, Obb, Visibility, Count };
struct Cost {uint64_t calls=0,ticks=0,peak=0;};
struct Totals {
    std::array<Cost,static_cast<std::size_t>(Work::Count)> work{};
    uint64_t groundObbs=0,bodyObbs=0,sphereOverlap=0,sphereContact=0;
};
inline thread_local Totals* active=nullptr;
class Scope {
    Totals* previous_;
public:
    explicit Scope(Totals* value):previous_(active){active=value;}
    ~Scope(){active=previous_;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};
class Timer {
    Cost* cost_=nullptr;LARGE_INTEGER start_{};
public:
    explicit Timer(Work work) {
        if(active){cost_=&active->work[static_cast<std::size_t>(work)];QueryPerformanceCounter(&start_);}
    }
    ~Timer() {
        if(!cost_)return;
        LARGE_INTEGER end{};QueryPerformanceCounter(&end);
        const auto elapsed=static_cast<uint64_t>(end.QuadPart-start_.QuadPart);
        ++cost_->calls;cost_->ticks+=elapsed;
        if(elapsed>cost_->peak)cost_->peak=elapsed;
    }
    Timer(const Timer&)=delete;
    Timer& operator=(const Timer&)=delete;
};
}
