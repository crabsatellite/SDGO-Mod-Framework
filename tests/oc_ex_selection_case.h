// Real native EX selection after FullChallenge; never writes profile or inventory.
int ExSelection(const SdgoGameTestContextV1* c) {
    static unsigned phase=0, balance=0, revision=0, requests=0;
    static ULONGLONG phaseAt=GetTickCount64();
    static ULONGLONG recordedAt=0;
    static unsigned frame=0;
    if(GetTickCount64()-recordedAt>=200) {
        wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\oc-ex-selection-%04u.bmp",c->directory,frame++);
        if(visualFrame.Request(path))recordedAt=GetTickCount64();
    }
    auto module=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV2"));
    auto ex=reinterpret_cast<SdgoOcCustomClientExV1Fn>(GetProcAddress(module,"SdgoOcCustomClientExV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(module,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientExV1 e{};e.size=sizeof(e);e.version=1;
    if(!observe||!ex||!click||!observe(&s)||!ex(&e))return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-phaseAt<1200)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned p){phase=p;phaseAt=GetTickCount64();return SDGO_GAME_TEST_WAIT;};
    const auto require=[&](bool ok,const char* text){c->Assert(ok,text);return ok;};
    if(phase==0) {
        if(!require(s.oc_level==15&&e.available&&e.visible&&e.known,"reaching EX automatically opens the authoritative eight-choice skill page"))return SDGO_GAME_TEST_FAIL;
        if(!require(e.ability==0&&e.remaining>=1&&e.cost==1&&e.enabled_choices==255,"new EX unit offers eight abilities and the actual modifier balance"))return SDGO_GAME_TEST_FAIL;
        balance=e.remaining;revision=s.revision;requests=s.request_count;
        visualFrame.Request((std::wstring(c->directory)+L"\\ex-choices.bmp").c_str());
        click("ex",L"ExBtn0");return next(1);
    }
    if(phase==1) {
        if(!require((s.windows&SDGO_OC_WINDOW_CONFIRM)&&s.revision==revision&&s.request_count==requests,"selection opens confirmation without sending a mutation"))return SDGO_GAME_TEST_FAIL;
        click("confirm",L"CancelBtn");return next(2);
    }
    if(phase==2) {
        if(!require(e.visible&&e.ability==0&&e.remaining==balance&&s.revision==revision&&s.request_count==requests,"cancelling keeps ability, inventory and revision unchanged"))return SDGO_GAME_TEST_FAIL;
        click("ex",L"ExBtn0");return next(3);
    }
    if(phase==3) {click("confirm",L"OKBtn");return next(4);}
    if(phase==4) {
        if(!require(e.last_ret_val==0&&e.ability==1&&e.remaining+1==balance&&s.revision==revision+1,
                    "real server ACK equips attack EX, consumes exactly one modifier and advances revision once"))return SDGO_GAME_TEST_FAIL;
        c->Observe("EX ability=1; modifier delta=-1; revision delta=1; authoritative ex_set ACK accepted");
        click("confirm",L"OKBtn");return next(5);
    }
    if(phase==5) {click("items",L"OcItemsRadarBtn");return next(50);}
    if(phase==50) {click("over",L"ExBtn");return next(6);}
    if(phase==6) {
        if(!require(e.visible&&e.known&&e.ability==1&&e.remaining+1==balance&&e.enabled_choices==254,"reopening reads the persisted ability and disables the equipped choice"))return SDGO_GAME_TEST_FAIL;
        requests=s.request_count;click("ex",L"ExBtn0");return next(7);
    }
    if(!require(s.request_count==requests&&s.revision==revision+1&&e.remaining+1==balance,"reselecting the current ability sends no request and consumes nothing"))return SDGO_GAME_TEST_FAIL;
    click("ex",L"CloseBtn");return SDGO_GAME_TEST_PASS;
}

// Re-login verification on the already upgraded unit; no challenge or charge.
int ExReopen(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,requests=0,revision=0,balance=0;
    static ULONGLONG at=GetTickCount64();
    auto module=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV2"));
    auto ex=reinterpret_cast<SdgoOcCustomClientExV1Fn>(GetProcAddress(module,"SdgoOcCustomClientExV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(module,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientExV1 e{};e.size=sizeof(e);e.version=1;
    if(!observe||!ex||!click||!observe(&s)||!ex(&e))return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-at<1500)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned p){phase=p;at=GetTickCount64();return SDGO_GAME_TEST_WAIT;};
    const auto require=[&](bool ok,const char* text){c->Assert(ok,text);return ok;};
    if(phase==0){
        if(!require(s.oc_level==15&&e.ability==1,"re-login loads the actual saved EX ability"))return SDGO_GAME_TEST_FAIL;
        click("items",L"OcItemsRadarBtn");return next(1);
    }
    if(phase==1){click("over",L"ExBtn");return next(2);}
    if(phase==2){
        char detail[160];sprintf_s(detail,"EX reopen visible=%u known=%u ability=%u balance=%u choices=%02X",e.visible,e.known,e.ability,e.remaining,e.enabled_choices);c->Observe(detail);
        if(!require(e.visible&&e.known&&e.ability==1&&e.enabled_choices==254,"reopened EX page disables the equipped ability and enables the other seven"))return SDGO_GAME_TEST_FAIL;
        requests=s.request_count;revision=s.revision;balance=e.remaining;
        visualFrame.Request((std::wstring(c->directory)+L"\\ex-reopened.bmp").c_str());
        return next(3);
    }
    if(phase==3){click("ex",L"ExBtn0");return next(4);}
    if(phase==4){
        if(!require(s.request_count==requests&&s.revision==revision&&e.remaining==balance,"reselecting the equipped skill sends no mutation and spends no item"))return SDGO_GAME_TEST_FAIL;
        click("ex",L"ExBtn1");return next(5);
    }
    if(phase==5){click("confirm",L"CancelBtn");return next(6);}
    if(!require(e.visible&&e.ability==1&&s.request_count==requests&&s.revision==revision&&e.remaining==balance,"cancelling a replacement preserves the equipped EX skill and inventory"))return SDGO_GAME_TEST_FAIL;
    click("ex",L"CloseBtn");return SDGO_GAME_TEST_PASS;
}
