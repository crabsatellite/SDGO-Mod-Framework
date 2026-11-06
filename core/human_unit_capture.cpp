#include <windows.h>
#include "sdgo/human_unit_capture.h"
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <atomic>

namespace sdgo::unit_forms {
namespace {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"capture callback queue requires lock-free native counters");
constexpr std::size_t kSource = 0x476, kPlayer = 0x1f54;
using PlayerKey = std::pair<std::uintptr_t,std::uintptr_t>;
using InvocationKey = std::pair<std::uint32_t,std::uint64_t>;
struct Input {
    std::array<std::uint8_t,kSource> source{};
    std::array<std::uint8_t,kPlayer> player{};
    PlayerKey key{};std::uintptr_t sourceAddress = 0;
    std::uint64_t epoch = 0, generation = 0, invocation = 0;
    std::uint8_t rule = 0, mode = 0;
};
struct Completed {
    std::uint64_t epoch = 0, generation = 0, invocation = 0;
    std::array<HumanUnitSeed,4> units{};
    unsigned count = 0;
};
struct Record {
    struct Definition { std::uintptr_t address=0;std::uint32_t id=0,target=0;bool valid=false; };
    SdgoBattleHumanUnitEventV1 event{};
    std::array<std::uint8_t,kSource> source{};
    std::array<std::uint8_t,kPlayer> player{};
    std::array<Definition,4> definitions{};
    bool readable=false;
};
std::uintptr_t Address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }
std::uint32_t U32(const std::uint8_t* p) { std::uint32_t value = 0;std::memcpy(&value,p,4);return value; }
std::uint64_t U64(const std::uint8_t* p) { std::uint64_t value = 0;std::memcpy(&value,p,8);return value; }
bool CopyRecord(const SdgoBattleHumanUnitEventV1* event,Record* record,const GraphBindings* graphs) {
    __try {
        record->event=*event;record->readable=false;record->definitions.fill({});
        if(event->size!=sizeof(*event)||event->version!=1||event->reserved||!event->sequence||
           event->thread_id!=GetCurrentThreadId()||!event->room||event->type<1||event->type>5)return false;
        if(event->type>=3)return !event->player&&!event->source;
        if(!event->source||!event->player)return false;
        if(event->type==2&&event->native_result!=1)return true;
        std::memcpy(record->source.data(),event->source,kSource);
        std::memcpy(record->player.data(),event->player,kPlayer);
        const auto selected=record->player[0x33c];if(selected>=4)return true;
        if(event->type==2){
            const bool deck=event->mode==6||event->mode==7;
            for(unsigned i=0;i<(deck?4u:1u);++i){
                const unsigned index=deck?i:selected;const auto root=U32(record->source.data()+(deck?0x19e+i*0x99:0x49));
                if(!root||!graphs->hasEntry(root))continue;
                const auto* slot=record->player.data()+0x340+index*0x658;
                const auto* definition=*reinterpret_cast<const std::uint8_t*const*>(slot+0x6c);
                auto& output=record->definitions[index];output.address=Address(definition);
                if(definition){output.id=U32(definition);output.target=U32(definition+0xc);output.valid=true;}
            }
        }
        record->readable=true;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { record->readable=false;return record->event.size==sizeof(*event); }
}
bool ReadAfter(const Record* record, const Input* input,
               const GraphBindings* graphs, Completed* completed) {
        if(!record->readable)return false;
        const auto* live = record->player.data();
        const auto* source = record->source.data();
        const auto owner = U32(input->source.data()+0x429);
        if (!owner || (owner & 0x80000000u) || U32(live) != owner || U32(source+0x429) != owner) return false;
        const bool deckMode = input->mode == 6 || input->mode == 7;
        const unsigned selected = input->player[0x33c];
        if(!deckMode&&live[0x33c]!=selected)return false;
        for (unsigned i = 0; i < (deckMode ? 4u : 1u); ++i) {
            const auto index = deckMode ? i : selected;
            const auto at = deckMode ? 0x19e+i*0x99 : 0x49;
            const auto* entry = input->source.data()+at;
            const auto root = U32(entry);if (!root || !graphs->hasEntry(root)) continue;
            const auto serial = U64(entry+8);
            if (!serial || U32(source+at) != root || U64(source+at+8) != serial) return false;
            const auto* slot = live+0x340+index*0x658;
            const auto& definition = record->definitions[index];
            const auto* graphNode = graphs->inspectNode(root,root);
            if (U32(slot) != root || !definition.valid || definition.id != root ||
                !graphNode || definition.target != graphNode->target) return false;
            auto& unit = completed->units[completed->count++];
            unit.root = root;unit.owner = owner;unit.deck = index;unit.serial = serial;unit.definition = reinterpret_cast<const void*>(definition.address);
            auto& seed = unit.composition;seed.rule = input->rule;seed.mode = input->mode;seed.source_rank = input->source[0x1f];
            seed.part_deck = deckMode ? static_cast<std::uint8_t>(i+1) : live[0x1d93];
            if (seed.part_deck < 1 || seed.part_deck > 4) return false;
            std::memcpy(&seed.boost_bonus,live+0x1dac+(seed.part_deck-1)*0x68,4);
            std::memcpy(seed.serialized_entry.data(),entry,seed.serialized_entry.size());
            const auto* before = input->player.data()+0x340+index*0x658;
            std::memcpy(seed.before.bytes.data(),before+0x64,kBankBytes);
            std::memcpy(seed.before.stats.data(),before+0xe,kStatsBytes);
        }
        return completed->count != 0;
}
}

struct HumanUnitCapture::Impl {
    struct Room { std::uint64_t epoch = 0;bool accepted = false; };
    std::shared_ptr<const GraphBindings> graphs;
    mutable std::mutex mutex;
    std::atomic<bool> healthy{true};
    static constexpr std::size_t capacity=128;
    struct Cell { std::atomic<std::uint64_t> ready{0};Record record; };
    std::array<Cell,capacity> queue;
    std::atomic<std::uint64_t> head{0},tail{0};
    std::uint64_t next = 0, completed = 0, rejected = 0;
    std::map<std::uintptr_t,Room> rooms;
    std::map<PlayerKey,Completed> players;
    std::map<PlayerKey,std::uint64_t> latest;
    std::map<InvocationKey,Input> pending;
    explicit Impl(std::shared_ptr<const GraphBindings> value) : graphs(std::move(value)) { if (!graphs) healthy = false; }
    std::uint64_t number() { if (++next == 0) { stop();return 0; }return next; }
    void stop() { healthy = false;rooms.clear();players.clear();latest.clear();pending.clear(); }
    void clearRoom(std::uintptr_t room) {
        for (auto i = players.begin(); i != players.end();) i = i->first.first == room ? players.erase(i) : std::next(i);
        for (auto i = latest.begin(); i != latest.end();) i = i->first.first == room ? latest.erase(i) : std::next(i);
        for (auto i = pending.begin(); i != pending.end();) i = i->second.key.first == room ? pending.erase(i) : std::next(i);
    }
    void process(const Record& record);
    bool drain(){
        const auto limit=head.load(std::memory_order_acquire);auto position=tail.load(std::memory_order_relaxed);
        if(!healthy.load())return false;
        while(position<limit){
            auto& cell=queue[position%capacity];
            if(cell.ready.load(std::memory_order_acquire)!=position+1)return false;
            process(cell.record);++position;tail.store(position,std::memory_order_release);
            if(!healthy.load())return false;
        }
        return head.load(std::memory_order_acquire)==limit;
    }
};

HumanUnitCapture::HumanUnitCapture(std::shared_ptr<const GraphBindings> graphs) : impl_(std::make_unique<Impl>(std::move(graphs))) {}
HumanUnitCapture::~HumanUnitCapture() = default;
void HumanUnitCapture::disable() noexcept { impl_->healthy.store(false,std::memory_order_release); }

void HumanUnitCapture::observe(const SdgoBattleHumanUnitEventV1& event) noexcept {
    auto& state=*impl_;if(!state.healthy.load(std::memory_order_acquire))return;
    const auto position=state.head.fetch_add(1,std::memory_order_acq_rel);
    if(position==UINT64_MAX||position-state.tail.load(std::memory_order_acquire)>=Impl::capacity){disable();return;}
    auto& cell=state.queue[position%Impl::capacity];
    if(!CopyRecord(&event,&cell.record,state.graphs.get())){disable();return;}
    cell.ready.store(position+1,std::memory_order_release);
}

void HumanUnitCapture::Impl::process(const Record& record) {
    const auto& event=record.event;
    try {
        auto& state = *this;
        const auto room = Address(event.room);const PlayerKey key{room,Address(event.player)};
        const InvocationKey invocation{event.thread_id,event.sequence};
        if (event.type >= SDGO_BATTLE_HUMAN_UNIT_ROOM_RELEASED) {
            if (!state.healthy) return;
            if (event.source || event.player) { state.stop();return; }
            if (event.type == SDGO_BATTLE_HUMAN_UNIT_START_ACCEPTED) {
                const auto found = state.rooms.find(room);if (found != state.rooms.end()) found->second.accepted = true;
            } else {
                state.clearRoom(room);state.rooms.erase(room);
                if (event.type == SDGO_BATTLE_HUMAN_UNIT_ROOM_INVALIDATED) {
                    if (state.rooms.size() >= 4096) { state.stop();return; }
                    const auto epoch = state.number();if (state.healthy) state.rooms[room] = {epoch,false};
                }
            }
            return;
        }
        if (!event.player || !event.source) { state.stop();return; }
        if (event.type == SDGO_BATTLE_HUMAN_UNIT_BEFORE) {
            Input input;input.key = key;input.sourceAddress = Address(event.source);input.rule = event.rule;input.mode = event.mode;input.invocation = event.sequence;
            {
                if (!state.healthy) return;
                state.players.erase(key);const auto found = state.rooms.find(room);
                if (found == state.rooms.end()) { ++state.rejected;return; }
                // Initial construction requires a fresh start boundary. A
                // later player rebuild needs its own explicit native profile.
                if(found->second.accepted){state.clearRoom(room);state.rooms.erase(room);++state.rejected;return;}
                input.epoch = found->second.epoch;input.generation = state.number();if (!state.healthy) return;
                if (state.latest.size() >= 8192 && !state.latest.count(key)) { state.stop();return; }
                state.latest[key] = input.generation;
            }
            const bool read = record.readable;
            if(read){input.source=record.source;input.player=record.player;}
            if (!state.healthy) return;
            const auto current = state.latest.find(key);const auto epoch = state.rooms.find(room);
            if (current == state.latest.end() || current->second != input.generation || epoch == state.rooms.end() || epoch->second.epoch != input.epoch) return;
            if (!read) { state.latest.erase(current);++state.rejected;return; }
            if (state.pending.size() >= 1024 || !state.pending.emplace(invocation,std::move(input)).second) state.stop();
            return;
        }
        Input input;
        {
            if (!state.healthy) return;
            const auto found = state.pending.find(invocation);if (found == state.pending.end()) {
                state.clearRoom(room);state.rooms.erase(room);++state.rejected;return;
            }
            input = std::move(found->second);state.pending.erase(found);
            if (input.key != key || input.sourceAddress != Address(event.source) || input.rule != event.rule || input.mode != event.mode) { state.stop();return; }
        }
        Completed frame;frame.epoch = input.epoch;frame.generation = input.generation;frame.invocation = input.invocation;
        const bool captured = event.native_result == 1 && ReadAfter(&record,&input,state.graphs.get(),&frame);
        if (!state.healthy) return;
        const auto current = state.latest.find(key);const auto epoch = state.rooms.find(room);
        if (current == state.latest.end() || current->second != input.generation || epoch == state.rooms.end() || epoch->second.epoch != input.epoch) return;
        if (!captured) { state.latest.erase(current);++state.rejected;return; }
        state.players[key] = std::move(frame);++state.completed;
    } catch (...) { stop(); }
}

bool HumanUnitCapture::snapshot(const void* room, const void* player, std::uint32_t deck,
                                std::uint32_t root, std::uint64_t serial, HumanUnitCaptureSnapshot& output) const {
    auto& state = *impl_;std::lock_guard<std::mutex> lock(state.mutex);
    if(!state.drain())return false;
    const auto r = state.rooms.find(Address(room));const auto p = state.players.find({Address(room),Address(player)});
    if (!state.healthy || r == state.rooms.end() || !r->second.accepted || p == state.players.end() || p->second.epoch != r->second.epoch) return false;
    for (unsigned i = 0; i < p->second.count; ++i) {
        const auto& unit = p->second.units[i];
        if (unit.deck == deck && unit.root == root && unit.serial == serial) {
            auto snapshot=HumanUnitCaptureSnapshot{Address(room),Address(player),p->second.epoch,p->second.generation,p->second.invocation,unit};
            if(!state.healthy.load(std::memory_order_acquire)||state.head.load(std::memory_order_acquire)!=state.tail.load(std::memory_order_acquire))return false;
            output=std::move(snapshot);return true;
        }
    }
    return false;
}
bool HumanUnitCapture::current(const HumanUnitCaptureSnapshot& snapshot) const {
    HumanUnitCaptureSnapshot actual;
    return this->snapshot(reinterpret_cast<const void*>(snapshot.room),reinterpret_cast<const void*>(snapshot.player),snapshot.unit.deck,
        snapshot.unit.root,snapshot.unit.serial,actual) && actual.roomEpoch == snapshot.roomEpoch && actual.generation == snapshot.generation;
}
HumanUnitCaptureStatus HumanUnitCapture::status() const {
    auto& s = *impl_;std::lock_guard<std::mutex> lock(s.mutex);
    const bool drained=s.drain();
    return {s.healthy.load()&&drained,static_cast<std::uint32_t>(s.rooms.size()),static_cast<std::uint32_t>(s.players.size()),static_cast<std::uint32_t>(s.pending.size()),s.completed,s.rejected};
}
bool HumanUnitCapture::inspect(HumanUnitCaptureSnapshot& output) const {
    auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);if(!s.drain())return false;
    for(const auto& player:s.players){
        const auto room=s.rooms.find(player.first.first);
        if(room==s.rooms.end()||!room->second.accepted||room->second.epoch!=player.second.epoch||!player.second.count)continue;
        if(!s.healthy.load()||s.head.load()!=s.tail.load())return false;
        output={player.first.first,player.first.second,player.second.epoch,player.second.generation,player.second.invocation,player.second.units[0]};return true;
    }
    return false;
}
} // namespace sdgo::unit_forms
