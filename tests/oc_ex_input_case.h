int ExInput(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,revision=0,ability=0,balance=0,requests=0;
    static ULONGLONG since=GetTickCount64();static bool requested=false;
    static ShopPointerInput pointer;
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto ex=reinterpret_cast<SdgoOcCustomClientExV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientExV1"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientExV1 e{};e.size=sizeof(e);e.version=1;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!ex||!items||!click||!observe(&s)||!ex(&e)||!items(&v))return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-since<1500)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const wchar_t* name){
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured the real EX entry and choices");return true;
    };
    if(phase==0){
        auto* panel=v.toggle?At<void*>(v.toggle,0x15C):nullptr;
        auto* button=panel?Control(panel,L"ExBtn",4):nullptr;
        char detail[200];sprintf_s(detail,"EX native entry level=%u available=%u button=%p enabled=%u x=%.0f y=%.0f w=%d h=%d",s.oc_level,e.available,button,
            button?At<unsigned char>(button,0x167):0,button?At<float>(button,0x148):0,button?At<float>(button,0x14C):0,button?At<int>(button,0x154):0,button?At<int>(button,0x158):0);c->Observe(detail);
        if(!capture(L"ex-entry"))return SDGO_GAME_TEST_WAIT;
        c->Assert(s.oc_level==15&&button&&e.available&&At<unsigned char>(button,0x167),"EX-level unit has an enabled EX button");
        if(!button||!e.available||!At<unsigned char>(button,0x167))return SDGO_GAME_TEST_FAIL;
        revision=s.revision;
        int x=At<int>(button,0x154)/2,y=At<int>(button,0x158)/2;
        for(auto* p=button;p;p=At<void*>(p,0x15C)) {x+=static_cast<int>(At<float>(p,0x148));y+=static_cast<int>(At<float>(p,0x14C));}
        sprintf_s(detail,"EX pointer destination %d,%d",x,y);c->Observe(detail);
        for(auto name:{L"ExIcon",L"CustomIcon",L"BackIcon1"})if(auto* icon=Control(panel,name,1)){
            sprintf_s(detail,"EX icon %ls x=%.0f y=%.0f w=%d h=%d visible=%u enabled=%u",name,At<float>(icon,0x148),At<float>(icon,0x14C),At<int>(icon,0x154),At<int>(icon,0x158),At<unsigned char>(icon,0x162),At<unsigned char>(icon,0x167));c->Observe(detail);}
        c->Assert(pointer.Install(api_,mod_)&&pointer.MoveToClientPoint(x,y),"virtual pointer enters native page hit testing at the EX button");
        return next(10);
    }
    if(phase==10){if(!capture(L"ex-hover"))return SDGO_GAME_TEST_WAIT;pointer.SetLeftButton(true);return next(11);}
    if(phase==11){pointer.SetLeftButton(false);return next(1);}
    if(phase==1){
        char detail[160];sprintf_s(detail,"EX native click visible=%u known=%u choices=%u status=%d",e.visible,e.known,e.enabled_choices,e.last_ret_val);c->Observe(detail);
        c->Assert(e.visible&&e.known,"native mouse click opens the server-backed EX selection window");
        if(!e.visible||!e.known){pointer.Clear();return SDGO_GAME_TEST_FAIL;}
        ability=e.ability;balance=e.remaining;
        if(!capture(L"ex-choices"))return SDGO_GAME_TEST_WAIT;
        requests=s.request_count;
        c->Assert(e.remaining>=e.cost,"existing test inventory permits previewing an EX choice");
        if(e.remaining<e.cost){pointer.Clear();return SDGO_GAME_TEST_FAIL;}
        pointer.MoveToClientPoint(645,e.ability==1?254:202);return next(12);
    }
    if(phase==12){pointer.SetLeftButton(true);return next(13);}
    if(phase==13){pointer.SetLeftButton(false);return next(14);}
    if(phase==14){
        c->Assert((s.windows&SDGO_OC_WINDOW_CONFIRM)&&s.request_count==requests,"native choice click opens confirmation without sending a skill change");
        if(!(s.windows&SDGO_OC_WINDOW_CONFIRM)){pointer.Clear();return SDGO_GAME_TEST_FAIL;}
        if(!capture(L"ex-confirm"))return SDGO_GAME_TEST_WAIT;
        pointer.Clear();click("confirm",L"CancelBtn");return next(15);
    }
    if(phase==15){
        c->Assert(e.visible&&s.request_count==requests&&e.remaining==balance&&e.ability==ability,"cancelling EX selection keeps the original skill and inventory");
        pointer.MoveToClientPoint(982,146);return next(16);
    }
    if(phase==16){pointer.SetLeftButton(true);return next(17);}
    if(phase==17){pointer.SetLeftButton(false);return next(2);}
    pointer.Clear();
    c->Assert(!e.visible&&s.revision==revision&&e.ability==ability&&e.remaining==balance,"opening and closing EX changes no skill, revision or inventory");
    return SDGO_GAME_TEST_PASS;
}
