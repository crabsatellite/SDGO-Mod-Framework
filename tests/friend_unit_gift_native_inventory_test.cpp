#include "../mods/friend_unit_gift_client/native_inventory.h"
#include "../mods/friend_unit_gift_client/native_reply_queue.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
using sdgo::friend_unit_gift::client::NativeInventorySync;
namespace {
std::array<std::uint8_t,0x28> info{};
std::uint8_t* info_pointer=info.data();
std::array<std::array<std::uint8_t,25>,36> brief{};
std::array<std::array<std::uint8_t,153>,36> full{};
unsigned gets=0,deletions=0,sends=0,changes=0;
std::uint64_t requested=0;
bool send_ok=true;
template<class T>void Put(void* p,std::size_t offset,T value){std::memcpy(static_cast<std::uint8_t*>(p)+offset,&value,sizeof(value));}
void Check(bool b,const char* message){if(!b){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
void Unit(std::uint16_t slot,std::uint64_t uid,bool complete=true){
 auto& b=brief[slot-1];auto& f=full[slot-1];b.fill(0);f.fill(0);
 Put<std::uint32_t>(b.data(),0,12345);Put(b.data(),4,uid);Put(b.data(),12,slot);
 if(complete){Put<std::uint32_t>(f.data(),0,12345);Put(f.data(),4,slot);Put(f.data(),8,uid);}
}
void* __fastcall Brief(void*,void*,void* out,std::uint16_t slot){++gets;std::memcpy(out,brief.at(slot-1).data(),25);return out;}
void* __fastcall Full(void*,void*,void* out,std::uint16_t slot){++gets;std::memcpy(out,full.at(slot-1).data(),153);return out;}
void __fastcall Erase(void* self,void*,const void* data,std::uint16_t slot){
 Check(self==info.data() && data==nullptr,"deletion calls the shipped null-data slot ABI");
 ++deletions;brief.at(slot-1).fill(0);full.at(slot-1).fill(0);
}
bool __cdecl RequestDetailShim(std::uint64_t uid,void*){++sends;requested=uid;return send_ok;}
void __cdecl Changed(void*){++changes;}
}
int main(){
 using namespace sdgo::friend_unit_gift;
 sdgo::friend_unit_gift::client::NativeUnitDetailQueue details;
 constexpr std::uint64_t detail_uid=0x1122334455667788ULL;
 std::array<std::uint8_t,167> detail_packet{};
 wire::Write16(detail_packet.data(),0x0699);
 std::array<std::uint8_t,153> detail_record{};detail_record.fill(0xA5);
 Put<std::uint32_t>(detail_record.data(),0,12345);Put<std::uint16_t>(detail_record.data(),4,2);
 Put(detail_record.data(),8,detail_uid);
 std::memcpy(detail_packet.data()+14,detail_record.data(),detail_record.size());
 const auto original_packet=detail_packet;
 Check(!details.Observe(detail_packet.data(),detail_packet.size()),"unrequested native details are not handed to the gift cache");
 details.Expect(9,detail_uid);
 Check(!details.Observe(detail_packet.data(),detail_packet.size()-1),"truncated native detail replies are rejected");
 details.Expect(9,detail_uid+1);
 Check(!details.Observe(detail_packet.data(),detail_packet.size()),"another owned unit cannot satisfy this detail read");
 details.Expect(9,detail_uid);
 Check(details.Observe(detail_packet.data(),detail_packet.size()) && detail_packet==original_packet,
       "only the authentic tracked reply is copied and its packet remains unchanged");
 sdgo::friend_unit_gift::client::NativeUnitDetailQueue::Record copied{};
 Check(!details.Pop(10,&copied),"a changed session discards old native detail data");
 details.Expect(10,detail_uid);
 Check(details.Observe(detail_packet.data(),detail_packet.size()) && details.Pop(10,&copied) && copied==detail_record,
       "UI-thread handoff preserves every byte including acquired date and equipped parts");
 Check(!details.Pop(10,&copied) && !details.Observe(detail_packet.data(),detail_packet.size()),
       "a completed read cannot replay its native cache write");
 details.Expect(10,detail_uid);Put<std::uint16_t>(detail_packet.data()+14,4,1201);
 Check(!details.Observe(detail_packet.data(),detail_packet.size()),"an out-of-capacity native target is rejected");
 sdgo::friend_unit_gift::client::NativeReplyQueue queue;
 std::array<std::uint8_t,kCarrierBytes> reply{};
 wire::Write16(reply.data(),kNativeReplyOpcode);
 Check(!queue.Capture(reply.data(),reply.size()),"ordinary storage replies keep their native path");
 std::memcpy(reply.data()+kReplyEnvelopeOffset,kEnvelopeMagic.data(),kEnvelopeMagic.size());
 Check(!queue.Capture(reply.data(),reply.size()-1),"unproved short payloads are never copied");
 for(unsigned i=0;i<9;++i)Check(queue.Capture(reply.data(),reply.size()),"owned namespace is consumed before native mail parsing");
 Check(queue.dropped()==1,"network handoff is bounded");
 std::array<std::uint8_t,kCarrierBytes> observed{};unsigned copies=0;
 while(queue.Pop(&observed)){Check(observed==reply,"handoff retains the actual decoded bytes");++copies;}
 Check(copies==8,"full queue never invents additional replies");
 Put<std::uint32_t>(info.data(),4,1);Put<std::uint32_t>(info.data(),0x0C,36);Put<std::uint8_t>(info.data(),0x20,1);
 NativeInventorySync sync;
 Check(sync.Bind({&info_pointer,reinterpret_cast<NativeInventorySync::Get>(&Brief),reinterpret_cast<NativeInventorySync::Get>(&Full),reinterpret_cast<NativeInventorySync::Delete>(&Erase),&RequestDetailShim,&Changed,nullptr}),"bind native callbacks");
 sync.SetSession(1);constexpr std::uint64_t old_uid=0x1122334455667788ULL,new_uid=old_uid+9;
 Unit(2,old_uid);Check(sync.HasFull(old_uid),"native brief/full identity agrees without floating point");
 Check(!sync.QueueRemoval(0),"zero UID is rejected");
 Check(sync.QueueRemoval(old_uid+1),"already absent instance can be acknowledged");sync.Tick(0);
 Check(deletions==0 && sync.HasFull(old_uid),"an absent UID never deletes another slot");
 Check(sync.QueueRemoval(old_uid),"confirmed removal queues by UID");sync.Tick(1);
 Check(deletions==1 && !sync.HasFull(old_uid) && sync.last_removed()==old_uid,"confirmed removal is read back from the native cache");
 Check(sync.QueueDetail(new_uid) && sync.QueueDetail(new_uid),"duplicate read request coalesces");sync.Tick(2);
 Check(sends==1 && requested==new_uid && sync.Busy() && sync.last_added()==0,"sending a detail query does not fabricate an accepted unit");
 Unit(5,new_uid,false);sync.Tick(3);
 Check(sync.Busy() && sends==1,"brief-only presence cannot prove full native data");
 Put<std::uint64_t>(full[4].data(),8,new_uid+1);sync.Tick(4);Check(sync.Busy(),"wrong full UID cannot satisfy synchronization");
 Unit(5,new_uid);sync.Tick(5);Check(!sync.Busy() && sync.last_added()==new_uid && changes>=2,"real full data completes the queued read");
 sync.SetSession(2);Put<std::uint8_t>(info.data(),0x20,0);const auto before=gets;
 sync.QueueRemoval(new_uid);sync.Tick(10);Check(gets==before && deletions==1,"initializing inventory is never scanned or deleted");
 sync.SetSession(3);Put<std::uint8_t>(info.data(),0x20,1);Put<std::uint32_t>(info.data(),4,0);
 Check(!sync.HasFull(new_uid) && gets==before,"missing native allocations prevent unsafe slot access");
 Put<std::uint32_t>(info.data(),4,1);sync.QueueRemoval(new_uid);sync.SetSession(4);sync.Tick(11);
 Check(sync.last_removed()==0 && deletions==1,"session changes discard old actor work");
 send_ok=false;sync.QueueDetail(new_uid+100);sync.Tick(20);sync.Tick(21);sync.Tick(22);
 Check(sync.error()==NativeInventorySync::Error::send_failed && sends==4 && deletions==1,"only bounded read-only attempts repeat");
 sync.Tick(23);Check(sends==4,"failed synchronization pauses until explicit retry");
 send_ok=true;sync.Retry();sync.Tick(24);Check(sends==5,"explicit retry sends one read, never a gift mutation");
 Unit(6,new_uid+100);sync.Tick(25);Check(!sync.Busy() && sync.error()==NativeInventorySync::Error::none,"retry completes from native data");
 std::puts("NATIVE INVENTORY SYNC CONTRACTS PASSED");
}
