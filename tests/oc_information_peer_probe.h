// Opt-in native two-client inspection. Records come from the live room model;
// the original card constructor receives exactly the stock owner/peer flag.
int(__cdecl* peerSession)(const SdgoGameTestContextV1*) = nullptr;
sdgo::oc_custom::Information peerInformation;
bool peerReplyReady=false, peerReplySent=false;
int __cdecl PeerReady(const SdgoGameTestContextV1* context) {
    static ULONGLONG start=GetTickCount64();static bool captured=false;
    if(!captured && GetTickCount64()-start>15000) {
        auto runtime=GetModuleHandleW(L"game_test_client.dll");
        auto capture=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));
        const auto path=std::wstring(context->directory)+L"\\onboarding-diagnostic.bmp";
        captured=capture && capture(path.c_str())!=0;
    }
    return nativeOnboarding(context);
}
void __cdecl PeerInformationReply(void*,std::uint16_t command,std::int32_t status,
                                 const std::uint8_t* data,std::uint32_t bytes) {
    int result=-1;
    peerReplyReady=command==static_cast<std::uint16_t>(sdgo::oc_custom::Command::information) && !status &&
        sdgo::oc_custom::ReadInformationReply(data,bytes,&result,&peerInformation) && !result;
}
const std::uint8_t* PeerRoom() {
    using App=std::uint8_t*(__cdecl*)();
    const auto app=reinterpret_cast<App>(api_->Symbol(mod_,"game.app"));
    auto* a=app?app():nullptr;
    if(!a || At<unsigned>(a,0x4A0)!=18)return nullptr;
    auto* state=At<void*>(a,0x4A4);
    if(!state || At<void*>(state)!=api_->Symbol(mod_,"flow.three_d_wait_room_vtable"))return nullptr;
    auto* manager=At<void*>(state,4);auto* model=manager?At<void*>(manager,0x38):nullptr;
    using Info=std::uint8_t*(__thiscall*)(void*);
    const auto info=reinterpret_cast<Info>(api_->Symbol(mod_,"flow.mission_room_info"));
    if(!model || At<Info>(At<void*>(model),0x1C)!=info)return nullptr;
    return info(model);
}
int __cdecl PeerCapture(const SdgoGameTestContextV1* context) {
    const int session=peerSession(context);
    if(session==SDGO_GAME_TEST_FAIL)return session;
    static bool identitySaved=false;
    auto** ownProfile=static_cast<std::uint8_t**>(api_->Symbol(mod_,"hangar.profile_global"));
    if(!identitySaved && ownProfile && *ownProfile && At<std::uint64_t>(*ownProfile,0x1B1)) {
        FILE* f=nullptr;const auto path=std::wstring(context->directory)+L"\\information-own-unit.txt";
        if(!_wfopen_s(&f,path.c_str(),L"wb") && f) {
            std::fprintf(f,"%llu",At<std::uint64_t>(*ownProfile,0x1B1));std::fclose(f);identitySaved=true;
        }
    }
    static unsigned stage=0;
    static bool opened=false,requested=false;
    static ULONGLONG visibleAt=0;
    static ULONGLONG roomAt=0,lastOpenAttempt=0;
    static std::uint64_t inspected=0;
    static unsigned inspectionIndex=0;
    static bool firstRequested=false,firstCaptured=false;
    wchar_t target[32]{},plan[MAX_PATH]{};
    GetEnvironmentVariableW(L"SDGO_GAME_TEST_PLAN",plan,MAX_PATH);
    GetPrivateProfileStringW(L"run",L"information_target",L"",target,32,plan);
    const auto uid=_wcstoui64(target,nullptr,10);
    if(stage==2) {
        if(!uid || uid==inspected)return session;
        stage=0;
        ++inspectionIndex;
        peerReplySent=peerReplyReady=firstRequested=firstCaptured=false;
        peerInformation={};
    }
    if(!stage && !opened)inspected=uid;
    const auto outputPrefix=std::wstring(context->directory)+L"\\information-"+
        (inspectionIndex ? std::to_wstring(inspectionIndex)+L"-" : L"");
    const auto* room=PeerRoom();
    if(!uid || !room || room[5]==5 || room[5]==8)return SDGO_GAME_TEST_WAIT;
    auto** profile=static_cast<std::uint8_t**>(api_->Symbol(mod_,"hangar.profile_global"));
    if(!profile || !*profile)return SDGO_GAME_TEST_WAIT;
    const bool own=At<std::uint64_t>(*profile,0x1B1)==uid;
    const std::uint8_t* record=nullptr;unsigned occupied=0;
    for(unsigned slot=0;slot<12;++slot) {
        const auto* entry=room+0x533+slot*0x48E;
        if(At<unsigned>(entry,0x429))++occupied;
        if(At<std::uint64_t>(entry,0x51)==uid && At<unsigned>(entry,0x429))record=entry+0x49;
    }
    if(occupied!=2 || !record)return SDGO_GAME_TEST_WAIT;
    if(!roomAt)roomAt=GetTickCount64();
    if(GetTickCount64()-roomAt<3000)return SDGO_GAME_TEST_WAIT;
    if(!opened) {
        if(GetTickCount64()-lastOpenAttempt<1000)return SDGO_GAME_TEST_WAIT;
        lastOpenAttempt=GetTickCount64();
        using Alloc=void*(__cdecl*)(std::size_t);
        using Ctor=void*(__thiscall*)(void*,const void*,unsigned);
        using Enqueue=void*(__cdecl*)(void*,unsigned);
        using Dtor=void(__thiscall*)(void*);
        using Free=void(__cdecl*)(void*);
        auto alloc=reinterpret_cast<Alloc>(api_->Symbol(mod_,"friend_unit_gift.native_alloc"));
        auto ctor=reinterpret_cast<Ctor>(api_->Symbol(mod_,"friend_unit_gift.native_card_ctor"));
        auto enqueue=reinterpret_cast<Enqueue>(api_->Symbol(mod_,"friend_unit_gift.enqueue_ui"));
        auto dtor=reinterpret_cast<Dtor>(api_->Symbol(mod_,"friend_unit_gift.native_card_dtor"));
        auto free=reinterpret_cast<Free>(api_->Symbol(mod_,"friend_unit_gift.native_free"));
        auto* temporary=alloc(0x148);
        if(!temporary)return SDGO_GAME_TEST_FAIL;
        ctor(temporary,own?*profile+0x1A9:record,own?0:1);
        opened=enqueue(temporary,1)!=nullptr;
        dtor(temporary);free(temporary);
        if(opened)context->Assert(1,own?"opened original owner card from native profile":"opened original peer card from server-loaded room participant");
        return SDGO_GAME_TEST_WAIT;
    }
    auto* service=currentCard;auto* root=service?At<void*>(service,0x6C):nullptr;
    if(!root || !At<std::uint8_t>(root,0x162) || At<std::uint64_t>(service,0x94)!=uid)return SDGO_GAME_TEST_WAIT;
    if(!visibleAt)visibleAt=GetTickCount64();
    auto runtime=GetModuleHandleW(L"game_test_client.dll");
    auto capture=reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(runtime,"SdgoGameTestCaptureFrameV1"));
    auto status=reinterpret_cast<LONG(__cdecl*)(HRESULT*)>(GetProcAddress(runtime,"SdgoGameTestCaptureStatusV1"));
    if(!stage && !firstCaptured && GetPrivateProfileIntW(L"run",L"information_first_frame",0,plan)) {
        auto* panel=Control(root,L"OcInformationRadarPanel",4);
        if(!panel || !At<std::uint8_t>(panel,0x162))return SDGO_GAME_TEST_WAIT;
        if(!firstRequested) {
            const auto path=outputPrefix+L"first.bmp";
            firstRequested=capture && capture(path.c_str())!=0;
            return firstRequested?SDGO_GAME_TEST_WAIT:SDGO_GAME_TEST_FAIL;
        }
        HRESULT hr=E_PENDING;
        const auto captureState=status?status(&hr):4;
        if(captureState!=3 && captureState!=4)return SDGO_GAME_TEST_WAIT;
        firstCaptured=captureState==3 && SUCCEEDED(hr);
        context->Assert(firstCaptured,"captured the first observed visible radar of the selected unit");
        if(!firstCaptured)return SDGO_GAME_TEST_FAIL;
    }
    if(!peerReplySent) {
        auto module=GetModuleHandleW(L"friend_unit_gift_client.dll");
        auto send=reinterpret_cast<sdgo::friend_unit_gift::ClientExtensionSendFn>(GetProcAddress(module,SDGO_FRIEND_UNIT_GIFT_CLIENT_EXTENSION_SEND_EXPORT));
        std::uint8_t data[sdgo::oc_custom::kUnitRequestBytes]{};
        sdgo::oc_custom::UnitRequest query;query.unit_uid=uid;sdgo::oc_custom::WriteUnitRequest(data,query);
        peerReplySent=send && send(static_cast<std::uint16_t>(sdgo::oc_custom::Command::information),data,sizeof(data),&PeerInformationReply,nullptr)!=0;
    }
    if(!peerReplyReady || GetTickCount64()-visibleAt<5000)return SDGO_GAME_TEST_WAIT;
    if(!requested) {
        bool correct=peerInformation.profile.unit_uid==uid;
        auto* child=At<std::uint8_t*>(service,0x13C);
        correct=correct && child && At<std::uint8_t>(child,0x94+136)==peerInformation.g2_slots;
        for(unsigned i=0;i<4 && correct;++i) {
            correct=At<std::uint16_t>(child,0x94+138+i*2)==peerInformation.g2_skills[i];
            auto* icon=Control(root,L"OcInformationG1Icon"+std::to_wstring(i),1);
            correct=correct && icon && bool(At<std::uint8_t>(icon,0x162))==bool(peerInformation.g1_skills[i]);
        }
        for(const auto* name:{L"GBlindIcon",L"GBlindStatic",L"AttributeSkillBlindIcon"}) {
            auto* c=Control(root,name,std::wcscmp(name,L"GBlindStatic")?1:3);
            correct=correct && (!c || !At<std::uint8_t>(c,0x162));
        }
        context->Assert(correct,"native G1 visibility and G2 slots/skills match the independent server information reply");
        if(!correct)return SDGO_GAME_TEST_FAIL;
        const auto path=outputPrefix+std::to_wstring(stage)+L".bmp";
        requested=capture && capture(path.c_str())!=0;
        return requested?SDGO_GAME_TEST_WAIT:SDGO_GAME_TEST_FAIL;
    }
    HRESULT hr=E_PENDING;const auto captureState=status?status(&hr):4;
    if(captureState!=3 && captureState!=4)return SDGO_GAME_TEST_WAIT;
    context->Assert(captureState==3 && SUCCEEDED(hr),"captured genuine owner/peer information backbuffer");
    if(captureState!=3 || FAILED(hr))return SDGO_GAME_TEST_FAIL;
    using Callback=unsigned(__stdcall*)(void*,unsigned,void*,void*);
    auto close=reinterpret_cast<Callback>(api_->Symbol(mod_,"friend_unit_gift.native_card_callback"));
    auto* button=Control(root,L"CloseBtn",4);if(!button || !close)return SDGO_GAME_TEST_FAIL;
    currentCard=nullptr;close(service,0x101,static_cast<std::uint8_t*>(button)+0x118,button);
    ++stage;opened=requested=false;visibleAt=0;
    if(stage==2) {
        std::array<std::uint8_t,sdgo::oc_custom::kInformationReplyBytes> bytes{};
        sdgo::oc_custom::WriteInformationReply(bytes.data(),0,peerInformation);
        FILE* f=nullptr;const auto path=outputPrefix+L"proof.bin";
        if(_wfopen_s(&f,path.c_str(),L"wb") || !f)return SDGO_GAME_TEST_FAIL;
        const bool saved=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();std::fclose(f);
        context->Assert(saved,"saved native server snapshot after closing and reopening the card");
    }
    return SDGO_GAME_TEST_WAIT;
}
