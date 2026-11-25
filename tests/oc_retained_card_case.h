// Real retained 100%-chip inventory and UI selection. Never confirms a charge.
int RetainedCard(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,revision=0,level=0;
    static ULONGLONG since=GetTickCount64();
    static bool requested=false;
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!items||!click||!observe(&s)||!items(&v))return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-since<1200)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const wchar_t* name){
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured the real retained-card UI before Present");return true;
    };
    if(phase==0){revision=s.revision;level=s.oc_level;click("over",L"OcItemListBtn");return next(1);}
    if(!v.ready)return SDGO_GAME_TEST_WAIT;
    unsigned slot=10;for(unsigned i=0;i<8;++i)if(v.owned[i]==3240004)slot=i;
    c->Assert(slot<8,"catalogue 2220013 is listed under its real inventory ID 3240004");
    if(slot>=8)return SDGO_GAME_TEST_FAIL;
    c->Assert(v.counts[slot]==7,"retained success chip keeps the seven-item test inventory");
    const auto cell=L"Item3DRender"+std::to_wstring(slot);
    if(phase==1){
        // CStatic::SetText uses its own string; button-state text slots are not
        // a valid caption observer for these controls. Inspect the real capture.
        for(const auto* prefix:{L"ItemStatic",L"NumberStatic"}){
            auto* label=Control(v.panel,std::wstring(prefix)+std::to_wstring(slot),3);
            c->Assert(label&&At<unsigned char>(label,0x162),"native item effect and quantity statics are visible");
        }
        c->Assert(At<unsigned char>(v.rows[slot],0x167)!=0,"server permits selecting the chip at this OC level");
        if(!capture(L"oc-card-inventory"))return SDGO_GAME_TEST_WAIT;
        if(!InventoryDragInput(c,true))return SDGO_GAME_TEST_FAIL;
        click("items",cell.c_str());return next(2);
    }
    if(phase==2||phase==4){
        c->Assert(s.challenge_selected==1&&v.placed[0]==3240004&&s.item_models_shown==1,"selecting the retained chip fills its native model slot");
        if(phase==2){if(!capture(L"oc-card-selected"))return SDGO_GAME_TEST_WAIT;click("items",cell.c_str());return next(3);}
        click("items",L"OcItemsRadarBtn");return next(5);
    }
    if(phase==3){c->Assert(!s.challenge_selected&&!s.item_models_shown,"clicking the selected chip deselects it");click("items",cell.c_str());return next(4);}
    if(phase==5){
        c->Assert(s.windows&SDGO_OC_WINDOW_ENTRY_LIT,"existing unit can preview an OC challenge");
        click("entry",L"OverCustomBtn");return next(6);
    }
    if(phase==6){
        c->Assert((s.windows&SDGO_OC_WINDOW_CHALLENGE)&&s.challenge_selected==1&&v.placed[0]==3240004,"challenge refresh preserves the selected retained chip");
        if(!capture(L"oc-card-challenge"))return SDGO_GAME_TEST_WAIT;
        click("challenge",L"StartBtn");return next(7);
    }
    if(phase==7){
        c->Assert(s.windows&SDGO_OC_WINDOW_CONFIRM,"retained card still requires final confirmation");
        if(!capture(L"oc-card-confirm"))return SDGO_GAME_TEST_WAIT;
        click("confirm",L"CancelBtn");return next(8);
    }
    if(phase==8){click("challenge",L"CloseBtn");return next(9);}
    c->Assert(s.revision==revision&&s.oc_level==level,"cancelled card preview leaves progression unchanged");
    c->Assert(!(s.windows&(SDGO_OC_WINDOW_CHALLENGE|SDGO_OC_WINDOW_CONFIRM)),"all preview dialogs close without enhancement");
    return SDGO_GAME_TEST_PASS;
}
