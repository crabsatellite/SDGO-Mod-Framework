#pragma once
#include <windows.h>
#include <cstdint>
#include "sdgo/sdgo_api.h"

// The shipped Miles driver first opens DirectSound and crashes in its failure
// cleanup on a non-input desktop. Its real AIL_open_digital_driver routine
// tests flag bit 0 before changing preference 15 (DIG_USE_WAVEOUT). Preserve
// that preference and use its native WaveOut path; never synthesize a driver.
// Master gain is per returned Miles driver, not the Windows/system volume.
class QuietAudio {
public:
    using OpenFn=void*(WINAPI*)(uint32_t,int,int,uint32_t);
    using PreferenceFn=int(WINAPI*)(uint32_t,int);
    using VolumeFn=void(WINAPI*)(void*,float);
    using ReadVolumeFn=float(WINAPI*)(void*);
    using ErrorFn=const char*(WINAPI*)();
    struct Bindings {PreferenceFn preference;VolumeFn volume;ReadVolumeFn read_volume;ErrorFn error=nullptr;};
    struct Observation {uint32_t active,opened,muted,failed,open_failed;};
    bool Install(const SdgoApi* api,SdgoMod* mod,const Bindings* supplied=nullptr) {
        if (!api || !api->HookImport || (active_ && active_!=this))return false;
        api_=api;mod_=mod;
        if (supplied) bindings_=*supplied;
        else {
            HMODULE miles=GetModuleHandleW(L"Mss32.dll");
            if (!miles)return false;
            bindings_={reinterpret_cast<PreferenceFn>(GetProcAddress(miles,"_AIL_set_preference@8")),
                reinterpret_cast<VolumeFn>(GetProcAddress(miles,"_AIL_set_digital_master_volume_level@8")),
                reinterpret_cast<ReadVolumeFn>(GetProcAddress(miles,"_AIL_digital_master_volume_level@4")),
                reinterpret_cast<ErrorFn>(GetProcAddress(miles,"_AIL_last_error@0"))};
        }
        if (!bindings_.preference || !bindings_.volume || !bindings_.read_volume)return false;
        active_=this;
        if (!api->HookImport(mod,"Mss32.dll","_AIL_open_digital_driver@16",reinterpret_cast<void*>(&Open),
                            reinterpret_cast<void**>(&original_)) || !original_) {active_=nullptr;return false;}
        installed_=true;return true;
    }
    Observation State() const {
        AcquireSRWLockShared(&lock_);
        const Observation state{installed_ ? 1u:0u,opened_,muted_,failed_,openFailed_};
        ReleaseSRWLockShared(&lock_);
        return state;
    }
private:
    static void* WINAPI Open(uint32_t rate,int bits,int channels,uint32_t flags) {
        QuietAudio* self=active_;
        if (!self || !self->original_)return nullptr;
        self->bindings_.preference(15,1);
        if (self->api_->Log) self->api_->Log(self->mod_,SDGO_LOG_INFO,"quiet Miles native WaveOut open: rate=%u bits=%d channels=%d flags=%u",rate,bits,channels,flags|1u);
        void* driver=self->original_(rate,bits,channels,flags|1u);
        if (self->api_->Log) self->api_->Log(self->mod_,SDGO_LOG_INFO,"quiet Miles native driver returned %p",driver);
        if (!driver) {
            if(self->api_->Log && self->bindings_.error) {
                const char* error=self->bindings_.error();
                self->api_->Log(self->mod_,SDGO_LOG_ERROR,"quiet Miles native open failed: %.256s",error ? error : "no driver error text");
            }
            // The original client may probe several formats/devices. A null
            // return supplies no audible driver; preserve that native failure.
            AcquireSRWLockExclusive(&self->lock_);
            ++self->openFailed_;
            ReleaseSRWLockExclusive(&self->lock_);
            return nullptr;
        }
        self->bindings_.volume(driver,0.0f);
        const bool muted=self->bindings_.read_volume(driver)==0.0f;
        // Publish only completed volume readback, atomically with its count.
        // Polling must not interpret an in-flight mute as an audible driver.
        AcquireSRWLockExclusive(&self->lock_);
        ++self->opened_;
        if (muted)++self->muted_;
        else ++self->failed_;
        ReleaseSRWLockExclusive(&self->lock_);
        return driver;
    }
    inline static QuietAudio* active_=nullptr;
    Bindings bindings_{};
    const SdgoApi* api_=nullptr;
    SdgoMod* mod_=nullptr;
    OpenFn original_=nullptr;
    bool installed_=false;
    mutable SRWLOCK lock_=SRWLOCK_INIT;
    uint32_t opened_=0,muted_=0,failed_=0,openFailed_=0;
};
