#include "../mods/ai_dialogue/policy.h"
#include <cassert>
using namespace sdgo::ai_dialogue;
int main() {
    Request facts{};facts.local=101;facts.count=4;
    facts.roster[0].pilot=101;facts.roster[0].team=1;facts.roster[0].alive=1;
    facts.roster[1].pilot=102;facts.roster[1].team=2;facts.roster[1].alive=1;
    facts.roster[2].pilot=0x80000002;facts.roster[2].team=1;facts.roster[2].alive=1;facts.roster[2].ai=1;
    facts.roster[3].pilot=0x80000003;facts.roster[3].team=2;facts.roster[3].alive=1;facts.roster[3].ai=1;
    Request pending=facts;pending.kind=3;pending.speaker=0x80000002;pending.target=101;pending.focus=0x80000003;
    assert(CanAddress(pending,facts));
    auto changed=facts;changed.roster[2].team=2;
    assert(!CanAddress(pending,changed));
    changed=facts;changed.roster[3].team=1;assert(!CanAddress(pending,changed));
    changed=facts;changed.roster[0].alive=0;assert(!CanAddress(pending,changed));
    changed=facts;changed.local=102;assert(!CanAddress(pending,changed));
    // The same AI is an opponent to a different human, despite both humans
    // seeing the same globally broadcast line. The addressee is independent.
    changed=facts;changed.local=102;pending.target=102;pending.kind=1;
    pending.killer=pending.speaker;pending.victim=102;
    assert(CanAddress(pending,changed));
    pending.kind=3;assert(!CanAddress(pending,changed));
    pending.kind=2;pending.killer=102;pending.victim=pending.speaker;
    changed.roster[2].alive=0;assert(CanAddress(pending,changed));
    changed.roster[2].team=2;assert(!CanAddress(pending,changed));
    assert(AddressBody(L"test1",L"一起集火重武装！")==L"test1，一起集火重武装！");
    assert(AddressBody(L"test1",L"test1，一起集火！")==L"test1，一起集火！");
    assert(AddressBody(L"test1",L"test1号机先撤。")==L"test1，test1号机先撤。");
    assert(AddressBody(L"玩家甲",L"玩家甲: 好枪。")==L"玩家甲，好枪。");
    assert(AddressBody(L"玩家甲",L"玩家甲，").empty());
    return 0;
}
