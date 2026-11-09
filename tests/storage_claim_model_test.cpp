#include "sdgo/storage_claim_model.h"
#include <cassert>
#include <deque>
#include <iostream>
using namespace sdgo::storage;
namespace {
// In-order server: one reply per request, answered oldest first.
struct Server {
    Tab tab=Tab::Items;std::vector<Entry> rewards;
    std::set<Key> refuseCheck,refuseClaim,choice;
    std::deque<Request> queue;uint32_t maxInFlight=0,sent=0,checks=0,claims=0,lists=0;
    void Pump(Batch& b,uint64_t now=1) {
        while(const auto r=b.Next()){b.Sent(*r,now,true);queue.push_back(*r);++sent;
            if(r->operation==Operation::Check)++checks;if(r->operation==Operation::Claim)++claims;if(r->operation==Operation::List)++lists;}
        maxInFlight=std::max<uint32_t>(maxInFlight,static_cast<uint32_t>(queue.size()));
    }
    void Answer(Batch& b) {
        const auto r=queue.front();queue.pop_front();
        if(r.operation==Operation::List) {
            std::vector<Entry> page;const size_t first=static_cast<size_t>(r.page)*5;
            for(size_t i=first;i<rewards.size() && i<first+5;++i)page.push_back(rewards[i]);
            assert(b.ListReply(tab,static_cast<uint32_t>(rewards.size()),page));
        }else if(r.operation==Operation::Check) {
            if(refuseCheck.count(r.key))assert(b.CheckReply({},false,false,900));
            else assert(b.CheckReply(r.key,true,choice.count(r.key)!=0));
        }else {
            if(refuseClaim.count(r.key)){assert(b.ClaimReply({},false,2209));return;}
            const auto i=std::find_if(rewards.begin(),rewards.end(),[&](const Entry& e){return e.key==r.key;});assert(i!=rewards.end() && !i->claimed);
            if(tab==Tab::Items)rewards.erase(i);else i->claimed=true;
            assert(b.ClaimReply(r.key,true));
        }
    }
    void Run(Batch& b){for(int n=0;n<100000 && (b.Busy() || b.Outstanding());++n){Pump(b);if(queue.empty())break;Answer(b);}}
};
Server Items(uint64_t count){Server s;for(uint64_t i=1;i<=count;++i)s.rewards.push_back({{0x100000000ULL+i,0},false});return s;}
Server Gifts(uint64_t count){Server s;s.tab=Tab::Gifts;for(uint64_t i=1;i<=count;++i)s.rewards.push_back({{0xF0000000ULL+i,i*1234567890123ULL},false});return s;}
}
int main() {
    // Twelve items over three pages: three list reads, then twelve preflights and
    // twelve claims in flight together (window 16), then one refreshing read.
    {auto s=Items(12);Batch b;assert(b.Start(Tab::Items,7,12,5000,0,16));s.Run(b);
     assert(b.phase()==Phase::Complete && b.completed()==12 && s.rewards.empty() && b.collected()==12);
     assert(s.lists==4 && s.checks==12 && s.claims==12 && s.maxInFlight==12 && b.rounds()==1 && !b.Next());}
    // The window bounds requests awaiting an answer.
    {auto s=Items(12);Batch b;assert(b.Start(Tab::Items,7,12,5000,0,3));s.Run(b);assert(b.completed()==12 && s.maxInFlight==3);}
    {Batch b;assert(!b.Start(Tab::Items,7,1,10,0,0) && !b.Start(Tab::Items,7,1,10,0,Batch::kMaxDepth+1) && !b.Start(Tab::Items,0,1,10,0));}
    // Gifts stay listed: claimed letters are skipped, every page is read once.
    {auto s=Gifts(12);s.rewards[0].claimed=true;s.rewards[7].claimed=true;Batch b;assert(b.Start(Tab::Gifts,7,12,5000,0,8));s.Run(b);
     assert(b.phase()==Phase::Complete && b.completed()==10 && s.lists==4 && s.checks==10);
     for(const auto& e:s.rewards)assert(e.claimed);}
    // Refusals and manual selections are skipped and reported, the rest is claimed.
    {auto s=Items(7);s.refuseCheck.insert(s.rewards[2].key);s.refuseClaim.insert(s.rewards[5].key);Batch b;b.Start(Tab::Items,7,7,5000,0);s.Run(b);
     assert(b.phase()==Phase::Complete && b.completed()==5 && b.skipped()==2 && s.rewards.size()==2 && b.error()==2209 && b.rounds()==1);}
    {auto s=Gifts(4);s.choice.insert(s.rewards[1].key);Batch b;b.Start(Tab::Gifts,7,4,5000,0);s.Run(b);
     assert(b.phase()==Phase::Complete && b.completed()==3 && b.manual()==1 && !s.rewards[1].claimed && s.claims==3);}
    // Every preflight refused: nothing was granted and no refresh is needed.
    {auto s=Items(3);for(const auto& e:s.rewards)s.refuseCheck.insert(e.key);Batch b;b.Start(Tab::Items,7,3,5000,0);s.Run(b);
     assert(b.phase()==Phase::Complete && !b.completed() && b.skipped()==3 && s.claims==0 && s.lists==1);}
    // Stop during preflight: nothing more is sent; late preflights are absorbed.
    {auto s=Items(6);Batch b;b.Start(Tab::Items,7,6,5000,0);s.Pump(b);s.Answer(b);s.Pump(b);s.Answer(b);s.Pump(b);
     assert(b.Waiting(Operation::Check) && s.queue.size()==6);s.Answer(b);b.Stop(Reason::Cancelled);
     assert(b.phase()==Phase::Stopped && b.Outstanding() && !b.Next() && !b.Start(Tab::Items,7,6,10,3));
     while(!s.queue.empty())s.Answer(b);assert(!b.Outstanding() && s.claims==0 && b.completed()==0);}
    // Stop during claims: claims already sent finish, no new claim is sent.
    {auto s=Items(6);Batch b;b.Start(Tab::Items,7,6,5000,0,4);
     for(int n=0;n<6+2;++n){s.Pump(b);s.Answer(b);} // two lists, six preflights
     s.Pump(b);assert(b.Waiting(Operation::Claim) && s.queue.size()==4);s.Answer(b);b.Stop(Reason::Cancelled);
     assert(b.Busy() && !b.Next());while(!s.queue.empty())s.Answer(b);
     assert(b.phase()==Phase::Stopped && b.completed()==4 && s.rewards.size()==2 && s.claims==4 && !b.Next());}
    // A claim without an answer is Unknown; its late grant still counts.
    {auto s=Items(1);Batch b;b.Start(Tab::Items,7,1,10,0);s.Pump(b);s.Answer(b);s.Pump(b);s.Answer(b);s.Pump(b,100);
     b.Tick(20000,10000);assert(b.phase()==Phase::Unknown && b.reason()==Reason::Timeout && !b.Next() && !b.Start(Tab::Items,7,1,10,20001));
     s.Answer(b);assert(b.completed()==1 && b.phase()==Phase::Stopped && !b.Outstanding());}
    // A preflight without an answer only stops the batch.
    {auto s=Items(1);Batch b;b.Start(Tab::Items,7,1,10,0);s.Pump(b);s.Answer(b);s.Pump(b,100);b.Tick(20000,10000);
     assert(b.phase()==Phase::Stopped && b.reason()==Reason::Timeout && b.Outstanding());s.Answer(b);assert(!b.Outstanding() && !s.claims);}
    // Disconnect while claims are out: Unknown, never retried.
    {auto s=Items(3);Batch b;b.Start(Tab::Items,7,3,10,0);for(int n=0;n<4;++n){s.Pump(b);s.Answer(b);}s.Pump(b);
     assert(b.Waiting(Operation::Claim));b.Disconnect();assert(b.phase()==Phase::Unknown && !b.Next());
     while(!s.queue.empty())s.Answer(b);assert(b.completed()==3 && b.phase()==Phase::Stopped);}
    // A granted item that is still listed means the server did not do what it said.
    {Batch b;b.Start(Tab::Items,7,1,10,0);const Key k{4,0};
     auto r=b.Next();b.Sent(*r,1,true);b.ListReply(Tab::Items,1,{{k,false}});r=b.Next();b.Sent(*r,1,true);b.CheckReply(k,true);
     r=b.Next();b.Sent(*r,1,true);b.ClaimReply(k,true);r=b.Next();assert(r && r->operation==Operation::List && r->page==0);b.Sent(*r,1,true);
     b.ListReply(Tab::Items,1,{{k,false}});assert(b.reason()==Reason::Changed && b.phase()==Phase::Stopped && !b.Next());}
    // A grant for a reward never requested leaves the batch Unknown.
    {Batch b;b.Start(Tab::Items,7,1,10,0);const Key k{4,0};
     auto r=b.Next();b.Sent(*r,1,true);b.ListReply(Tab::Items,1,{{k,false}});r=b.Next();b.Sent(*r,1,true);b.CheckReply(k,true);
     r=b.Next();b.Sent(*r,1,true);b.ClaimReply({5,0},true);assert(b.phase()==Phase::Unknown && b.Outstanding() && !b.completed());
     b.ClaimReply(k,true);assert(b.completed()==1 && !b.Outstanding());}
    // A preflight success naming a different reward stops before any claim.
    {Batch b;b.Start(Tab::Items,7,2,10,0);auto r=b.Next();b.Sent(*r,1,true);b.ListReply(Tab::Items,2,{{{4,0},false},{{5,0},false}});
     r=b.Next();b.Sent(*r,1,true);r=b.Next();b.Sent(*r,1,true);b.CheckReply({5,0},true);assert(b.reason()==Reason::Changed && !b.Next());}
    // A failed claim send is Unknown; a failed list/preflight send only stops.
    {auto s=Items(1);Batch b;b.Start(Tab::Items,7,1,10,0);s.Pump(b);s.Answer(b);s.Pump(b);s.Answer(b);
     const auto r=b.Next();assert(r && r->operation==Operation::Claim);b.Sent(*r,1,false);assert(b.phase()==Phase::Unknown && b.Outstanding());}
    {Batch b;b.Start(Tab::Items,7,1,10,0);const auto r=b.Next();b.Sent(*r,1,false);assert(b.phase()==Phase::Stopped && !b.Outstanding() && b.reason()==Reason::SendFailed);}
    {Batch b;b.Start(Tab::Items,7,1,10,0);bool stale=false;try{b.Sent({Operation::Claim,Tab::Items,0,{4,0}},1,true);}catch(const std::exception&){stale=true;}assert(stale);}
    {auto s=Items(0);Batch b;b.Start(Tab::Items,7,0,10,0);s.Run(b);assert(b.phase()==Phase::Complete && !b.completed() && s.lists==1);}
    // Items beyond the native 255-page range are claimed in a second round.
    {auto s=Items(1300);Batch b;b.Start(Tab::Items,7,1300,5000,0,32);s.Run(b);
     assert(b.phase()==Phase::Complete && b.completed()==1300 && s.rewards.empty() && b.rounds()==2);}
    // The per-batch limit stops after claiming that many.
    {auto s=Items(7);Batch b;b.Start(Tab::Items,7,7,3,0);s.Run(b);assert(b.phase()==Phase::Stopped && b.reason()==Reason::Limit && b.completed()==3 && s.rewards.size()==4);}
    // Gift letters beyond 255 pages stop with a page-limit notice after claiming what was readable.
    {auto s=Gifts(2000);for(auto& e:s.rewards)e.claimed=true;Batch b;b.Start(Tab::Gifts,7,2000,5000,0);s.Run(b);
     assert(b.reason()==Reason::PageLimit && s.lists==255 && !b.Next());}
    // List errors and stale page replies stop without claiming.
    {Batch b;b.Start(Tab::Items,7,1,10,0);const auto r=b.Next();b.Sent(*r,1,true);b.Reject();assert(b.phase()==Phase::Stopped && b.reason()==Reason::Rejected && !b.Outstanding());}
    const Key large{0xFEDCBA9812345678ULL,0},gift{0xF0000001,0x8765432112345678ULL};
    const auto wire=Packet({Operation::Claim,Tab::Items,0,large});assert(wire.size()==14 && wire[0]==0xCC && wire[1]==9 && wire[6]==0x78 && wire[13]==0xFE);
    const auto giftWire=Packet({Operation::Claim,Tab::Gifts,0,gift});assert(giftWire.size()==18 && giftWire[0]==0x30 && giftWire[1]==6 && giftWire[9]==0xF0 && giftWire[17]==0x87);
    assert(Packet({Operation::List,Tab::Items,254,{}})[6]==255);
    bool rejected=false;try{Packet({Operation::List,Tab::Gifts,255,{}});}catch(const std::exception&){rejected=true;}assert(rejected);
    std::cout<<"STORAGE CLAIM MODEL PASSED: whole-tab collection, pipelined preflight/claim window, one refresh, skipped refusals/selections, stop/timeout/disconnect without retries, page range rounds, limits and native wire layouts\n";
}
