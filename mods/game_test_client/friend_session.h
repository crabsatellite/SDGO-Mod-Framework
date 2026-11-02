#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include "friend_actions.h"
#include "friend_extra_actions.h"
#include "sdgo/game_test.h"

namespace friends {
// Bounded developer IPC for a single explicitly owned GameTest session. All
// actions still run on the native main thread through FriendActions.
class FriendSession {
    FriendActions* actions_=nullptr;
    FriendExtraActions* extras_=nullptr;
    uint8_t** profile_=nullptr;
    bool extraCommand_=false;
    uint32_t sequence_=0;
    bool active_=false,ready_=false;
    uint64_t lastWrite_=0,commandAt_=0;
    unsigned completed_=0;
    static std::string Utf8(const char* value) {
        wchar_t wide[512]{};char encoded[2048]{};
        if(!MultiByteToWideChar(CP_ACP,0,value,-1,wide,512) ||
            !WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,encoded,2048,nullptr,nullptr))return "<invalid-native-text>";
        return encoded;
    }
    bool OwnName(char* name) const {
        __try {
            if(!profile_||!*profile_)return false;
            memcpy(name,*profile_+0x161,16);name[16]=0;
            return name[0]!=0;
        }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }

    static uint32_t Operation(const wchar_t* text) {
        const wchar_t* names[]={L"",L"ensure",L"refresh",L"page",L"add",L"accept",L"reject",L"open",L"send",L"remove",L"close",L"profile"};
        for(uint32_t i=1;i<12;++i)if(!wcscmp(text,names[i]))return i;
        return 0;
    }
    FriendActions::Observation Observation() const {
        auto state=actions_->State();
        if(extraCommand_&&extras_) {
            const auto e=extras_->State();
            state.result=e.result;state.command_sequence=e.command_sequence;state.operation=e.operation;
            state.target_id=e.target_id;memcpy(state.target_name,e.target_name,sizeof(state.target_name));
            state.request_sent=0;state.request_opcode=state.reply_opcode=state.reply_status=0;
            state.reply_consumed=0;
        }
        return state;
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
        const auto s=Observation();
        const std::wstring path=std::wstring(context->directory)+L"\\friend-result.ini",temporary=path+L".tmp";
        FILE* f=_wfopen(temporary.c_str(),L"wb");
        if(!f)return;
        fprintf(f,"[run]\nid=%s\npid=%lu\nready=%u\n[command]\nsequence=%u\ndone=%u\noperation=%u\nresult=%u\nrequest_sent=%u\nrequest_opcode=%u\ntransport_sequence=%u\nreply_sequence=%u\nreply_opcode=%u\nreply_status=%u\nreply_result=%u\nreply_consumed=%u\n",
            context->run_id,GetCurrentProcessId(),ready_?1u:0u,sequence_,done?1u:0u,s.operation,s.result,s.request_sent,
            s.request_opcode,s.request_transport_sequence,s.reply_sequence,s.reply_opcode,s.reply_status,s.reply_result,s.reply_consumed);
        fprintf(f,"local_validation_rejected=%u\nlocal_validation_code=%u\n",
            s.local_validation_rejected,s.local_validation_code);
        fprintf(f,"[state]\napp=%u\npilot=%u\ninit=%u\npage=%u\nfriend_count=%u\nconversation_count=%u\nmessage_observed=%u\nlast_sender=%s\nlast_message=%s\n",
            s.native_app_state,s.profile_pilot_id,s.messenger_init_state,s.current_page,s.friend_count,s.conversation_count,
            s.message_observed,Utf8(s.last_sender).c_str(),Utf8(s.last_message).c_str());
        fprintf(f,"list_page_replies=%u\nlist_total_count=%u\nlist_entries_seen=%u\n",
            s.list_page_replies,s.list_total_count,s.list_entries_seen);
        char own[17]{};OwnName(own);fprintf(f,"own_name=%s\n",Utf8(own).c_str());
        fprintf(f,"[pending]\navailable=%u\nsequence=%u\nsender_id=%u\nrecipient_id=%u\nsender=%s\nrecipient=%s\n",
            s.pending.available,s.pending.sequence,s.pending.sender_id,s.pending.recipient_id,Utf8(s.pending.sender).c_str(),Utf8(s.pending.recipient).c_str());
        if(extraCommand_&&extras_) {
            const auto e=extras_->State();
            fprintf(f,"[extra]\nconversation_count_before=%u\nconversation_count_after=%u\nselected_target_matched=%u\nprofile_constructed=%u\nprofile_enqueued=%u\nprofile_enqueue_count=%u\n",
                e.conversation_count_before,e.conversation_count_after,e.selected_target_matched,
                e.profile_constructed,e.profile_enqueued,e.profile_enqueue_count);
            fprintf(f,"profile_clone_created=%u\nprofile_clone_identity_matched=%u\nprofile_initialized=%u\nprofile_request_started=%u\nprofile_reply_consumed=%u\nprofile_reply_target_matched=%u\nprofile_reply_opcode=%u\nprofile_reply_target_id=%u\nprofile_destroyed=%u\n",
                e.profile_clone_created,e.profile_clone_identity_matched,e.profile_initialized,e.profile_request_started,
                e.profile_reply_consumed,e.profile_reply_target_matched,e.profile_reply_opcode,e.profile_reply_target_id,
                e.profile_destroyed);
        }
        for(unsigned i=0;i<std::min(s.friend_count,16u);++i) {
            const auto& row=s.friends[i];
            fprintf(f,"[friend%u]\nid=%u\nname=%s\nrelation=%u\npresence=%u\nconversation_open=%u\n",
                i,row.pilot_id,Utf8(row.nickname).c_str(),row.relationship,row.presence,row.conversation_open);
        }
        fclose(f);MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        lastWrite_=GetTickCount64();
    }
public:
    void Bind(FriendActions* actions,FriendExtraActions* extras,const SdgoApi* api,SdgoMod* mod){
        actions_=actions;extras_=extras;profile_=static_cast<uint8_t**>(api->Symbol(mod,"flow.profile_pointer"));
    }
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
        const auto observed=Observation();
        const bool terminal=observed.result==FriendActions::kReady || observed.result==FriendActions::kAlreadySatisfied ||
            observed.result==FriendActions::kReplyObserved || observed.result>=FriendActions::kBusy ||
            (observed.operation==FriendActions::kSendMessage && observed.result==FriendActions::kRequestSent && observed.request_sent);
        if(active_&&terminal) {
            if(!ready_) {
                if(observed.result>=FriendActions::kBusy){
                    Write(context,true);
                    char message[192];
                    sprintf_s(message,"native messenger initialization result=%u app=%u init=%u friends=%u request=%04X reply=%04X/%04X",
                        observed.result,observed.native_app_state,observed.messenger_init_state,observed.friend_count,
                        observed.request_opcode,observed.reply_opcode,observed.reply_status);
                    context->Observe(message);
                    context->Assert(0,"native messenger initialization failed");return SDGO_GAME_TEST_FAIL;
                }
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
        extraCommand_=command.operation>=10;
        const auto result=extraCommand_&&extras_?extras_->Submit(command):actions_->Submit(command);active_=true;commandAt_=GetTickCount64();
        if(result==FriendActions::kNativeFault){context->Assert(0,"native friend command could not be submitted");return SDGO_GAME_TEST_FAIL;}
        Write(context,false);return SDGO_GAME_TEST_WAIT;
    }
};
}
