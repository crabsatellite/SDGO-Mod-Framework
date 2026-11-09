#pragma once
// Live hangar refresh after a server sort or GB batch.
//
// The hangar requests travel as native paper (mail) sends 0x0891. The World's
// CRequest::ReqSendMsg calls the mail procedure, which runs the hangar command
// synchronously and returns 0 only after it committed; the World then replies
// 0x06AD (success) or 0x079F (refused) on the same session, one reply per send.
// After every expected success the client asks the World for each slot page
// with the native REQ_UNIT_SLOT_PAGE request 0x04FF (page p = slots 6p+1..6p+6,
// read from the database by sp_GetUserUnitPage_V2), writes each 0x08F2 record
// through the native CUnitInfo::SyncFullData, clears occupied slots that no
// page reported, and re-requests the user info (0x06B3). Its 0x05C1 reply
// rebuilds the active-unit record (whose slot moves with a sort) and the GB
// balance, so the refresh ends only once that reply has been handled. No
// record is fabricated; anything missing leaves the old "re-login" path in
// place.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sdgo::hangar::live {

constexpr uint16_t kSendOk = 0x06AD;
constexpr uint16_t kSendRefused = 0x079F;
constexpr uint16_t kPageRequest = 0x04FF;
constexpr uint16_t kPageReply = 0x08F2;
constexpr uint16_t kPageError = 0x09E4;
constexpr uint16_t kUserInfoRequest = 0x06B3;
constexpr uint16_t kUserInfoReply = 0x05C1;
constexpr size_t kRecordBytes = 0x99;
constexpr size_t kPageSlots = 6;
constexpr size_t kMaxPageBytes = 7 + kPageSlots * kRecordBytes;
constexpr size_t kMaxSlots = 1200;

// One observed reply, copied off the receive path.
struct Packet {
    uint16_t opcode = 0;
    uint16_t size = 0;
    std::array<uint8_t, kMaxPageBytes> data{};
};

// Copies a decoded reply when it is one the refresh consumes; false otherwise.
inline bool Capture(uint16_t opcode, const uint8_t* bytes, Packet* out) {
    size_t size = 0;
    if (!bytes || !out) return false;
    if (opcode == kSendOk) size = 6;
    else if (opcode == kSendRefused || opcode == kPageError) size = 8;
    else if (opcode == kUserInfoReply) size = 2;  // only its arrival matters
    else if (opcode == kPageReply) {
        const uint8_t count = bytes[6];
        if (count > kPageSlots) return false;
        size = 7 + count * kRecordBytes;
    } else {
        return false;
    }
    out->opcode = opcode;
    out->size = static_cast<uint16_t>(size);
    std::memcpy(out->data.data(), bytes, size);
    return true;
}

enum class State : uint32_t { Idle = 0, AwaitAck = 1, Reload = 2, AwaitInfo = 3 };
enum class Result : uint32_t { None = 0, Refreshed = 1, Refused = 2, Unknown = 3 };

struct Io {
    void* user = nullptr;
    bool (*send)(void* user, const uint8_t* bytes, int size) = nullptr;
    void (*write)(void* user, const uint8_t* record) = nullptr;   // CUnitInfo::SyncFullData
    bool (*occupied)(void* user, uint16_t slot) = nullptr;       // brief present at slot
    void (*clear)(void* user, uint16_t slot) = nullptr;          // native slot clear
    bool (*user_info)(void* user) = nullptr;                     // 0x06B3 request
};

class Refresh {
public:
    // A send is about to happen whose success reply (expected count) proves the
    // server committed; acknowledgements are counted from here.
    void Begin(size_t expectedAcks, uint64_t now, uint64_t ackTimeout, uint64_t reloadTimeout) {
        state_ = State::AwaitAck;
        expected_ = expectedAcks;
        ok_ = refused_ = 0;
        sendsDone_ = false;
        ackDeadline_ = now + ackTimeout;
        reloadTimeout_ = reloadTimeout;
        pages_ = nextPage_ = replies_ = errors_ = written_ = cleared_ = 0;
        seen_.assign(kMaxSlots + 1, 0);
        answered_.assign(kMaxSlots / kPageSlots + 1, 0);
        answeredPages_ = emptyReplies_ = 0;
        infoSeen_ = infoSeenEarlier_ = false;
    }
    void SendsDone() { sendsDone_ = true; }
    void Abort() { if (state_ != State::Idle) Finish(Result::Unknown); }

    // Consumes drained replies and advances; returns true when a refresh ended.
    bool Tick(uint64_t now, uint32_t capacity, const std::vector<Packet>& packets, const Io& io,
              int maxSendsPerTick = 4) {
        if (state_ == State::Idle) return false;
        for (const auto& p : packets) Consume(p, capacity, io);
        if (state_ == State::AwaitAck) {
            if (refused_) return Finish(Result::Refused);
            if (sendsDone_ && ok_ >= expected_) StartReload(now, capacity);
            else if (now >= ackDeadline_) return Finish(Result::Unknown);
        }
        if (state_ == State::AwaitInfo) {
            // The pre-handler copies 0x05C1 before the native handler applies
            // it, so success is reported one tick after the reply was seen.
            if (infoSeenEarlier_) return Finish(Result::Refreshed);
            if (infoSeen_) { infoSeenEarlier_ = true; return false; }
            if (now >= reloadDeadline_) return Finish(Result::Unknown);
            return false;
        }
        if (state_ != State::Reload) return false;
        // A page error or a record the client cannot hold ends the reload at once;
        // the cache keeps what it had and the re-login path applies.
        if (errors_) return Finish(Result::Unknown);
        for (int sent = 0; sent < maxSendsPerTick && nextPage_ < pages_; ++sent) {
            uint8_t request[10] = {};
            request[0] = static_cast<uint8_t>(kPageRequest & 0xFF);
            request[1] = static_cast<uint8_t>(kPageRequest >> 8);
            const uint32_t page = nextPage_;
            std::memcpy(request + 6, &page, sizeof(page));
            if (!io.send || !io.send(io.user, request, sizeof(request))) return Finish(Result::Unknown);
            ++nextPage_;
        }
        // A non-empty reply answers the page of its slots, so a native lazy
        // load of a page already answered is not counted twice.
        if (answeredPages_ + emptyReplies_ >= pages_ && nextPage_ >= pages_) {
            for (uint32_t slot = 1; slot <= capacity && slot <= kMaxSlots; ++slot) {
                if (seen_[slot] || !io.occupied || !io.occupied(io.user, static_cast<uint16_t>(slot))) continue;
                if (io.clear) io.clear(io.user, static_cast<uint16_t>(slot));
                ++cleared_;
            }
            if (!io.user_info || !io.user_info(io.user)) return Finish(Result::Unknown);
            state_ = State::AwaitInfo;
            infoSeen_ = infoSeenEarlier_ = false;
            reloadDeadline_ = now + kInfoTimeout;
            return false;
        }
        if (now >= reloadDeadline_) return Finish(Result::Unknown);
        return false;
    }

    State state() const { return state_; }
    Result result() const { return result_; }
    uint32_t generation() const { return generation_; }
    uint32_t pages() const { return pages_; }
    uint32_t replies() const { return replies_; }
    uint32_t written() const { return written_; }
    uint32_t cleared() const { return cleared_; }
    size_t acknowledged() const { return ok_; }
    size_t expected() const { return expected_; }

private:
    void StartReload(uint64_t now, uint32_t capacity) {
        state_ = State::Reload;
        const uint32_t slots = capacity > kMaxSlots ? static_cast<uint32_t>(kMaxSlots) : capacity;
        pages_ = static_cast<uint32_t>((slots + kPageSlots - 1) / kPageSlots);
        reloadDeadline_ = now + reloadTimeout_;
    }
    void Consume(const Packet& p, uint32_t capacity, const Io& io) {
        if (state_ == State::AwaitInfo) {
            if (p.opcode == kUserInfoReply) infoSeen_ = true;
            return;
        }
        if (state_ == State::AwaitAck) {
            if (p.opcode == kSendOk) ++ok_;
            else if (p.opcode == kSendRefused) ++refused_;
            return;
        }
        if (state_ != State::Reload) return;
        if (p.opcode == kPageError) { ++replies_; ++errors_; return; }
        if (p.opcode != kPageReply || p.size < 7) return;
        ++replies_;
        const uint8_t count = p.data[6];
        if (!count) { ++emptyReplies_; return; }
        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t* record = p.data.data() + 7 + i * kRecordBytes;
            uint32_t type = 0;
            uint16_t slot = 0;
            std::memcpy(&type, record, sizeof(type));
            std::memcpy(&slot, record + 4, sizeof(slot));
            // A record outside the client's allocated slot pages cannot be
            // written natively; the reload then stays incomplete (re-login).
            if (!type || !slot || slot > capacity || slot > kMaxSlots) { ++errors_; continue; }
            if (io.write) io.write(io.user, record);
            seen_[slot] = 1;
            ++written_;
            const uint32_t page = (slot - 1u) / kPageSlots;
            if (!answered_[page]) { answered_[page] = 1; ++answeredPages_; }
        }
    }
    bool Finish(Result result) {
        state_ = State::Idle;
        result_ = result;
        ++generation_;
        return true;
    }

    State state_ = State::Idle;
    Result result_ = Result::None;
    uint32_t generation_ = 0;
    size_t expected_ = 0, ok_ = 0, refused_ = 0;
    bool sendsDone_ = false;
    uint64_t ackDeadline_ = 0, reloadDeadline_ = 0, reloadTimeout_ = 0;
    uint32_t pages_ = 0, nextPage_ = 0, replies_ = 0, errors_ = 0, written_ = 0, cleared_ = 0;
    std::vector<uint8_t> seen_, answered_;
    uint32_t answeredPages_ = 0, emptyReplies_ = 0;
    bool infoSeen_ = false, infoSeenEarlier_ = false;
    static constexpr uint64_t kInfoTimeout = 10000;
};

} // namespace sdgo::hangar::live
