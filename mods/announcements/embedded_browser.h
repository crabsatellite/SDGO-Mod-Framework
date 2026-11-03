#pragma once
#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <commctrl.h>
#include <memory>

namespace announcement_browser {
inline LRESULT CALLBACK ParentMessages(HWND parent,UINT message,WPARAM first,LPARAM second,UINT_PTR identity,DWORD_PTR) {
    // Embedded window hosts ask the parent for the ordinary window icon. The native
    // game forwards that non-game message into a scene object that can be null
    // between a successful room reply and construction of the next page.
    if(message==WM_GETICON)return DefWindowProcW(parent,message,first,second);
    if(message==WM_NCDESTROY)RemoveWindowSubclass(parent,ParentMessages,identity);
    return DefSubclassProc(parent,message,first,second);
}
inline bool ProtectParent(HWND parent) {
    return SetWindowSubclass(parent,ParentMessages,reinterpret_cast<UINT_PTR>(&ParentMessages),0)!=FALSE;
}
struct Job {
    HWND parent=nullptr,window=nullptr;
    HDESK desktop=nullptr;
    int x=0,y=0,width=0,height=0;
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    IStream* volatile stream=nullptr;
    HRESULT result=E_PENDING;
    volatile LONG abandoned=0;
    ~Job(){if(ready)CloseHandle(ready);}
};
inline HWND window=nullptr;
inline void* wrapper=nullptr;

inline DWORD WINAPI Thread(void* argument) {
    std::unique_ptr<std::shared_ptr<Job>> incoming(static_cast<std::shared_ptr<Job>*>(argument));
    auto job=*incoming;incoming.reset();
    if(!SetThreadDesktop(job->desktop)){job->result=HRESULT_FROM_WIN32(GetLastError());SetEvent(job->ready);return 0;}
    const HRESULT apartment=OleInitialize(nullptr);
    if(FAILED(apartment)){job->result=apartment;SetEvent(job->ready);return 0;}
    HMODULE atl=LoadLibraryExW(L"atl.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Initialize=BOOL(WINAPI*)();
    using Control=HRESULT(WINAPI*)(HWND,IUnknown**);
    const auto initialize=atl?reinterpret_cast<Initialize>(GetProcAddress(atl,"AtlAxWinInit")):nullptr;
    const auto control=atl?reinterpret_cast<Control>(GetProcAddress(atl,"AtlAxGetControl")):nullptr;
    if(!initialize || !control || !initialize())job->result=E_NOINTERFACE;
    else {
        // A child of the actual native game window on the same desktop. No
        // standalone browser, external URL, executable or additional UI process.
        job->window=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_NOPARENTNOTIFY,L"AtlAxWin",L"Shell.Explorer.2",
            WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,job->x,job->y,job->width,job->height,
            job->parent,nullptr,atl,nullptr);
        if(!job->window)job->result=HRESULT_FROM_WIN32(GetLastError());
        else {
            IUnknown* unknown=nullptr;IWebBrowser2* browser=nullptr;
            job->result=control(job->window,&unknown);
            if(SUCCEEDED(job->result) && unknown)
                job->result=unknown->QueryInterface(__uuidof(IWebBrowser2),reinterpret_cast<void**>(&browser));
            if(SUCCEEDED(job->result) && browser){
                IStream* marshaled=nullptr;
                job->result=CoMarshalInterThreadInterfaceInStream(__uuidof(IWebBrowser2),browser,&marshaled);
                InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&job->stream),marshaled);
            }
            if(browser)browser->Release();if(unknown)unknown->Release();
        }
    }
    SetEvent(job->ready);
    if(SUCCEEDED(job->result) && !InterlockedCompareExchange(&job->abandoned,0,0)) {
        MSG message{};
        while(IsWindow(job->window) && GetMessageW(&message,nullptr,0,0)>0){
            TranslateMessage(&message);DispatchMessageW(&message);
            if(InterlockedCompareExchange(&job->abandoned,0,0))break;
        }
    }
    if(IsWindow(job->window))DestroyWindow(job->window);
    auto* stream=static_cast<IStream*>(InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&job->stream),nullptr));
    if(stream){CoReleaseMarshalData(stream);stream->Release();}
    // ATL registers a process-wide window class whose procedure remains in this
    // system DLL. Retain the loaded module until the game process terminates.
    OleUninitialize();return 0;
}

inline HRESULT Create(void* native) {
    auto* words=static_cast<uint32_t*>(native);
    DWORD process=0;GetWindowThreadProcessId(reinterpret_cast<HWND>(words[0]),&process);
    if(process!=GetCurrentProcessId())return E_INVALIDARG;
    if(!ProtectParent(reinterpret_cast<HWND>(words[0])))return E_FAIL;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized) && initialized!=RPC_E_CHANGED_MODE)return initialized;
    auto job=std::make_shared<Job>();if(!job->ready)return E_OUTOFMEMORY;
    job->parent=reinterpret_cast<HWND>(words[0]);job->desktop=GetThreadDesktop(GetCurrentThreadId());
    job->x=static_cast<int>(*reinterpret_cast<float*>(words+6));
    job->y=static_cast<int>(*reinterpret_cast<float*>(words+7));
    job->width=static_cast<int>(words[5]);job->height=static_cast<int>(words[4]);
    if(job->width<=0 || job->height<=0){
        RECT bounds{};GetClientRect(job->parent,&bounds);
        job->x=5;job->y=203;job->width=340;job->height=390;
        if(job->width>bounds.right-10)job->width=bounds.right-10;
        if(job->height>bounds.bottom-job->y-10)job->height=bounds.bottom-job->y-10;
        if(job->width<100 || job->height<80)return E_INVALIDARG;
        words[4]=static_cast<uint32_t>(job->height);words[5]=static_cast<uint32_t>(job->width);
        *reinterpret_cast<float*>(words+6)=static_cast<float>(job->x);
        *reinterpret_cast<float*>(words+7)=static_cast<float>(job->y);
    }
    auto* argument=new std::shared_ptr<Job>(job);
    HANDLE thread=CreateThread(nullptr,0,Thread,argument,0,nullptr);
    if(!thread){delete argument;return HRESULT_FROM_WIN32(GetLastError());}
    CloseHandle(thread);
    // A child ActiveX site can synchronously query its native parent. Pump the
    // owning window's messages during finite initialization, as OLE requires.
    const uint64_t deadline=GetTickCount64()+30000;
    DWORD waited=WAIT_TIMEOUT;
    while(GetTickCount64()<deadline){
        waited=MsgWaitForMultipleObjects(1,&job->ready,FALSE,100,QS_SENDMESSAGE);
        if(waited==WAIT_OBJECT_0)break;
        MSG message{};PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
    }
    if(waited!=WAIT_OBJECT_0){InterlockedExchange(&job->abandoned,1);return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
    if(FAILED(job->result))return job->result;
    auto* stream=static_cast<IStream*>(InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&job->stream),nullptr));
    IWebBrowser2* browser=nullptr;
    const HRESULT result=stream?CoGetInterfaceAndReleaseStream(stream,__uuidof(IWebBrowser2),reinterpret_cast<void**>(&browser)):E_NOINTERFACE;
    if(FAILED(result)){InterlockedExchange(&job->abandoned,1);PostMessageW(job->window,WM_CLOSE,0,0);return result;}
    words[1]=reinterpret_cast<uint32_t>(job->window);words[2]=reinterpret_cast<uint32_t>(browser);
    window=job->window;wrapper=native;return S_OK;
}
inline void Visible(void* native,bool shown) {
    if(native==wrapper && IsWindow(window))ShowWindow(window,shown?SW_SHOWNOACTIVATE:SW_HIDE);
}
inline void Close() {
    HWND owned=window;window=nullptr;wrapper=nullptr;
    if(IsWindow(owned)){ShowWindow(owned,SW_HIDE);PostMessageW(owned,WM_CLOSE,0,0);}
}
}
