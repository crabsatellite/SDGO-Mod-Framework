#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include "friend_actions.h"
#include "friend_extra_actions.h"
#include "friend_room_actions.h"
#include "unit_gift_actions.h"
#include "pvp_session_actions.h"
#include "sdgo/game_test.h"

namespace friends {

inline bool IsFriendCommandTerminal(
    const FriendActions::Observation& observed)
{
    return observed.result == FriendActions::kReady ||
        observed.result == FriendActions::kAlreadySatisfied ||
        observed.result == FriendActions::kReplyObserved ||
        observed.result >= FriendActions::kBusy ||
        ((observed.operation == FriendActions::kSendMessage ||
          observed.operation == FriendActions::kAddByName ||
          observed.operation == FriendActions::kAcceptPending ||
          observed.operation == FriendActions::kRejectPending ||
          observed.operation == FriendRoomActions::kSendInvite) &&
         observed.result == FriendActions::kRequestSent &&
         observed.request_sent);
}

// Bounded developer IPC for a single explicitly owned GameTest session. All
// actions still run on the native main thread through FriendActions.
class FriendSession {
    FriendActions* actions_=nullptr;
    FriendExtraActions* extras_=nullptr;
    FriendRoomActions* roomActions_=nullptr;
    PvpSessionActions* pvpActions_=nullptr;
    bool pvpCommand_=false;
    UnitGiftActions unitGifts_;
    uint8_t** profile_=nullptr;
    bool extraCommand_=false;
    bool roomCommand_=false;
    bool giftCommand_=false;
    uint32_t giftOperation_=0;
    FriendActions::Result giftResult_=FriendActions::kUnbound;
    uint32_t sequence_=0;
    bool active_=false,ready_=false;
    uint64_t lastWrite_=0,commandAt_=0;
    unsigned completed_=0;
    bool (*captureRequest_)(const wchar_t*)=nullptr;
    HRESULT (*captureStatus_)()=nullptr;
    bool captureCommand_=false,capturePending_=false;
    HRESULT captureResult_=E_PENDING;
    uint64_t captureAt_=0;
    std::wstring captureFile_;
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
        const wchar_t* names[]={L"",L"ensure",L"refresh",L"page",L"add",L"accept",L"reject",L"open",L"send",L"remove",L"close",L"profile",L"room_create",L"invite_send",L"invite_arm_accept",L"invite_arm_reject",L"invite_decide",L"room_leave",L"gift_context",L"gift_open",L"gift_select_unit",L"gift_note",L"gift_preview",L"gift_confirm",L"gift_cancel",L"gift_storage",L"gift_inbox",L"gift_returned",L"gift_select",L"gift_accept",L"gift_return",L"gift_takeback",L"gift_provenance",L"gift_receipt",L"room_join",L"room_ready",L"room_start",L"battle_finish"};
        for(uint32_t i=1;i<38;++i)if(!wcscmp(text,names[i]))return i;
        return 0;
    }
    FriendActions::Observation Observation() const {
        auto state=actions_->State();
        if(captureCommand_) {
            state.result=SUCCEEDED(captureResult_)?FriendActions::kReady:FriendActions::kNativeFault;
            state.command_sequence=sequence_;state.operation=0;
            state.request_sent=state.reply_consumed=0;
            state.request_opcode=state.reply_opcode=state.reply_status=0;
        } else if(pvpCommand_&&pvpActions_) {
            const auto p=pvpActions_->State();state.result=p.result;state.operation=p.operation;state.command_sequence=p.sequence;
            state.request_sent=state.reply_consumed=0;
            state.request_opcode=state.reply_opcode=state.reply_status=0;
        } else if(giftCommand_) {
            state.result=giftResult_;state.command_sequence=sequence_;state.operation=giftOperation_;
            state.request_sent=0;state.request_opcode=state.reply_opcode=state.reply_status=0;
            state.reply_consumed=0;
        } else if(roomCommand_&&roomActions_) {
            const auto e=roomActions_->State();
            state.result=e.result;state.command_sequence=e.command_sequence;state.operation=e.operation;
            state.target_id=e.target_id;memcpy(state.target_name,e.target_name,sizeof(state.target_name));
            state.request_sent=e.request_sent;state.request_opcode=e.request_opcode;
            state.request_transport_sequence=e.request_transport_sequence;
            state.reply_opcode=e.reply_opcode;state.reply_status=e.reply_status;
            state.reply_consumed=e.reply_consumed;state.reply_sequence=e.reply_sequence;state.reply_result=e.reply_result;
        } else if(extraCommand_&&extras_) {
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
    static bool Number64(const wchar_t* file,const wchar_t* key,uint64_t* output) {
        wchar_t text[32]{};
        const auto count=GetPrivateProfileStringW(L"command",key,L"0",text,32,file);
        if(!count||count>=31)return false;
        uint64_t value=0;
        for(unsigned i=0;i<count;++i) {
            if(text[i]<L'0'||text[i]>L'9')return false;
            const uint64_t digit=text[i]-L'0';
            if(value>(UINT64_MAX-digit)/10)return false;
            value=value*10+digit;
        }
        *output=value;return true;
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
        fprintf(f,"[guards]\nadd_list_valid=%u\nadd_class_matches=%u\nadd_root_ready=%u\nadd_edit_ready=%u\nadd_button_match=%u\nadd_button_parent=%u\nadd_button_visible=%u\nadd_button_enabled=%u\nadd_button_hit_test=%u\nadd_callback_invoked=%u\npending_list_valid=%u\npending_class_matches=%u\npending_packet_matches=%u\npending_target_matches=%u\npending_root_ready=%u\npending_sender_id=%u\npending_recipient_id=%u\npending_button_match=%u\npending_button_parent=%u\npending_button_visible=%u\npending_button_enabled=%u\npending_button_hit_test=%u\npending_callback_invoked=%u\n",
            s.add_dialog_list_valid,s.add_dialog_class_matches,
            s.add_dialog_root_ready,s.add_dialog_edit_ready,
            s.add_button_match,s.add_button_parent,
            s.add_button_visible,s.add_button_enabled,
            s.add_button_hit_test,
            s.add_callback_invoked,
            s.pending_dialog_list_valid,s.pending_dialog_class_matches,
            s.pending_dialog_packet_matches,s.pending_dialog_target_matches,
            s.pending_dialog_root_ready,s.pending_dialog_sender_id,
            s.pending_dialog_recipient_id,
            s.pending_button_match,s.pending_button_parent,
            s.pending_button_visible,s.pending_button_enabled,
            s.pending_button_hit_test,
            s.pending_callback_invoked);
        fprintf(f,"[state]\napp=%u\npilot=%u\ninit=%u\npage=%u\nfriend_count=%u\nconversation_count=%u\nmessage_observed=%u\nlast_sender=%s\nlast_message=%s\n",
            s.native_app_state,s.profile_pilot_id,s.messenger_init_state,s.current_page,s.friend_count,s.conversation_count,
            s.message_observed,Utf8(s.last_sender).c_str(),Utf8(s.last_message).c_str());
        fprintf(f,"list_page_replies=%u\nlist_total_count=%u\nlist_entries_seen=%u\n",
            s.list_page_replies,s.list_total_count,s.list_entries_seen);
        fprintf(f,"message_applied=%u\nmessage_application_status=%u\nmessage_sender_pilot=%u\nmessage_before_lines=%u\nmessage_after_lines=%u\nmessage_online_flag=%u\n",
            s.message_applied,s.message_application_status,s.message_sender_pilot,
            s.message_before_lines,s.message_after_lines,s.message_online_flag);
        char own[17]{};OwnName(own);fprintf(f,"own_name=%s\n",Utf8(own).c_str());
        fprintf(f,"[pending]\navailable=%u\nsequence=%u\nsender_id=%u\nrecipient_id=%u\nsender=%s\nrecipient=%s\n",
            s.pending.available,s.pending.sequence,s.pending.sender_id,s.pending.recipient_id,Utf8(s.pending.sender).c_str(),Utf8(s.pending.recipient).c_str());
        if(extraCommand_&&extras_&&!roomCommand_) {
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
        if(roomActions_)roomActions_->Write(f);
        unitGifts_.Write(f,context);
        fprintf(f,"[capture]\npending=%u\nhresult=%lu\n",capturePending_?1u:0u,static_cast<unsigned long>(captureResult_));
        if(pvpActions_)pvpActions_->Append(f);
        fclose(f);MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
        lastWrite_=GetTickCount64();
    }
public:
    void BindFrameCapture(bool (*request)(const wchar_t*),HRESULT (*status)()) {
        captureRequest_=request;captureStatus_=status;
    }
    void Bind(FriendActions* actions,FriendExtraActions* extras,const SdgoApi* api,SdgoMod* mod,FriendRoomActions* roomActions=nullptr){
        actions_=actions;extras_=extras;roomActions_=roomActions;profile_=static_cast<uint8_t**>(api->Symbol(mod,"flow.profile_pointer"));
    }
    void BindPvp(PvpSessionActions* pvp){pvpActions_=pvp;}
    int Poll(const SdgoGameTestContextV1* context) {
        if(!actions_){context->Assert(0,"friend action adapter is unavailable");return SDGO_GAME_TEST_FAIL;}
        const std::wstring abortPath=std::wstring(context->directory)+L"\\friend-abort.ini";
        wchar_t abortId[64]{};
        GetPrivateProfileStringW(L"run",L"id",L"",abortId,64,abortPath.c_str());
        const std::wstring expectedRun(context->run_id,context->run_id+strlen(context->run_id));
        if(expectedRun==abortId){context->Assert(0,"owned friend campaign requested a bounded abort");return SDGO_GAME_TEST_FAIL;}
        if(capturePending_) {
            captureResult_=captureStatus_();
            if(captureResult_==E_PENDING && GetTickCount64()-captureAt_<30000)return SDGO_GAME_TEST_WAIT;
            if(captureResult_==E_PENDING)captureResult_=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            capturePending_=false;
            context->Assert(SUCCEEDED(captureResult_),"the actual game backbuffer was captured before Present");
            if(SUCCEEDED(captureResult_))++completed_;
            Write(context,true);
            return SUCCEEDED(captureResult_)?SDGO_GAME_TEST_WAIT:SDGO_GAME_TEST_FAIL;
        }
        if(!ready_&&!active_) {
            const auto result=actions_->EnsureInitialized();
            if(result>=FriendActions::kBusy){context->Assert(0,"native messenger initialization could not start");return SDGO_GAME_TEST_FAIL;}
            active_=true;commandAt_=GetTickCount64();
        }
        actions_->Tick();
        if(roomActions_)roomActions_->Tick(context);
        if(pvpActions_)pvpActions_->Tick(context);
        const auto observed=Observation();
        const bool terminal=IsFriendCommandTerminal(observed);
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
        const uint64_t commandTimeout=pvpCommand_&&pvpActions_&&pvpActions_->State().operation==PvpSessionActions::FinishBattle?900000:90000;
        if(active_&&GetTickCount64()-commandAt_>commandTimeout) {
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
        captureCommand_=!wcscmp(op,L"capture");
        if(captureCommand_) {
            wchar_t label[48]{};
            const auto count=GetPrivateProfileStringW(L"command",L"label",L"",label,48,commandPath.c_str());
            bool valid=count>0 && count<47;
            for(unsigned i=0;i<count;++i)
                valid=valid && ((label[i]>=L'a'&&label[i]<=L'z')||(label[i]>=L'A'&&label[i]<=L'Z')||
                    (label[i]>=L'0'&&label[i]<=L'9')||label[i]==L'-'||label[i]==L'_');
            captureFile_=std::wstring(context->directory)+L"\\capture-"+label+L".bmp";
            if(!valid||!captureRequest_||!captureStatus_||!captureRequest_(captureFile_.c_str())) {
                captureResult_=E_INVALIDARG;Write(context,true);
                context->Assert(0,"a scoped native frame capture could not be requested");return SDGO_GAME_TEST_FAIL;
            }
            captureResult_=E_PENDING;capturePending_=true;captureAt_=GetTickCount64();
            Write(context,false);return SDGO_GAME_TEST_WAIT;
        }
        FriendActions::Command command{};command.sequence=sequence_;command.operation=Operation(op);
        command.target_id=GetPrivateProfileIntW(L"command",L"target_id",0,commandPath.c_str());
        command.page=GetPrivateProfileIntW(L"command",L"page",1,commandPath.c_str());
        if(!command.operation || !Text(commandPath.c_str(),L"target_name",command.target_name,sizeof(command.target_name)) ||
            !Text(commandPath.c_str(),L"body",command.body,sizeof(command.body))) {
            context->Assert(0,"friend command is malformed");return SDGO_GAME_TEST_FAIL;
        }
        pvpCommand_=command.operation>=34;
        giftCommand_=command.operation>=18&&command.operation<=33;
        roomCommand_=command.operation>=12&&!giftCommand_&&!pvpCommand_;
        extraCommand_=command.operation>=10&&!roomCommand_&&!giftCommand_&&!pvpCommand_;
        const uint32_t channel=GetPrivateProfileIntW(L"command",L"channel_id",0,commandPath.c_str());
        const uint32_t room=GetPrivateProfileIntW(L"command",L"room_id",0,commandPath.c_str());
        if(roomCommand_&&(!roomActions_||channel>UINT16_MAX)){context->Assert(0,"friend room command lacks a supported native bridge or channel");return SDGO_GAME_TEST_FAIL;}
        FriendActions::Result result;
        if(pvpCommand_) {
            const uint32_t humans=GetPrivateProfileIntW(L"command",L"human_count",0,commandPath.c_str());
            if(!pvpActions_){context->Assert(0,"coordinated native PvP adapter is unavailable");return SDGO_GAME_TEST_FAIL;}
            const uint32_t group=GetPrivateProfileIntW(L"command",L"channel_group",UINT32_MAX,commandPath.c_str());
            if(channel>UINT16_MAX){context->Assert(0,"coordinated native channel id is invalid");return SDGO_GAME_TEST_FAIL;}
            result=pvpActions_->Submit(command,humans,context,group,static_cast<uint16_t>(channel));
        } else if(giftCommand_) {
            uint64_t unit=0,gift=0;uint16_t note[200]{},noteLength=0;wchar_t hex[802]{};
            const auto hexLength=GetPrivateProfileStringW(L"command",L"note_utf16_hex",L"",hex,802,commandPath.c_str());
            if(hexLength>=801||!Number64(commandPath.c_str(),L"unit_uid",&unit)||
                !Number64(commandPath.c_str(),L"gift_id",&gift)||
                !UnitGiftActions::DecodeHexNote(hex,note,&noteLength)) {
                context->Assert(0,"unit gift action input is malformed");return SDGO_GAME_TEST_FAIL;
            }
            giftOperation_=command.operation;
            result=giftResult_=unitGifts_.Invoke(context,command.operation-17,command.target_id,unit,gift,note,noteLength);
        } else result=roomCommand_?roomActions_->Submit(command,static_cast<uint16_t>(channel),room):
            extraCommand_&&extras_?extras_->Submit(command):actions_->Submit(command);
        active_=true;commandAt_=GetTickCount64();
        if(result==FriendActions::kNativeFault){context->Assert(0,"native friend command could not be submitted");return SDGO_GAME_TEST_FAIL;}
        Write(context,false);return SDGO_GAME_TEST_WAIT;
    }
};
}
