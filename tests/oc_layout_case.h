// Layout-only inspection: works with an existing EX unit and does not open
// a challenge, place items or send allocation requests.
int LayoutOnly(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,revision=0,level=0;
    static float radarY=0;
    static ULONGLONG since=GetTickCount64();
    static bool requested=false;
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!items||!click||!observe(&s)||!items(&v)||!v.toggle||!v.remove[0])return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-since<1500)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const wchar_t* name){
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);
        if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured native OC layout before Present");return true;
    };
    auto* radar=At<void*>(v.toggle,0x15C);
    auto* info=At<void*>(v.remove[0],0x15C);
    if(!radar||!info)return SDGO_GAME_TEST_FAIL;
    if(phase==0) {
        if(!requested) {
            revision=s.revision;level=s.oc_level;radarY=At<float>(radar,0x14C);
            api_->Log(mod_,SDGO_LOG_INFO,"layout radar=(%.1f,%.1f,%d,%d) info=(%.1f,%.1f,%d,%d)",
                At<float>(radar,0x148),radarY,At<int>(radar,0x154),At<int>(radar,0x158),
                At<float>(info,0x148),At<float>(info,0x14C),At<int>(info,0x154),At<int>(info,0x158));
            c->Assert(!v.list_visible&&s.radar_frames>0,"OC page opens with the native radar rendering");
            c->Assert(radarY>=100&&radarY<=110,"radar panel starts level with the factory side panels");
            c->Assert(radarY+At<int>(radar,0x158)<680,"radar panel remains above the factory tabs");
        }
        if(!capture(L"oc-layout-radar"))return SDGO_GAME_TEST_WAIT;
        click("over",L"OcItemListBtn");return next(1);
    }
    if(phase==1) {
        c->Assert(v.list_visible,"moved radar switch still opens the native item list");
        if(!capture(L"oc-layout-items"))return SDGO_GAME_TEST_WAIT;
        click("items",L"OcItemsRadarBtn");return next(2);
    }
    if(phase==2) {
        c->Assert(!v.list_visible&&At<float>(radar,0x14C)==radarY,"returning from items preserves radar alignment");
        click("tab",L"UnitUpRadio");return next(3);
    }
    if(phase==3) {
        click("tab",L"OverCustomRadio");return next(4);
    }
    c->Assert((s.windows&SDGO_OC_WINDOW_TAB_ACTIVE)&&!v.list_visible&&At<float>(radar,0x14C)==radarY,"reopening OC does not accumulate the layout offset");
    c->Assert(s.revision==revision&&s.oc_level==level&&!s.challenge_selected,"layout inspection preserves enhancement and item selection");
    return SDGO_GAME_TEST_PASS;
}
