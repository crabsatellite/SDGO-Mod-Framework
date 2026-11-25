// Exercises the challenge button's native hover/press handlers without
// releasing a click inside it or submitting a reinforcement request.
int ButtonCaptions(const SdgoGameTestContextV1* c) {
    static unsigned phase=0,revision=0,level=0;
    static ULONGLONG since=GetTickCount64();
    static bool requested=false,checked=false;
    auto dll=GetModuleHandleW(L"oc_custom_client.dll");
    auto observe=reinterpret_cast<SdgoOcCustomClientGetSnapshotV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientGetSnapshotV2"));
    auto getUi=reinterpret_cast<SdgoOcCustomClientUiV1Fn>(GetProcAddress(dll,"SdgoOcCustomClientUiV1"));
    auto click=reinterpret_cast<SdgoOcCustomClientClickV2Fn>(GetProcAddress(dll,"SdgoOcCustomClientClickV2"));
    SdgoOcCustomClientSnapshotV2 s{};s.size=sizeof(s);s.version=2;
    SdgoOcCustomClientUiV1 u{};u.size=sizeof(u);u.version=1;
    if(!observe||!getUi||!click||!observe(&s)||!getUi(&u))return SDGO_GAME_TEST_FAIL;
    if(s.busy||GetTickCount64()-since<1200)return SDGO_GAME_TEST_WAIT;
    const auto next=[&](unsigned n){phase=n;since=GetTickCount64();requested=checked=false;return SDGO_GAME_TEST_WAIT;};
    if(phase==0){
        if(auto* start=Control(u.challenge,L"StartBtn",4)) {
            char geometry[160];sprintf_s(geometry,"loaded challenge button width=%d height=%d",At<int>(start,0x154),At<int>(start,0x158));c->Observe(geometry);
        }
        c->Assert((s.windows&SDGO_OC_WINDOW_ENTRY_LIT)!=0,"existing test unit can open the native challenge without changing its level");
        if(!(s.windows&SDGO_OC_WINDOW_ENTRY_LIT))return SDGO_GAME_TEST_FAIL;
        revision=s.revision;level=s.oc_level;click("entry",L"OverCustomBtn");return next(1);
    }
    if(phase==5) {
        c->Assert(!(s.windows&(SDGO_OC_WINDOW_CHALLENGE|SDGO_OC_WINDOW_CONFIRM)),"caption inspection closes without confirming reinforcement");
        c->Assert(s.revision==revision&&s.oc_level==level&&!s.challenge_selected,"caption mouse states do not modify progression or select consumables");
        return SDGO_GAME_TEST_PASS;
    }
    auto* button=u.challenge?Control(u.challenge,L"StartBtn",4):nullptr;
    if(!button||!(s.windows&SDGO_OC_WINDOW_CHALLENGE))return SDGO_GAME_TEST_FAIL;
    const auto mouse=reinterpret_cast<void(__thiscall*)(void*,unsigned,int,int,unsigned,unsigned)>(At<void**>(button)[19]);
    const auto update=reinterpret_cast<void(__thiscall*)(void*)>(At<void**>(button)[7]);
    if(!checked) {
        if(phase==1||phase==4)mouse(button,WM_MOUSEMOVE,-1,-1,0,0);
        if(phase==2)RadioMouse(button,WM_MOUSEMOVE);
        if(phase==3)RadioMouse(button,WM_LBUTTONDOWN);
        if(phase==4)mouse(button,WM_LBUTTONUP,-1,-1,0,0);
        update(button);
        char geometry[160];sprintf_s(geometry,"button state=%u x=%.0f y=%.0f width=%d height=%d root_x=%.0f root_y=%.0f",phase,
            At<float>(button,0x148),At<float>(button,0x14C),At<int>(button,0x154),At<int>(button,0x158),
            At<float>(u.challenge,0x148),At<float>(u.challenge,0x14C));c->Observe(geometry);
        c->Assert(At<unsigned>(button,0x18C)==(phase==2?1u:phase==3?2u:0u),"native button enters the requested idle/hover/pressed/leave state");
        auto** first=At<unsigned char**>(button,0x190);auto** end=At<unsigned char**>(button,0x194);
        const auto count=first&&end>=first?end-first:0;
        c->Assert(count>=2&&count<=64,"the original button supplies multiple native caption states");
        if(count<2||count>64)return SDGO_GAME_TEST_FAIL;
        const auto read=[](const unsigned char* item){
            const auto str=At<String>(item);
            return std::wstring(str.capacity<8?str.data.inline_data:str.data.pointer,str.length);
        };
        const auto caption=read(first[0]);
        c->Assert(!caption.empty()&&caption.find(L"Text")==std::wstring::npos&&caption.find(L"test")==std::wstring::npos,"challenge uses its localized caption instead of a resource placeholder");
        for(std::ptrdiff_t i=0;i<count;++i) {
            bool same=read(first[i])==caption;
            for(unsigned offset:{0x48u,0x4Cu,0x50u,0x54u,0x68u,0x6Cu})same=same&&At<unsigned>(first[i],offset)==At<unsigned>(first[0],offset);
            c->Assert(same,"every native caption state has identical wording, position, size, alignment and font");
        }
        checked=true;
    }
    if(!requested){requested=visualFrame.Request((std::wstring(c->directory)+L"\\oc-button-state-"+std::to_wstring(phase)+L".bmp").c_str());return SDGO_GAME_TEST_WAIT;}
    HRESULT hr=E_PENDING;const auto state=visualFrame.State(&hr);if(state!=3&&state!=4)return SDGO_GAME_TEST_WAIT;
    c->Assert(state==3&&SUCCEEDED(hr),"captured the actual native challenge button state");
    if(phase==4){click("challenge",L"CloseBtn");return next(5);}
    return next(phase+1);
}
