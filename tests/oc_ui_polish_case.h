// Native UI regression. Uses the initialized client's input buffer and native
// control handlers; never creates a profile, inventory reply or screenshot.
template<class T> void UiPut(void* p,unsigned offset,T value) {
    std::memcpy(static_cast<unsigned char*>(p)+offset,&value,sizeof(value));
}
bool PreviewInput(const SdgoGameTestContextV1* c,void* factory,bool blocked) {
    auto** input=static_cast<void**>(api_->Symbol(mod_,"game_test.input_manager"));
    auto update=reinterpret_cast<void(__thiscall*)(void*)>(api_->Symbol(mod_,"oc_custom.client_factory_preview_input"));
    if(!input||!*input||!factory||!update)return false;
    auto* mouse=At<void*>(*input,8);auto* render=At<void*>(factory,0xD4);
    auto** first=At<void**>(render,0x21C);auto** end=At<void**>(render,0x220);
    const auto index=At<unsigned>(factory,0x1AC);
    if(!mouse||!first||first+index>=end)return false;
    auto* model=At<void*>(first[index],0x34);if(!model)return false;
    std::array<unsigned char,20> saved{};std::memcpy(saved.data(),static_cast<char*>(mouse)+0xC,saved.size());
    std::array<unsigned char,64> transform{};std::memcpy(transform.data(),static_cast<char*>(model)+0x50,transform.size());
    const auto zoom=At<float>(factory,0x198);
    // Coordinates inside the real model control, covered by the challenge.
    UiPut<int>(mouse,0x18,450);UiPut<int>(mouse,0x1C,360);UiPut<int>(mouse,0xC,0);
    UiPut<unsigned char>(mouse,0x10,2);update(factory);
    UiPut<int>(mouse,0x18,480);UiPut<unsigned char>(mouse,0x10,3);update(factory);
    const bool rotated=std::memcmp(transform.data(),static_cast<char*>(model)+0x50,transform.size())!=0;
    const auto dragging=At<unsigned>(factory,0x3F4);
    UiPut<unsigned char>(mouse,0x10,0);UiPut<int>(mouse,0xC,-120);update(factory);
    bool zoomed=zoom!=At<float>(factory,0x198);
    UiPut<int>(mouse,0xC,120);update(factory);
    zoomed=zoomed || zoom!=At<float>(factory,0x198);
    std::memcpy(static_cast<char*>(mouse)+0xC,saved.data(),saved.size());
    UiPut<unsigned>(factory,0x3F4,0);
    c->Assert(blocked?!rotated:rotated,blocked?"modal blocks native model rotation":"native model rotation works after closing modal");
    c->Assert(blocked?!zoomed:zoomed,blocked?"modal blocks native model zoom":"native model zoom works after closing modal");
    c->Assert(blocked?dragging==0:dragging==3,blocked?"modal cancels preview drag capture":"native preview accepts drag capture when modal closes");
    return (blocked?!rotated:rotated)&&(blocked?!zoomed:zoomed)&&(blocked?dragging==0:dragging==3);
}
void RadioMouse(void* p,unsigned event) {
    auto fn=reinterpret_cast<void(__thiscall*)(void*,unsigned,int,int,unsigned,unsigned)>(At<void**>(p)[19]);
    if(event!=WM_LBUTTONUP)fn(p,WM_MOUSEMOVE,-1,-1,0,0);
    fn(p,event,static_cast<int>(At<float>(p,0x148))+30,static_cast<int>(At<float>(p,0x14C))+25,0,0);
    reinterpret_cast<void(__thiscall*)(void*)>(At<void**>(p)[7])(p);
}
int PolishUi(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,revision=0,level=0;
    static ULONGLONG since=GetTickCount64();static bool requested=false;
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto getUi=reinterpret_cast<SdgoOcCustomClientUiV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientUiV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientUiV1 u{};u.size=sizeof(u);u.version=1;
    if(!observe||!getUi||!click||!observe(&s)||!getUi(&u)||!u.tab||!u.factory)return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-since<1500)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const wchar_t* name){
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);
        if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured the actual native UI frame");return true;
    };
    if(phase==0) {
        c->Assert(At<unsigned>(u.tab,0x134)==12&&At<void*>(u.tab)==At<void*>(u.reference_tab),"OC tab uses the same native radio class as its neighbours");
        c->Assert(At<unsigned char>(u.tab,0x210)==At<unsigned char>(u.reference_tab,0x210),"OC caption follows the native per-state text selection");
        auto** a=At<unsigned char**>(u.tab,0x190);auto** b=At<unsigned char**>(u.reference_tab,0x190);
        const auto count=At<unsigned char**>(u.tab,0x194)-a;
        c->Assert(count==At<unsigned char**>(u.reference_tab,0x194)-b,"OC tab preserves all native caption states");
        for(int i=0;i<count;++i) {
            bool same=true;
            for(unsigned offset:{0x48u,0x4Cu,0x50u,0x54u,0x58u,0x5Cu,0x68u,0x6Cu})
                same=same&&At<unsigned>(a[i],offset)==At<unsigned>(b[i],offset);
            same=same&&At<unsigned char>(a[i],0x64)==At<unsigned char>(b[i],0x64);
            c->Assert(same,"OC caption has native geometry, font and colour");
        }
        revision=s.revision;level=s.oc_level;
        if(!capture(L"oc-ui-selected"))return SDGO_GAME_TEST_WAIT;
        click("tab",L"UnitUpRadio");return next(1);
    }
    if(phase==1) {
        if(!requested) {RadioMouse(u.tab,WM_MOUSEMOVE);
        c->Assert(At<unsigned>(u.tab,0x18C)==1,"OC tab uses native hover state");}
        if(!capture(L"oc-ui-hover"))return SDGO_GAME_TEST_WAIT;
        return next(2);
    }
    if(phase==2) {
        if(!requested) {RadioMouse(u.tab,WM_LBUTTONDOWN);
        c->Assert(At<unsigned>(u.tab,0x18C)==2,"OC tab uses native pressed state");}
        if(!capture(L"oc-ui-pressed"))return SDGO_GAME_TEST_WAIT;
        RadioMouse(u.tab,WM_LBUTTONUP);return next(3);
    }
    if(phase==3) {
        c->Assert((s.windows&SDGO_OC_WINDOW_TAB_ACTIVE)!=0,"native mouse release opens the OC page");
        if(!PreviewInput(c,u.factory,false))return SDGO_GAME_TEST_FAIL;
        click("entry",L"OverCustomBtn");return next(4);
    }
    if(phase==4) {
        c->Assert((s.windows&SDGO_OC_WINDOW_CHALLENGE)!=0,"challenge panel opens through its normal button");
        if(!PreviewInput(c,u.factory,true))return SDGO_GAME_TEST_FAIL;
        c->Assert(At<int>(u.challenge,0x158)<650,"challenge height fits its actual item list");
        if(!capture(L"oc-ui-challenge"))return SDGO_GAME_TEST_WAIT;
        click("challenge",L"StartBtn");return next(5);
    }
    if(phase==5) {
        c->Assert((s.windows&SDGO_OC_WINDOW_CONFIRM)!=0,"native challenge confirmation remains required");
        if(!PreviewInput(c,u.factory,true))return SDGO_GAME_TEST_FAIL;
        if(!capture(L"oc-ui-confirm"))return SDGO_GAME_TEST_WAIT;
        click("confirm",L"CancelBtn");return next(6);
    }
    if(phase==6) {click("challenge",L"CloseBtn");return next(7);}
    if(phase==7) {
        if(!PreviewInput(c,u.factory,false))return SDGO_GAME_TEST_FAIL;
        c->Assert(!(s.windows&(SDGO_OC_WINDOW_CHALLENGE|SDGO_OC_WINDOW_CONFIRM)),"both modal windows close");
        c->Assert(s.revision==revision&&s.oc_level==level,"cancelled UI preview does not change OC progression");
        if(!capture(L"oc-ui-closed"))return SDGO_GAME_TEST_WAIT;
        return SDGO_GAME_TEST_PASS;
    }
    return SDGO_GAME_TEST_FAIL;
}
