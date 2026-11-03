#include <windows.h>
#include <cstdio>
#include "../mods/announcements/embedded_browser.h"
namespace {
int iconQueries=0,gameMessages=0;
LRESULT CALLBACK NativeWindow(HWND window,UINT message,WPARAM first,LPARAM second){
    if(message==WM_GETICON){++iconQueries;return 0;}
    if(message==WM_APP+12){++gameMessages;return 42;}
    return DefWindowProcW(window,message,first,second);
}
}
int main(){
    WNDCLASSW type{};type.lpfnWndProc=NativeWindow;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"SDGO.Announcement.ParentMessageTest";
    if(!RegisterClassW(&type))return 1;
    HWND window=CreateWindowExW(0,type.lpszClassName,L"",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,type.hInstance,nullptr);
    if(!window)return 2;
    if(!announcement_browser::ProtectParent(window))return 3;
    iconQueries=0;gameMessages=0;
    HICON icon=LoadIconW(nullptr,MAKEINTRESOURCEW(32515));SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon));
    const auto observed=reinterpret_cast<HICON>(SendMessageW(window,WM_GETICON,ICON_BIG,0));
    const auto game=SendMessageW(window,WM_APP+12,0,0);
    const bool passed=observed==icon && iconQueries==0 && game==42 && gameMessages==1;
    DestroyWindow(window);UnregisterClassW(type.lpszClassName,type.hInstance);
    std::puts(passed?"announcement parent messages: passed":"announcement parent messages: failed");return passed?0:4;
}
