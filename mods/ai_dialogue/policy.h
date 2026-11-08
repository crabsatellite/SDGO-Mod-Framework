#pragma once
#include <string>
#include "protocol.h"

namespace sdgo::ai_dialogue {
inline const Participant* ParticipantFor(const Request& facts,uint32_t pilot) {
    for(unsigned i=0;i<facts.count&&i<kParticipants;++i)if(facts.roster[i].pilot==pilot)return &facts.roster[i];
    return nullptr;
}
inline bool CanAddress(const Request& event,const Request& current) {
    const auto* speaker=ParticipantFor(current,event.speaker);
    const auto* target=ParticipantFor(current,event.target);
    if(!speaker||!target||!speaker->ai||target->ai||event.target!=current.local||
       speaker->team<1||speaker->team>2||target->team<1||target->team>2)return false;
    if(event.kind==1)return speaker->team!=target->team&&event.killer==event.speaker&&event.victim==event.target;
    if(event.kind==2)return speaker->team!=target->team&&event.killer==event.target&&event.victim==event.speaker;
    if(event.kind!=3&&event.kind!=4)return false;
    if(speaker->team!=target->team||!speaker->alive||!target->alive)return false;
    if(event.kind==3) {
        const auto* focus=ParticipantFor(current,event.focus);
        return focus&&focus->alive&&focus->team>=1&&focus->team<=2&&focus->team!=speaker->team;
    }
    return true;
}
// The marker already carries the actual addressee. Resolve its native name on
// every viewer; a globally visible teammate line is not addressed to all viewers.
inline std::wstring AddressBody(const std::wstring& name,std::wstring body) {
    if(name.empty())return {};
    if(body.compare(0,name.size(),name)==0) {
        const auto next=body.size()>name.size()?body[name.size()]:L'\0';
        if(next==L'，'||next==L','||next==L'：'||next==L':'||next==L' '||!next) {
            body.erase(0,name.size());
            body.erase(0,body.find_first_not_of(L" ，,：:"));
        }
    }
    return body.empty()?std::wstring{}:name+L"，"+body;
}
}
