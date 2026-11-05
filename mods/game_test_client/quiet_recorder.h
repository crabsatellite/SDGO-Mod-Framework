#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// Optional review footage for a hidden GameTest client: frames of the genuine
// backbuffer, copied immediately before Present like the single-frame capture.
// Nothing is shown, simulated or composited. Inactive unless the run sets
// SDGO_GAME_TEST_RECORD_DIR; a clip records while <dir>\record.on exists, and
// its first line names the clip; an optional second line limits recording to
// clients whose executable path contains that text. Frames are written by a background thread as
// <clip>_<pid>.raw (top-down BGRA) plus <clip>_<pid>.idx (QPC time per frame), so
// several clients can share one folder; a busy disk
// drops frames instead of stalling the game. tools/game_test_recording.py turns
// the pair into a constant-rate video using the recorded times.
class QuietRecorder {
public:
    QuietRecorder() {
        wchar_t dir[MAX_PATH]{};
        const DWORD n=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_DIR",dir,MAX_PATH);
        if (n && n<MAX_PATH) { dir_=dir; enabled_=true; }
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); frequency_=f.QuadPart;
        InitializeCriticalSection(&lock_);
        InitializeConditionVariable(&wake_);
    }
    ~QuietRecorder() {
        if (writer_) {
            EnterCriticalSection(&lock_); quit_=true; LeaveCriticalSection(&lock_);
            WakeConditionVariable(&wake_);
            WaitForSingleObject(writer_,5000); CloseHandle(writer_);
        }
        if (copy_) copy_->Release();
        DeleteCriticalSection(&lock_);
    }
    QuietRecorder(const QuietRecorder&)=delete;
    QuietRecorder& operator=(const QuietRecorder&)=delete;

    void BeforePresent(IDirect3DDevice9* device) {
        if (!enabled_ || !device) return;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if (now.QuadPart-lastPoll_>=frequency_/4) { lastPoll_=now.QuadPart; PollTrigger(now.QuadPart); }
        if (!recording_ || now.QuadPart<nextDue_) return;
        const LONGLONG period=frequency_/kFramesPerSecond;
        nextDue_=(now.QuadPart-nextDue_>period) ? now.QuadPart+period : nextDue_+period;
        Capture(device,now.QuadPart);
    }

private:
    static constexpr LONGLONG kFramesPerSecond=30;
    static constexpr size_t kQueueLimit=24;
    struct Frame { std::vector<uint8_t> pixels; LONGLONG qpc=0; };
    enum class Command { Frame, Open, Close };
    struct Item { Command command; Frame frame; std::wstring name; UINT width=0,height=0; };

    // The trigger's whole content identifies the request: a changed file ends the current clip
    // even when a busy client (loading, no Presents) never saw the file disappear in between.
    void PollTrigger(LONGLONG now) {
        const std::wstring trigger=dir_+L"\\record.on";
        std::string content;
        if (FILE* f=_wfopen(trigger.c_str(),L"rb")) {
            char text[512]{}; const size_t got=fread(text,1,sizeof(text)-1,f); fclose(f);
            content.assign(text,got);
            if (content.empty()) content="clip";
        }
        if (content==request_) return;
        request_=content;
        if (recording_) {
            recording_=false;
            if (opened_) Push(Item{Command::Close,{},{},0,0});
            opened_=false;
        }
        if (content.empty()) return;
        {
            std::wstring name=L"clip";
            bool mine=true;
            {
                const char* text=content.c_str(); const size_t got=content.size();
                std::wstring parsed; size_t i=0;
                for (;i<got;++i) {
                    const char c=text[i];
                    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-') parsed.push_back(static_cast<wchar_t>(c));
                    else if (!parsed.empty()) break;
                }
                if (!parsed.empty()) name=parsed;
                // Optional second line: only clients whose executable path contains it record this clip.
                while (i<got && text[i]!=10) ++i;
                std::string filter;
                for (++i;i<got && text[i]!=13 && text[i]!=10;++i) filter.push_back(static_cast<char>(tolower(static_cast<unsigned char>(text[i]))));
                if (!filter.empty()) {
                    char image[MAX_PATH]{}; GetModuleFileNameA(nullptr,image,MAX_PATH);
                    std::string path(image); for (auto& c:path) c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    mine=path.find(filter)!=std::string::npos;
                }
            }
            if (!mine) return;
            clipName_=name; recording_=true; opened_=false; nextDue_=now;
        }
    }

    void Capture(IDirect3DDevice9* device,LONGLONG qpc) {
        IDirect3DSurface9* source=nullptr;
        D3DSURFACE_DESC desc{};
        HRESULT status=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source);
        if (SUCCEEDED(status)) status=source->GetDesc(&desc);
        if (SUCCEEDED(status) && (!desc.Width || !desc.Height || desc.Width>4096 || desc.Height>4096 ||
                (desc.Format!=D3DFMT_A8R8G8B8 && desc.Format!=D3DFMT_X8R8G8B8))) status=D3DERR_NOTAVAILABLE;
        if (SUCCEEDED(status) && (!copy_ || copyWidth_!=desc.Width || copyHeight_!=desc.Height || copyFormat_!=desc.Format)) {
            if (copy_) { copy_->Release(); copy_=nullptr; }
            status=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy_,nullptr);
            if (SUCCEEDED(status)) { copyWidth_=desc.Width; copyHeight_=desc.Height; copyFormat_=desc.Format; }
        }
        if (SUCCEEDED(status) && QueueSize()>=kQueueLimit) status=E_PENDING;   // disk behind: drop this frame
        if (SUCCEEDED(status)) status=device->GetRenderTargetData(source,copy_);
        D3DLOCKED_RECT pixels{};
        if (SUCCEEDED(status)) status=copy_->LockRect(&pixels,nullptr,D3DLOCK_READONLY);
        if (SUCCEEDED(status)) {
            const size_t row=static_cast<size_t>(desc.Width)*4;
            if (pixels.pBits && pixels.Pitch>=static_cast<INT>(row)) {
                Frame frame; frame.qpc=qpc; frame.pixels.resize(row*desc.Height);
                for (UINT y=0;y<desc.Height;++y)
                    std::memcpy(frame.pixels.data()+row*y,static_cast<const uint8_t*>(pixels.pBits)+static_cast<size_t>(pixels.Pitch)*y,row);
                copy_->UnlockRect();
                if (!opened_ || openedWidth_!=desc.Width || openedHeight_!=desc.Height) {
                    if (opened_) Push(Item{Command::Close,{},{},0,0});
                    Push(Item{Command::Open,{},clipName_+(segment_++ ? L"_"+std::to_wstring(segment_):L""),desc.Width,desc.Height});
                    opened_=true; openedWidth_=desc.Width; openedHeight_=desc.Height;
                }
                Push(Item{Command::Frame,std::move(frame),{},0,0});
            } else copy_->UnlockRect();
        }
        if (source) source->Release();
    }

    size_t QueueSize() {
        EnterCriticalSection(&lock_); const size_t n=queue_.size(); LeaveCriticalSection(&lock_); return n;
    }
    void Push(Item&& item) {
        EnsureWriter();
        EnterCriticalSection(&lock_); queue_.push_back(std::move(item)); LeaveCriticalSection(&lock_);
        WakeConditionVariable(&wake_);
    }
    void EnsureWriter() {
        if (writer_) return;
        writer_=CreateThread(nullptr,0,[](void* self)->DWORD{static_cast<QuietRecorder*>(self)->WriterLoop();return 0;},this,0,nullptr);
    }
    void WriterLoop() {
        FILE* raw=nullptr; FILE* idx=nullptr; unsigned long long count=0;
        for (;;) {
            EnterCriticalSection(&lock_);
            while (queue_.empty() && !quit_) SleepConditionVariableCS(&wake_,&lock_,INFINITE);
            if (queue_.empty() && quit_) { LeaveCriticalSection(&lock_); break; }
            Item item=std::move(queue_.front()); queue_.pop_front();
            LeaveCriticalSection(&lock_);
            if (item.command==Command::Open) {
                if (raw) fclose(raw); if (idx) fclose(idx);
                const std::wstring base=dir_+L"\\"+item.name+L"_"+std::to_wstring(GetCurrentProcessId());
                raw=_wfopen((base+L".raw").c_str(),L"wb");
                idx=_wfopen((base+L".idx").c_str(),L"w");
                if (idx) fprintf(idx,"sdgo-recording 1 width=%u height=%u format=bgra qpc_frequency=%lld\n",item.width,item.height,frequency_);
                count=0;
            } else if (item.command==Command::Close) {
                if (idx) { fprintf(idx,"end frames=%llu\n",count); fclose(idx); idx=nullptr; }
                if (raw) { fclose(raw); raw=nullptr; }
            } else if (raw && idx) {
                if (fwrite(item.frame.pixels.data(),item.frame.pixels.size(),1,raw)==1) { fprintf(idx,"%llu %lld\n",count,item.frame.qpc); ++count; fflush(idx); }
            }
        }
        if (idx) { fprintf(idx,"end frames=%llu\n",count); fclose(idx); }
        if (raw) fclose(raw);
    }

    bool enabled_=false,recording_=false,opened_=false,quit_=false;
    std::wstring dir_,clipName_;
    std::string request_;
    unsigned segment_=0;
    LONGLONG frequency_=1,lastPoll_=0,nextDue_=0;
    IDirect3DSurface9* copy_=nullptr;
    UINT copyWidth_=0,copyHeight_=0,openedWidth_=0,openedHeight_=0;
    D3DFORMAT copyFormat_=D3DFMT_UNKNOWN;
    HANDLE writer_=nullptr;
    CRITICAL_SECTION lock_{};
    CONDITION_VARIABLE wake_{};
    std::deque<Item> queue_;
};
