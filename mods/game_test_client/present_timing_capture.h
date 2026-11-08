#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>

// Passive quiet-lane evidence. The render callback never writes to disk.
// Records retain both QPC and the native uptime clock used by case boundaries.
class PresentTimingCapture {
    struct Sample {uint64_t sequence,uptime,qpc,gap;};
    static_assert(sizeof(Sample)==32);
    static constexpr size_t Capacity=8192;
    SRWLOCK lock_=SRWLOCK_INIT;
    std::array<Sample,Capacity> ring_{},pending_{};
    uint64_t produced_=0,consumed_=0,lost_=0;
    FILE* file_=nullptr;
    bool failed_=false;
public:
    void Record(uint64_t uptime,uint64_t qpc,uint64_t gap) {
        AcquireSRWLockExclusive(&lock_);
        const uint64_t sequence=++produced_;
        ring_[(sequence-1)%Capacity]={sequence,uptime,qpc,gap};
        ReleaseSRWLockExclusive(&lock_);
    }
    bool Drain(const wchar_t* path,uint64_t frequency) {
        if(failed_||!frequency)return !failed_;
        if(!file_) {
            file_=_wfopen(path,L"wb");
            if(!file_){failed_=true;return false;}
            const char magic[8]={'S','D','G','O','P','R','S','1'};
            const uint32_t pid=GetCurrentProcessId(),size=sizeof(Sample);
            if(fwrite(magic,1,sizeof(magic),file_)!=sizeof(magic)||
               fwrite(&pid,sizeof(pid),1,file_)!=1||fwrite(&size,sizeof(size),1,file_)!=1||
               fwrite(&frequency,sizeof(frequency),1,file_)!=1){failed_=true;return false;}
        }
        size_t count=0;
        AcquireSRWLockExclusive(&lock_);
        if(produced_-consumed_>Capacity) {
            lost_+=produced_-consumed_-Capacity;consumed_=produced_-Capacity;
        }
        while(consumed_<produced_&&count<Capacity) {
            pending_[count++]=ring_[consumed_%Capacity];++consumed_;
        }
        ReleaseSRWLockExclusive(&lock_);
        if((count&&fwrite(pending_.data(),sizeof(Sample),count,file_)!=count)||fflush(file_)!=0)
            failed_=true;
        return !failed_;
    }
    uint64_t Lost()const{return lost_;}
    bool Failed()const{return failed_;}
};
