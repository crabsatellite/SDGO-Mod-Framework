// Included by the native OC item fixture. Opt-in: changes the selected
// isolated test unit and consumes the test account's actual owned cards.
// No synthetic levels, inventory replies or frames are supplied by this case.
int FullChallenge(const SdgoGameTestContextV1* c) {
    static unsigned phase=0, level=8, initialCards=0, upgrades=0, used=0, frames=0;
    static ULONGLONG started=GetTickCount64(), phaseAt=started, lastFrame=0;
    static bool recording=false;
    const auto now=GetTickCount64();
    if(recording) {
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);
        if(state==QuietFrameCapture::Failed) {c->Assert(false,"continuous native frame capture succeeded");return SDGO_GAME_TEST_FAIL;}
        if(now-lastFrame>=200 && state!=QuietFrameCapture::Pending && state!=QuietFrameCapture::Capturing) {
            wchar_t name[80];swprintf_s(name,L"\\oc-ex-%06u-%08llu.bmp",frames,now-started);
            if(!visualFrame.Request((std::wstring(c->directory)+name).c_str()))return SDGO_GAME_TEST_FAIL;
            ++frames;lastFrame=now;
        }
    }
    auto module=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(module,"SdgoOcCustomClientGetSnapshotV2"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(module,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(module,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=SDGO_OC_CUSTOM_CLIENT_OBSERVER_VERSION;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!items||!click||!observe(&s)||!items(&v))return SDGO_GAME_TEST_FAIL;
    if(s.busy||now-phaseAt<1000)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;phaseAt=now;return SDGO_GAME_TEST_WAIT;};
    const auto require=[&](bool ok,const char* message){c->Assert(ok,message);return ok;};
    int card=-1;for(unsigned i=0;i<10;++i)if(v.owned[i]==3240005)card=static_cast<int>(i);
    if(phase==0) {
        if(!require(s.oc_level==8 && (s.windows&SDGO_OC_WINDOW_ENTRY_LIT),"the existing test unit starts at normal maximum and can challenge"))return SDGO_GAME_TEST_FAIL;
        if(!require(card>=0 && v.counts[card]>=6,"the existing account owns six or more 100-percent success cards"))return SDGO_GAME_TEST_FAIL;
        initialCards=v.counts[card];recording=true;started=now;
        return next(1);
    }
    if(phase==1) {
        if(!v.list_visible)click("over",L"OcItemListBtn");
        return next(2);
    }
    if(phase==2) {
        if(!require(v.list_visible&&v.ready&&card>=0,"the native inventory displays the server-owned success card"))return SDGO_GAME_TEST_FAIL;
        if(level>=9)click("items",(L"Item3DRender"+std::to_wstring(card)).c_str());
        return next(3);
    }
    if(phase==3) {
        if(!require(level==8 ? !s.challenge_selected : v.placed[0]==3240005&&s.item_models_shown==1,
                    "OC1 uses its native guaranteed rate; every later step selects exactly one success card"))return SDGO_GAME_TEST_FAIL;
        click("entry",L"OverCustomBtn");return next(4);
    }
    if(phase==4) {
        if(!require((s.windows&SDGO_OC_WINDOW_CHALLENGE)!=0,"native challenge window opens for the next level"))return SDGO_GAME_TEST_FAIL;
        click("challenge",L"StartBtn");return next(5);
    }
    if(phase==5) {
        if(!require((s.windows&SDGO_OC_WINDOW_CONFIRM)!=0,"the native confirmation is visible before consuming a card"))return SDGO_GAME_TEST_FAIL;
        click("confirm",L"OKBtn");return next(6);
    }
    if(phase==6) {
        if(!require(s.oc_level==level+1&&s.last_upgrade_success==1&&s.confirm_text_id==126704,
                    "the server confirms one successful OC level and displays the native success result"))return SDGO_GAME_TEST_FAIL;
        if(level>=9)++used;
        ++level;++upgrades;
        char message[120];sprintf_s(message,"actual OC progression level=%u completed=%u success_cards_consumed=%u",level,upgrades,used);c->Observe(message);
        return next(7);
    }
    if(phase==7) {click("confirm",L"OKBtn");return next(8);}
    if(phase==8) {
        const unsigned remaining=card>=0 ? static_cast<unsigned>(v.counts[card]):0;
        if(!require(remaining==initialCards-used,"server inventory decreases by exactly one card per eligible upgrade"))return SDGO_GAME_TEST_FAIL;
        if(level<15)return next(1);
        if(!require(upgrades==7&&used==6&&s.info_levels==7,"seven actual upgrades reach EX and light all seven native OC indicators"))return SDGO_GAME_TEST_FAIL;
        if(v.list_visible)click("items",L"OcItemsRadarBtn");
        return next(9);
    }
    if(phase==9) {
        if(now-phaseAt<4000)return SDGO_GAME_TEST_WAIT;
        if(!require(s.oc_level==15&&!(s.windows&SDGO_OC_WINDOW_ENTRY_LIT),"EX remains at the cap with no further challenge available"))return SDGO_GAME_TEST_FAIL;
        recording=false;return next(10);
    }
    if(visualFrame.State()==QuietFrameCapture::Pending||visualFrame.State()==QuietFrameCapture::Capturing)return SDGO_GAME_TEST_WAIT;
    c->Assert(frames>100,"recorded the complete native progression continuously, including every confirmation and result");
    return frames>100 ? SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
}
