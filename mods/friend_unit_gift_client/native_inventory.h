#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>

namespace sdgo::friend_unit_gift::client {

// Inventory changes use the shipped cache deletion operation and the shipped
// read-only detail request. Its normal reply writes both the 153-byte full
// record and its 25-byte brief through CUnitInfo::WriteSyncRecord; no cache
// teardown, fabricated record, database access or replacement reply is used.
class NativeInventorySync {
public:
    using Get = void*(__thiscall*)(void*, void*, std::uint16_t);
    using Delete = void(__thiscall*)(void*, const void*, std::uint16_t);
    using RequestDetail = bool(__cdecl*)(std::uint64_t, void*);
    using Changed = void(__cdecl*)(void*);
    struct Bindings {
        std::uint8_t** info = nullptr;
        Get brief = nullptr;
        Get full = nullptr;
        Delete erase = nullptr;
        RequestDetail request_detail = nullptr;
        Changed changed = nullptr;
        void* user = nullptr;
    };
    enum class Error : std::uint32_t { none, native_access, send_failed, timeout };

    bool Bind(Bindings bindings) noexcept {
        bindings_ = bindings;
        return bindings.info && bindings.brief && bindings.full &&
               bindings.erase && bindings.request_detail;
    }
    void SetSession(std::uint64_t session) noexcept {
        if (session_ == session) return;
        session_ = session; read_ = count_ = attempts_ = 0;
        removal_ = pending_ = sent_at_ = 0; error_ = Error::none;
        last_added_ = last_removed_ = 0;
    }
    bool QueueRemoval(std::uint64_t uid) noexcept {
        if (!session_ || !uid || (removal_ && removal_ != uid)) return false;
        removal_ = uid;
        return true;
    }
    bool QueueDetail(std::uint64_t uid) noexcept {
        if (!session_ || !uid) return false;
        if (removal_ != uid && HasFull(uid)) { last_added_ = uid; return true; }
        for (std::size_t i = read_; i < count_; ++i)
            if (details_[i] == uid) return true;
        if (read_ == count_) read_ = count_ = 0;
        if (count_ == details_.size()) return false;
        details_[count_++] = uid;
        return true;
    }
    bool Busy() const noexcept { return removal_ || read_ < count_; }
    Error error() const noexcept { return error_; }
    std::uint64_t last_added() const noexcept { return last_added_; }
    std::uint64_t last_removed() const noexcept { return last_removed_; }
    void Retry() noexcept { error_ = Error::none; pending_ = sent_at_ = 0; attempts_ = 0; }

    bool HasFull(std::uint64_t uid) const noexcept {
        __try { return HasFullUnsafe(uid); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void Tick(std::uint64_t now) noexcept {
        if (!session_ || error_ != Error::none || !Busy() || !Ready()) return;
        if (removal_) {
            if (!Remove(removal_)) { error_ = Error::native_access; return; }
            last_removed_ = removal_; removal_ = 0;
            ChangedNow();
        }
        if (read_ == count_) return;
        const auto uid = details_[read_];
        if (HasFull(uid)) {
            last_added_ = uid; ++read_; pending_ = sent_at_ = 0; attempts_ = 0;
            ChangedNow(); return;
        }
        if (pending_ && now - sent_at_ < 10000) return;
        if (attempts_ >= 3) { error_ = Error::timeout; return; }
        ++attempts_;
        if (!bindings_.request_detail || !bindings_.request_detail(uid, bindings_.user)) {
            pending_ = 0; sent_at_ = now;
            if (attempts_ >= 3) error_ = Error::send_failed;
            return;
        }
        pending_ = uid; sent_at_ = now;
    }
private:
    template<class T> static T Read(const void* value, std::size_t offset=0) noexcept {
        T result{}; std::memcpy(&result, static_cast<const std::uint8_t*>(value)+offset, sizeof(result));return result;
    }
    bool ReadyUnsafe() const {
        if (!bindings_.info || !*bindings_.info || !bindings_.brief || !bindings_.full || !bindings_.erase) return false;
        const auto* info = *bindings_.info;
        const auto capacity = Read<std::uint32_t>(info,0x0C);
        return Read<std::uint8_t>(info,0x20)==1 && capacity>0 && capacity<=1200 &&
               Read<std::uint32_t>(info,4)>=(capacity+35)/36;
    }
    bool Ready() const noexcept {
        __try {return ReadyUnsafe();} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool HasFullUnsafe(std::uint64_t uid) const {
        if (!uid || !ReadyUnsafe()) return false;
        auto* info = *bindings_.info;
        const auto capacity = Read<std::uint32_t>(info,0x0C);
        for (std::uint32_t slot=1;slot<=capacity;++slot) {
            std::array<std::uint8_t,25> brief{};
            bindings_.brief(info,brief.data(),static_cast<std::uint16_t>(slot));
            if (!Read<std::uint32_t>(brief.data()) || Read<std::uint64_t>(brief.data(),4)!=uid ||
                Read<std::uint16_t>(brief.data(),12)!=slot) continue;
            std::array<std::uint8_t,153> full{};
            bindings_.full(info,full.data(),static_cast<std::uint16_t>(slot));
            return Read<std::uint32_t>(full.data())==Read<std::uint32_t>(brief.data()) &&
                   Read<std::uint16_t>(full.data(),4)==slot && Read<std::uint64_t>(full.data(),8)==uid;
        }
        return false;
    }
    bool RemoveUnsafe(std::uint64_t uid) {
        if (!ReadyUnsafe()) return false;
        auto* info = *bindings_.info;
        const auto capacity = Read<std::uint32_t>(info,0x0C);
        for (std::uint32_t slot=1;slot<=capacity;++slot) {
            std::array<std::uint8_t,25> brief{};
            bindings_.brief(info,brief.data(),static_cast<std::uint16_t>(slot));
            if (!Read<std::uint32_t>(brief.data()) || Read<std::uint64_t>(brief.data(),4)!=uid) continue;
            if (Read<std::uint16_t>(brief.data(),12)!=slot) return false;
            bindings_.erase(info,nullptr,static_cast<std::uint16_t>(slot));
            brief.fill(0);bindings_.brief(info,brief.data(),static_cast<std::uint16_t>(slot));
            return Read<std::uint32_t>(brief.data())==0 || Read<std::uint64_t>(brief.data(),4)!=uid;
        }
        return true; // An already-absent confirmed instance needs no deletion.
    }
    bool Remove(std::uint64_t uid) noexcept {
        __try {return RemoveUnsafe(uid);} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    void ChangedNow() noexcept {if (bindings_.changed) bindings_.changed(bindings_.user);}
    Bindings bindings_{};
    std::uint64_t session_=0,removal_=0,pending_=0,sent_at_=0,last_added_=0,last_removed_=0;
    std::array<std::uint64_t,1200> details_{};
    std::size_t read_=0,count_=0;
    unsigned attempts_=0;
    Error error_=Error::none;
};
}
