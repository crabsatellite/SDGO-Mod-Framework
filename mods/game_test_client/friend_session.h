#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include "friend_actions.h"
#include "sdgo/game_test.h"

namespace friends {
// Bounded developer IPC for a single explicitly owned GameTest session. All
// actions still run on the native main thread through FriendActions.
class FriendSession {
    FriendActions* actions_=nullptr;
    uint32_t sequence_=0;
    bool active_=false,ready_=false;
    uint64_t lastWrite_=0,commandAt_=0;
    unsigned completed_=0;

    static uint32_t Operation(const wchar_t* text) {
        const wchar_t* names[]={L"",L"ensure",L"refresh",L"page",L"add",L"accept",L"reject",L"open",L"send",L"remove"};
        for(uint32_t i=1;i<10;++i)if(!wcscmp(text,names[i]))return i;
        return 0;
    }
    static bool Text(const wchar_t* file,const wchar_t* key,char* target,int size) {
        wchar_t text[256]{};
        const auto count=GetPrivateProfileStringW(L"command",key,L"",text,256,file);
        if(count>=255)return false;
        BOOL fallback=FALSE;
        const auto n=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,text,-1,target,size,nullptr,&fallback);
        return n>0&&!fallback;
    }
    void Write(const SdgoGameTestContextV1* context,bool done) {
        const auto s=actions_->State();
        const std::wstring path=std::wstring(context->directory)+L"\\friend-result.ini",temporary=path+L".tmp";
        FILE* f=_wfopen(temporary.c_str(),L"wb");
        if(!f)return;
        fprintf(f,"[run]\nid=%s\npid=%lu\nready=%u\n[command]\nsequence=%u\ndone=%u\noperation=%u\nresult=%u\nrequest_sent=%u\nrequest_opcode=%u\ntransport_sequence=%u\nreply_sequence=%u\nreply_opcode=%u\nreply_status=%u\nreply_result=%u\nreply_consumed=%u\n",
            context->run_id,GetCurrentProcessId(),ready_?1u:0u,sequence_,done?1u:0u,s.operation,s.result,s.request_sent,
            s.request_opcode,s.request_transport_sequence,s.reply_sequence,s.reply_opcode,s.reply_status,s.reply_result,s.reply_consumed);
        fprintf(f,"[state]\napp=%u\npilot=%u\ninit=%u\npage=%u\nfriend_count=%u\nconversation_count=%u\nmessage_observed=%u\nlast_sender=%s\nlast_message=%s\n",
            s.native_app_state,s.profile_pilot_id,s.messenger_init_state,s.current_page,s.friend_count,s.conversation_count,
            s.message_observed,s.last_sender,s.last_message);
        fprintf(f,"list_page_replies=%u\nlist_total_count=%u\nlist_entries_seen=%u\n",
            s.list_page_replies,s.list_total_count,s.list_entries_seen);
        fprintf(f,"[pending]\navailable=%u\nsequence=%u\nsender_id=%u\nrecipient_id=%u\nsender=%s\nrecipient=%s\n",
            s.pending.available,s.pending.sequence,s.pending.sender_id,s.pending.recipient_id,s.pending.sender,s.pending.recipient);
        for(unsigned i=0;i<std::min(s.friend_count,16u);++i) {
            const auto& row=s.friends[i];
            fprintf(f,"[friend%u]\nid=%u\nname=%s\nrelation=%u\npresence=%u\nconversation_open=%u\n",
                i,row.pilot_id,row.nickname,row.relationship,row.presence,row.conversation_open);
        }
        fclose(f);MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        lastWrite_=GetTickCount64();
    }
public:
    void Bind(FriendActions* actions){actions_=actions;}
    int Poll(const SdgoGameTestContextV1* context) {
        if(!actions_){context->Assert(0,"friend action adapter is unavailable");return SDGO_GAME_TEST_FAIL;}
        const std::wstring abortPath=std::wstring(context->directory)+L"\\friend-abort.ini";
        wchar_t abortId[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",abortId,64,abortPath.c_str());
        const std::wstring expectedRun(context->run_id,context->run_id+strlen(context->run_id));
        if(expectedRun==abortId){context->Assert(0,"owned friend campaign requested a bounded abort");return SDGO_GAME_TEST_FAIL;}
        if(!ready_&&!active_) {
            const auto result=actions_->EnsureInitialized();
            if(result>=FriendActions::kBusy){context->Assert(0,"native messenger initialization could not start");return SDGO_GAME_TEST_FAIL;}
            active_=true;commandAt_=GetTickCount64();
        }
        actions_->Tick();
        const auto observed=actions_->State();
        const bool terminal=observed.result==FriendActions::kReady || observed.result==FriendActions::kAlreadySatisfied ||
            observed.result==FriendActions::kReplyObserved || observed.result>=FriendActions::kBusy;
        if(active_&&terminal) {
            if(!ready_) {
                if(observed.result>=FriendActions::kBusy){context->Assert(0,"native messenger initialization failed");return SDGO_GAME_TEST_FAIL;}
                ready_=true;context->Assert(1,"the real messenger initialized its server-loaded friend cache");
            } else {
                ++completed_;char message[192];
                sprintf_s(message,"friend command seq=%u op=%u result=%u sent=%u request=%04X reply=%04X/%04X consumed=%u",
                    sequence_,observed.operation,observed.result,observed.request_sent,observed.request_opcode,
                    observed.reply_opcode,observed.reply_status,observed.reply_consumed);
                context->Observe(message);
                if(observed.result==FriendActions::kNativeFault || observed.result==FriendActions::kCacheMalformed) {
                    context->Assert(0,"native friend action faulted or produced a malformed cache");return SDGO_GAME_TEST_FAIL;
                }
            }
            active_=false;Write(context,true);
        }
        if(active_&&GetTickCount64()-commandAt_>90000) {
            context->Assert(0,"native friend operation timed out waiting for real state/reply");return SDGO_GAME_TEST_FAIL;
        }
        if(GetTickCount64()-lastWrite_>=250)Write(context,!active_);
        if(active_||!ready_)return SDGO_GAME_TEST_WAIT;
        const std::wstring commandPath=std::wstring(context->directory)+L"\\friend-command.ini";
        wchar_t id[64]{},op[32]{};
        GetPrivateProfileStringW(L"run",L"id",L"",id,64,commandPath.c_str());
        const std::wstring expected(context->run_id,context->run_id+strlen(context->run_id));
        if(expected!=id)return SDGO_GAME_TEST_WAIT;
        const auto next=GetPrivateProfileIntW(L"command",L"sequence",0,commandPath.c_str());
        if(next<=sequence_)return SDGO_GAME_TEST_WAIT;
        if(next!=sequence_+1){context->Assert(0,"friend command sequence is not consecutive");return SDGO_GAME_TEST_FAIL;}
        GetPrivateProfileStringW(L"command",L"operation",L"",op,32,commandPath.c_str());
        sequence_=next;
        if(!wcscmp(op,L"finish")) {
            context->Assert(completed_>0,"the native friend session completed coordinated real operations");
            Write(context,true);return completed_?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
        }
        if(!wcscmp(op,L"snapshot")){++completed_;Write(context,true);return SDGO_GAME_TEST_WAIT;}
        FriendActions::Command command{};command.sequence=sequence_;command.operation=Operation(op);
        command.target_id=GetPrivateProfileIntW(L"command",L"target_id",0,commandPath.c_str());
        command.page=GetPrivateProfileIntW(L"command",L"page",1,commandPath.c_str());
        if(!command.operation || !Text(commandPath.c_str(),L"target_name",command.target_name,sizeof(command.target_name)) ||
            !Text(commandPath.c_str(),L"body",command.body,sizeof(command.body))) {
            context->Assert(0,"friend command is malformed");return SDGO_GAME_TEST_FAIL;
        }
        const auto result=actions_->Submit(command);active_=true;commandAt_=GetTickCount64();
        if(result==FriendActions::kNativeFault){context->Assert(0,"native friend command could not be submitted");return SDGO_GAME_TEST_FAIL;}
        Write(context,false);return SDGO_GAME_TEST_WAIT;
    }
};
}
