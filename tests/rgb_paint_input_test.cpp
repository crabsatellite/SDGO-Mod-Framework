#ifdef NDEBUG
#undef NDEBUG
#endif
#include <windows.h>
#include <commctrl.h>
#include <cassert>
#include <cstdio>
#include <cstdint>
namespace fixture {
unsigned forwarded=0,updates=0,picks=0,events=0;
LRESULT CALLBACK Forward(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){if(msg>=WM_MOUSEFIRST&&msg<=WM_MOUSELAST)++forwarded;return DefSubclassProc(hwnd,msg,wp,lp);}
void __fastcall Update(void*,void*){++updates;}
void __fastcall Pick(void*,void*){++picks;}
int __stdcall Event(void*,unsigned msg,std::uintptr_t id,void* control){assert(msg==0xa02&&id==17&&control);++events;return 37;}
}
#define DefSubclassProc fixture::Forward
#include "../mods/unit_appearance_client/rgb/rgb_paint_client.cpp"
#undef DefSubclassProc
int main(){
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"RgbInputRegression";
    assert(RegisterClassW(&cls));window=CreateWindowW(cls.lpszClassName,L"",WS_POPUP,0,0,1024,768,nullptr,nullptr,cls.hInstance,nullptr);assert(window);
    assert(SetWindowSubclass(window,&WindowProc,kSubclass,0));
    std::array<unsigned char,0x300> pageBytes{},rootBytes{},inventory{};
    page=pageBytes.data();root=rootBytes.data();pageTable=page;inventoryTable=inventory.data();
    Put<void*>(page,0,pageTable);Put<void*>(page,0xd0,root);Put<std::uint8_t>(page,0xac,1);Put<std::uint8_t>(root,0x162,1);Put<void*>(inventory.data(),0,inventoryTable);
    Snapshot snapshot;snapshot.identity={1,42};assert(panel.Open(snapshot));active=lastEditing=authorityReady=true;
    originalInventoryUpdate=reinterpret_cast<Simple>(&fixture::Update);originalInventoryPick=reinterpret_cast<Simple>(&fixture::Pick);originalInventoryEvent=&fixture::Event;
    auto native=[&](bool blocked){const auto u=fixture::updates,p=fixture::picks,e=fixture::events;
        InventoryUpdateHook(inventory.data(),nullptr);InventoryPickHook(inventory.data(),nullptr);
        assert(InventoryEventHook(inventory.data(),0xa02,17,inventory.data())==(blocked?0:37));
        assert(fixture::updates==u+!blocked&&fixture::picks==p+!blocked&&fixture::events==e+!blocked);
    };
    auto send=[&](UINT msg,int x,int y,WPARAM wp=0){return SendMessageW(window,msg,wp,MAKELPARAM(x,y));};
    native(true); // Hidden inventory polling, direct picking and item drag events.
    send(WM_LBUTTONDOWN,970,410);assert(pointerButtons==1&&panel.focus==0&&!panel.Dragging());
    auto before=fixture::forwarded;send(WM_MOUSEMOVE,500,700);send(WM_LBUTTONUP,500,700);
    assert(fixture::forwarded==before&&pointerButtons==0&&panel.focus==0); // Numeric field keeps keyboard focus after release.
    send(WM_CHAR,L'2',0,L'2');assert(panel.buffer==L"2");
    send(WM_LBUTTONDBLCLK,850,200);assert(panel.Dragging());send(WM_MOUSEMOVE,-100,-100);send(WM_LBUTTONUP,-100,-100);assert(!panel.Dragging()&&!pointerButtons);
    send(WM_RBUTTONDOWN,850,200);send(WM_MBUTTONDOWN,500,700);assert(pointerButtons==6);
    send(WM_RBUTTONUP,500,700);assert(pointerButtons==4);send(WM_MBUTTONUP,500,700);assert(!pointerButtons);
    POINT point{850,200};ClientToScreen(window,&point);before=fixture::forwarded;send(WM_MOUSEWHEEL,point.x,point.y,MAKEWPARAM(0,WHEEL_DELTA));assert(fixture::forwarded==before);
    send(WM_LBUTTONDOWN,940,640);assert(!panel.editing&&pointerButtons);native(true);
    send(WM_LBUTTONUP,940,640);native(false); // Do not expose the held mode-button click to native inventory.
    before=fixture::forwarded;send(WM_LBUTTONDOWN,850,200);send(WM_LBUTTONUP,850,200);assert(fixture::forwarded==before+2);
    panel.editing=lastEditing=true;
    before=fixture::forwarded;send(WM_RBUTTONDOWN,400,300);send(WM_MOUSEMOVE,420,300);send(WM_RBUTTONUP,420,300);assert(fixture::forwarded==before+3); // Preview rotation remains native.
    for(UINT cancel:{WM_CANCELMODE,WM_KILLFOCUS,WM_CAPTURECHANGED}){
        send(WM_LBUTTONDOWN,850,200);assert(pointerButtons);SendMessageW(window,cancel,0,0);assert(!pointerButtons&&!panel.Dragging());
    }
    authorityReady=false;send(WM_LBUTTONDOWN,850,200);assert(pointerButtons&&!panel.Dragging());send(WM_LBUTTONUP,850,200);
    active=false;native(false);active=true;Put<std::uint8_t>(root,0x162,0);native(false);Put<std::uint8_t>(root,0x162,1);
    send(WM_LBUTTONDOWN,850,200);Detach(false);assert(!pointerButtons&&!active);native(false);
    DestroyWindow(window);std::puts("RGB input: native inventory isolation, complete gestures, mode switch and cleanup passed");
}
