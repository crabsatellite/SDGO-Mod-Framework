#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace sdgo::storage {
enum class Tab { Items=0, Gifts=1 };
struct Key {
    uint64_t id=0,stamp=0;
    bool operator==(const Key& v)const{return id==v.id && stamp==v.stamp;}
    bool operator<(const Key& v)const{return std::tie(id,stamp)<std::tie(v.id,v.stamp);}
};
struct Entry {Key key;bool claimed=false;};
enum class Phase { Idle, NeedList, List, NeedCheck, Check, NeedClaim, Claim, Complete, Stopped, Unknown };
enum class Reason { None, Cancelled, Rejected, Selection, Changed, Timeout, SendFailed, Limit, Disconnected, PageLimit };
enum class Operation { List, Check, Claim };
struct Request {Operation operation;Tab tab;uint32_t page;Key key;};

// One confirmation claims the whole tab, page by page and without pauses:
//  1. read a list page (the native cache holds exactly that page),
//  2. send the original preflight for every unclaimed reward on it, back to back
//     (`depth` may overlap them; the Mod uses 1 because the World refuses
//     overlapping requests of one account),
//  3. send the original claim for every reward the server allowed, one at a time.
//     Every claimed reward is on the cached page, so the stock grant handler
//     never reloads the list by itself,
//  4. read the same page again when items were granted (later items move up),
//     otherwise the next page; finish on page one.
// The World answers every request exactly once but not necessarily in send order.
// Success replies carry the reward ID and are matched by it. Error replies carry
// no ID: they are counted, and once every other outstanding request of the kind
// has been answered the unanswered ones are the refused rewards. A refused reward
// is skipped and reported; it is never retried inside the batch. A claim whose
// answer is missing leaves the batch Unknown and nothing is retried.
class Batch {
public:
    static constexpr uint32_t kMaxDepth=32;
    bool Start(Tab tab,uint64_t session,uint32_t total,uint32_t limit,uint64_t now,uint32_t depth=8) {
        if(Busy() || Outstanding() || !session || !limit || !depth || depth>kMaxDepth)return false;
        (void)total;
        *this=Batch{};tab_=tab;session_=session;limit_=limit;depth_=depth;
        stage_=Stage::List;progress_=now;return true;
    }
    bool Busy()const{const auto p=phase();return p>=Phase::NeedList && p<=Phase::Claim;}
    bool Outstanding()const{return !inflight_.empty();}
    Phase phase()const {
        switch(stage_) {
        case Stage::Idle:return Phase::Idle;
        case Stage::List:return inflight_.empty()?Phase::NeedList:Phase::List;
        case Stage::Check:return Next()?Phase::NeedCheck:Phase::Check;
        case Stage::Claim:return Next()?Phase::NeedClaim:Phase::Claim;
        case Stage::Complete:return Phase::Complete;
        case Stage::Stopped:return Phase::Stopped;
        default:return Phase::Unknown;
        }
    }
    Reason reason()const{return reason_;}
    Tab tab()const{return tab_;}
    uint64_t session()const{return session_;}
    uint32_t completed()const{return completed_;}
    uint32_t skipped()const{return skipped_;}
    uint32_t manual()const{return manual_;}
    uint32_t collected()const{return collected_;}
    uint32_t total()const{return total_;}
    uint32_t page()const{return page_;}
    // List pages read by this batch.
    uint32_t rounds()const{return rounds_;}
    uint32_t depth()const{return depth_;}
    uint16_t error()const{return error_;}
    Key pending()const{return inflight_.empty()?Key{}:inflight_.front().key;}
    // Requests still awaiting an answer (refusals already counted are excluded).
    std::vector<Key> InFlight(Operation op)const {
        std::vector<Key> keys;for(const auto& f:inflight_)if(f.operation==op)keys.push_back(f.key);
        keys.resize(keys.size()>unmatched_?keys.size()-unmatched_:0);return keys;
    }
    uint32_t window()const{return stage_==Stage::Claim?1u:depth_;}
    void Stop(Reason reason) {
        cancel_=true;reason_=reason;
        if(ClaimsInFlight()){stage_=Stage::Claim;return;}
        if(stage_!=Stage::Unknown)stage_=Stage::Stopped;
    }
    void Disconnect() {
        reason_=Reason::Disconnected;cancel_=true;
        stage_=ClaimsInFlight()?Stage::Unknown:Stage::Stopped;
    }
    // Times out when no answer arrived for `timeout` while requests are outstanding.
    void Tick(uint64_t now,uint64_t timeout) {
        if(answers_!=tickAnswers_){tickAnswers_=answers_;progress_=now;}
        if(inflight_.empty() || stage_==Stage::Unknown)return;
        if(now>=progress_ && now-progress_>=timeout) {
            reason_=Reason::Timeout;cancel_=true;
            stage_=ClaimsInFlight()?Stage::Unknown:Stage::Stopped;
        }
    }
    std::optional<Request> Next()const {
        if(cancel_)return {};
        if(stage_==Stage::List)return inflight_.empty()?std::optional<Request>(Request{Operation::List,tab_,page_,{}}):std::nullopt;
        if(inflight_.size()>=window())return {};
        if(stage_==Stage::Check && checkIndex_<queue_.size())return Request{Operation::Check,tab_,0,queue_[checkIndex_]};
        if(stage_==Stage::Claim && claimIndex_<approved_.size())return Request{Operation::Claim,tab_,0,approved_[claimIndex_]};
        return {};
    }
    void Sent(const Request& request,uint64_t now,bool success) {
        const auto next=Next();
        if(!next || request.operation!=next->operation || request.tab!=tab_ || request.page!=next->page || !(request.key==next->key))
            throw std::runtime_error("stale storage request");
        if(request.operation==Operation::Check)++checkIndex_;
        if(request.operation==Operation::Claim)++claimIndex_;
        // A failed native send may still have queued a mutation. Never retry it.
        if(!success) {
            cancel_=true;reason_=Reason::SendFailed;
            if(request.operation==Operation::Claim){inflight_.push_back({request.operation,request.key,now});stage_=Stage::Unknown;}
            else if(!ClaimsInFlight())stage_=Stage::Stopped;
            return;
        }
        inflight_.push_back({request.operation,request.key,now});
        if(inflight_.size()==1)progress_=now;
    }
    bool Waiting(Operation op)const{return !inflight_.empty() && inflight_.front().operation==op;}
    bool ListReply(Tab tab,uint32_t total,const std::vector<Entry>& entries) {
        if(!Waiting(Operation::List) || tab!=tab_)return false;
        inflight_.pop_front();++answers_;++rounds_;
        if(cancel_){Settle();return true;}
        if(entries.size()>5 || total>1000000){Fail(Reason::Changed);return true;}
        std::set<Key> ids;
        for(const auto& entry:entries) {
            if(!entry.key.id || !ids.insert(entry.key).second){Fail(Reason::Changed);return true;}
            // The server removed (items) or marked (gifts) every reward it granted.
            if(!entry.claimed && done_.count(entry.key)){Fail(Reason::Changed);return true;}
        }
        total_=total;
        if(finishing_){finishing_=false;Finish();return true;}
        if(tab_==Tab::Items && page_==0 && total && entries.empty()){Fail(Reason::Changed);return true;}
        queue_.clear();approved_.clear();checkIndex_=claimIndex_=0;pageGranted_=0;
        for(const auto& entry:entries) {
            if(entry.claimed || refused_.count(entry.key))continue;
            if(completed_+static_cast<uint32_t>(queue_.size())>=limit_){limitHit_=true;break;}
            queue_.push_back(entry.key);++collected_;
        }
        if(queue_.empty()){NextPage();return true;}
        stage_=Stage::Check;return true;
    }
    // Preflight reply, matched by reward ID; a refusal is attributed once the
    // other outstanding preflights have answered.
    bool CheckReply(Key key,bool allowed,bool choice=false,uint16_t error=0) {
        if(!Waiting(Operation::Check))return false;
        ++answers_;
        if(allowed) {
            const auto match=Find(Operation::Check,key);
            if(match==inflight_.end()){Fail(Reason::Changed);ResolveRefusals();return true;}
            const auto granted=match->key;inflight_.erase(match);
            if(choice){++manual_;refused_.insert(granted);}
            else if(!cancel_)approved_.push_back(granted);
        }else {++unmatched_;++skipped_;error_=error;}
        ResolveRefusals();
        if(cancel_){Settle();return true;}
        if(inflight_.empty() && checkIndex_>=queue_.size()) {
            if(approved_.empty()){PageDone();return true;}
            stage_=Stage::Claim;
        }
        return true;
    }
    // Claim reply. Success names the granted reward; a refusal changed nothing
    // on the server.
    bool ClaimReply(Key key,bool success,uint16_t error=0) {
        if(!Waiting(Operation::Claim))return false;
        ++answers_;
        if(success) {
            const auto match=Find(Operation::Claim,key);
            if(match==inflight_.end()){cancel_=true;reason_=Reason::Changed;stage_=Stage::Unknown;return true;}
            const auto granted=match->key;inflight_.erase(match);
            if(!done_.insert(granted).second){Fail(Reason::Changed);return true;}
            ++completed_;++pageGranted_;
        }else {++unmatched_;++skipped_;error_=error;}
        ResolveRefusals();
        if(cancel_){Settle();return true;}
        if(inflight_.empty() && claimIndex_>=approved_.size())PageDone();
        return true;
    }
    void Reject(Reason reason=Reason::Rejected) {
        if(!inflight_.empty() && inflight_.front().operation!=Operation::Claim)inflight_.pop_front();
        Fail(reason);
    }
private:
    enum class Stage { Idle, List, Check, Claim, Complete, Stopped, Unknown };
    struct InFlightRequest {Operation operation;Key key;uint64_t sent;};
    bool ClaimsInFlight()const {
        const auto claims=std::count_if(inflight_.begin(),inflight_.end(),[](const InFlightRequest& f){return f.operation==Operation::Claim;});
        return static_cast<uint32_t>(claims)>(Waiting(Operation::Claim)?unmatched_:0u);
    }
    // Gift claim replies echo only the note ID; item IDs are unique on their own.
    std::deque<InFlightRequest>::iterator Find(Operation op,Key key) {
        return std::find_if(inflight_.begin(),inflight_.end(),[&](const InFlightRequest& f) {
            return f.operation==op && f.key.id==key.id && (!key.stamp || f.key.stamp==key.stamp);
        });
    }
    // Every outstanding request of the kind has answered: the ones left are the refused.
    void ResolveRefusals() {
        if(!unmatched_ || inflight_.size()!=unmatched_)return;
        for(const auto& f:inflight_)refused_.insert(f.key);
        inflight_.clear();unmatched_=0;
    }
    void Fail(Reason reason) {
        cancel_=true;reason_=reason;
        if(ClaimsInFlight()){stage_=Stage::Unknown;return;}
        if(stage_!=Stage::Unknown)stage_=Stage::Stopped;
    }
    // After cancellation: wait for claims already sent, then rest.
    void Settle() {
        if(ClaimsInFlight())return;
        if(stage_!=Stage::Unknown)stage_=Stage::Stopped;
        else if(inflight_.empty())stage_=Stage::Stopped;
    }
    void Finish() {
        if(limitHit_){Fail(Reason::Limit);return;}
        if(pageLimited_){Fail(Reason::PageLimit);return;}
        stage_=Stage::Complete;reason_=Reason::None;
    }
    // The claims of this page are settled. Granted items leave the list and the
    // following ones move up, so read the same page again; gifts stay listed.
    void PageDone() {
        if(tab_==Tab::Items && pageGranted_ && !limitHit_){stage_=Stage::List;return;}
        NextPage();
    }
    // Nothing more to claim on this page.
    void NextPage() {
        if(!limitHit_ && (page_+1)*5<total_) {
            // Native page numbers are one byte.
            if(page_>=254)pageLimited_=true;
            else{++page_;stage_=Stage::List;return;}
        }
        // Leave the page on its first list page, as a player would expect.
        if(page_){page_=0;finishing_=true;stage_=Stage::List;return;}
        Finish();
    }
    Stage stage_=Stage::Idle;Reason reason_=Reason::None;Tab tab_=Tab::Items;
    uint64_t session_=0,progress_=0;uint32_t answers_=0,tickAnswers_=0,unmatched_=0;
    uint32_t page_=0,completed_=0,skipped_=0,manual_=0,collected_=0,total_=0,limit_=5000,depth_=8;
    uint32_t rounds_=0,pageGranted_=0;uint16_t error_=0;
    std::vector<Key> queue_,approved_;size_t checkIndex_=0,claimIndex_=0;
    std::deque<InFlightRequest> inflight_;
    std::set<Key> done_,refused_;
    bool cancel_=false,finishing_=false,limitHit_=false,pageLimited_=false;
};

// Host wire formats: original authenticated requests; no account identity field.
inline std::vector<uint8_t> Packet(const Request& r) {
    const uint16_t op=r.operation==Operation::List?(r.tab==Tab::Items?0x9A1:0x9A2):
        r.operation==Operation::Check?(r.tab==Tab::Items?0x6CB:0x906):(r.tab==Tab::Items?0x9CC:0x630);
    std::vector<uint8_t> bytes(r.operation==Operation::List?10:r.tab==Tab::Items?14:18,0);
    auto put=[&](size_t offset,uint64_t value,size_t n){for(size_t i=0;i<n;++i)bytes[offset+i]=static_cast<uint8_t>(value>>(8*i));};
    put(0,op,2);
    if(r.operation==Operation::List){if(r.page>254)throw std::runtime_error("native storage page overflow");put(6,r.page+1,4);}
    else if(r.tab==Tab::Items)put(6,r.key.id,8);
    else {if(r.key.id>UINT32_MAX)throw std::runtime_error("gift note ID overflow");put(6,r.key.id,4);put(10,r.key.stamp,8);}
    return bytes;
}
}
