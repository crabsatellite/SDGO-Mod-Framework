// Read-only native inventory and transition regression. Uses the active test
// unit and server inventory, without allocation, purchase or challenge requests.
std::vector<void*> NativeInventories() {
    std::vector<void*> result;
    auto** manager=static_cast<void**>(api_->Symbol(mod_,"flow.service_managed_pointer"));
    auto* expected=api_->Symbol(mod_,"oc_custom.client_inventory_vtable");
    if(!manager||!*manager||!expected)return result;
    auto* head=At<void*>(*manager,0x44);if(!head)return result;
    unsigned guard=0;
    for(auto* node=At<void*>(head);node&&node!=head&&guard++<512;node=At<void*>(node)) {
        auto* service=At<void*>(node,8);
        if(service&&At<void*>(service)==expected)result.push_back(service);
    }
    return result;
}
bool InventoryDragInput(const SdgoGameTestContextV1* c,bool oc) {
    auto drag=reinterpret_cast<void(__thiscall*)(void*,void*)>(api_->Symbol(mod_,"oc_custom.client_inventory_drag_start"));
    bool exercised=false,clear=true,received=false;
    for(auto* service:NativeInventories()) {
        char state[160];sprintf_s(state,"inventory active=%u type=%d first_id=%u",At<unsigned char>(service,0xE4),At<int>(service,0x178),At<unsigned>(service,0x1B0));c->Observe(state);
        if(!At<unsigned char>(service,0xE4)||At<int>(service,0x178)==1||At<int>(service,0x178)==-1)continue;
        auto* root=At<void*>(service,0x108);
        if(!root)continue;
        char detail[192];sprintf_s(detail,"native drag inventory type=%d first_id=%u visible=%u oc=%u",At<int>(service,0x178),At<unsigned>(service,0x1B0),At<unsigned char>(root,0x162),oc?1:0);c->Observe(detail);
        for(unsigned cell=0;cell<8;++cell) {
            auto* row=At<void*>(service,0x14C+cell*4);
            if(!row)continue;
            std::array<unsigned char,0x38> mouse{};
            UiPut<unsigned short>(mouse.data(),4,0x2353);
            UiPut<int>(mouse.data(),0x30,static_cast<int>(At<float>(root,0x148)+At<float>(row,0x148))+At<int>(row,0x154)/2);
            UiPut<int>(mouse.data(),0x34,static_cast<int>(At<float>(root,0x14C)+At<float>(row,0x14C))+At<int>(row,0x158)/2);
            const auto beforeX=At<int>(service,0x190),beforeY=At<int>(service,0x194);
            drag(service,mouse.data());
            received=received||At<int>(service,0x190)!=beforeX||At<int>(service,0x194)!=beforeY;
            clear=clear&&!At<unsigned char>(service,0x18C);
            exercised=true;
            if(!clear)break;
        }
    }
    c->Assert(exercised,"drag regression reaches the real active native inventory");
    // Even an empty native cell records the drag's local coordinates before
    // looking up its item. Observe those writes too: an empty test inventory
    // must not disguise the same input leak that drags a populated player item.
    c->Assert(oc?clear&&!received:received,oc?"all eight OC cells reject underlying native drag input and capture":"ordinary native inventory receives drag coordinates again");
    return exercised&&(oc?clear&&!received:received);
}
int ItemsTransition(const SdgoGameTestContextV1* c,bool transitions=true) {
    static unsigned phase=0,round=0,revision=0,level=0,moving=0;
    static float left=0,right=0,previousLeft=0,previousRight=0;
    static ULONGLONG since=GetTickCount64();
    static bool requested=false;
    static bool directionOk=true,monotonic=true,captureOk=true;
    static unsigned captures=0;
    static std::array<int16_t,10> counts{};
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto items=reinterpret_cast<SdgoOcCustomClientItemsV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientItemsV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientItemsV1 v{};v.size=sizeof(v);v.version=1;
    if(!observe||!items||!click||!observe(&s)||!items(&v)||!v.toggle||!v.remove[0])return SDGO_GAME_TEST_FAIL;
    auto* radar=At<void*>(v.toggle,0x15C);
    auto* info=At<void*>(v.remove[0],0x15C);
    if(!radar||!info)return SDGO_GAME_TEST_FAIL;
    const auto visible=[](void* p){return p&&At<unsigned char>(p,0x162)!=0;};
    const auto recording=[&](bool on) {
        wchar_t folder[MAX_PATH]{};
        if(!GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_DIR",folder,MAX_PATH))return;
        const auto trigger=std::wstring(folder)+L"\\record.on";
        if(!on){DeleteFileW(trigger.c_str());return;}
        if(FILE* f=_wfopen(trigger.c_str(),L"wb")) {
            fprintf(f,"oc-entry-%u\n",round);fclose(f);
        }
    };
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=false;return SDGO_GAME_TEST_WAIT;};
    const auto capture=[&](const std::wstring& name){
        if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\"+name+L".bmp").c_str());return false;}
        HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);
        if(state!=3&&state!=4)return false;
        c->Assert(state==3&&SUCCEEDED(hr),"captured the actual native OC frame");return true;
    };
    if(phase==3) {
        if(GetTickCount64()-since>5000) {
            c->Assert(0,"OC transition must settle within five seconds");
            return SDGO_GAME_TEST_FAIL;
        }
        const auto x=At<float>(info,0x148),y=At<float>(radar,0x148);
        if(s.windows&SDGO_OC_WINDOW_TAB_ACTIVE) {
            if(x!=left||y!=right) {
                // Do not flush receipts during animation: repeated synchronous
                // logging can starve Present while native updates catch up.
                directionOk=directionOk&&x<=left&&y>=right;
                if(moving)monotonic=monotonic&&x>=previousLeft&&y<=previousRight;
                previousLeft=x;previousRight=y;++moving;
                if(!requested)requested=visualFrame.Request((std::wstring(c->directory)+L"\\oc-transition-"+
                    std::to_wstring(round)+L"-"+std::to_wstring(moving)+L".bmp").c_str());
                else {
                    HRESULT hr=E_PENDING;const auto status=visualFrame.State(&hr);
                    if(status==3||status==4){captureOk=captureOk&&status==3&&SUCCEEDED(hr);++captures;requested=false;}
                }
                return SDGO_GAME_TEST_WAIT;
            }
            if(GetTickCount64()-since<1000)return SDGO_GAME_TEST_WAIT;
            c->Assert(moving>0,"reentering OC produces observable intermediate animation frames");
            c->Assert(directionOk&&monotonic,"OC panels move from opposite entry edges without jumping backward");
            c->Assert(captureOk&&captures>=3,"captured multiple real intermediate frames without blocking animation on receipt logging");
            c->Assert(x==left&&y==right,"OC transition settles without accumulating an offset");
            char message[160];sprintf_s(message,"OC transition round=%u updates=%u intermediate_captures=%u",round,moving,captures);c->Observe(message);
            return next(4);
        }
        return SDGO_GAME_TEST_WAIT;
    }
    if(s.busy||GetTickCount64()-since<1200)return SDGO_GAME_TEST_WAIT;
    if(phase==0) {
        revision=s.revision;level=s.oc_level;
        left=At<float>(info,0x148);right=At<float>(radar,0x148);
        c->Assert(!v.list_visible&&s.radar_frames>0,"initial OC page displays its native radar");
        if(!capture(L"oc-inventory-radar"))return SDGO_GAME_TEST_WAIT;
        click("over",L"OcItemListBtn");return next(1);
    }
    if(phase==1||phase==5) {
        if(!v.ready)return SDGO_GAME_TEST_WAIT;
        c->Assert(v.list_visible,"native item-list switch opens the inventory page");
        for(unsigned i=0;i<8;++i) {
            const bool occupied=i<s.owned_items;
            c->Assert(visible(v.rows[i])==occupied,"native inventory model cells match actual held item types");
            c->Assert(visible(Control(v.panel,L"ItemStatic"+std::to_wstring(i),3))==occupied,"held item names are visible and empty names hidden");
            c->Assert(visible(Control(v.panel,L"NumberStatic"+std::to_wstring(i),3))==occupied,"held item quantities are visible and empty quantities hidden");
            if(phase==1)counts[i]=v.counts[i];
            else c->Assert(counts[i]==v.counts[i],"page switching preserves server-reported item counts");
            if(occupied) {
                c->Assert(v.owned[i]>0&&v.counts[i]>0,"held item rows retain real item IDs and positive counts");
                char message[160];sprintf_s(message,"OC inventory row=%u id=%u count=%d",i,v.owned[i],v.counts[i]);c->Observe(message);
            }
        }
        if(!capture(L"oc-inventory-items-"+std::to_wstring(phase)+L"-"+std::to_wstring(round)))return SDGO_GAME_TEST_WAIT;
        if(!InventoryDragInput(c,true))return SDGO_GAME_TEST_FAIL;
        if(!transitions) {
            c->Assert(s.revision==revision&&s.oc_level==level&&!s.challenge_selected,
                      "drag inspection does not enhance or select consumables");
            click("tab",L"UnitUpRadio");return next(6);
        }
        if(phase==5&&++round==2) {
            c->Assert(s.revision==revision&&s.oc_level==level&&!s.challenge_selected,"inventory and animation inspection does not enhance or select consumables");
            click("tab",L"UnitUpRadio");return next(6);
        }
        recording(true);
        click("items",L"OcItemsRadarBtn");
        click("tab",round ? L"StatusRadio" : L"UnitUpRadio");return next(2);
    }
    if(phase==2) {
        c->Assert(!(s.windows&SDGO_OC_WINDOW_TAB_ACTIVE),"native enhancement tab hides the OC page");
        c->Assert(At<float>(info,0x148)<left&&At<float>(radar,0x148)>right,
                  "hidden OC panels are parked at the entry edges before native service activation");
        if(!capture(L"oc-native-upgrade-"+std::to_wstring(round)))return SDGO_GAME_TEST_WAIT;
        // Receipt flushing can stall this update long enough for the native
        // fixed-step loop to catch up an entire transition before Present.
        // Let that work settle before clicking; do not log on the click frame.
        return next(7);
    }
    if(phase==7) {
        moving=captures=0;directionOk=monotonic=captureOk=true;
        click("tab",L"OverCustomRadio");return next(3);
    }
    if(phase==4) {
        recording(false);
        c->Assert(!v.list_visible&&s.radar_frames>0,"reentering OC restores radar view");
        if(!capture(L"oc-settled-"+std::to_wstring(round)))return SDGO_GAME_TEST_WAIT;
        click("over",L"OcItemListBtn");return next(5);
    }
    // Last operation: verify native drag input is restored on the ordinary
    // page, then end this isolated client without dropping/using any item.
    if(phase==6) {
        return InventoryDragInput(c,false)?SDGO_GAME_TEST_PASS:SDGO_GAME_TEST_FAIL;
    }
    return SDGO_GAME_TEST_FAIL;
}
