#pragma once
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
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
// clients whose executable path contains that text. Each Present copies the
// backbuffer into a small ring of render targets and reads back the one filled
// two frames earlier, so the copy never waits for the frame being drawn. Frames
// are written by a background thread as <clip>_<pid>.raw (top-down BGRA) plus
// <clip>_<pid>.idx (QPC time per frame), so several clients can share one
// folder; a busy disk drops frames instead of stalling the game.
// tools/game_test_recording.py turns the pair into a constant-rate video using
// the recorded times. SDGO_GAME_TEST_RECORD_FPS (1..120, default 60) bounds the
// capture rate. When SDGO_GAME_TEST_RECORD_FFMPEG names an ffmpeg executable,
// the writer instead feeds each frame to it once, stamped with the wall clock,
// and the clip lands as <clip>_<pid>.mkv at the game's own variable rate, true
// to real time (re-encode it to a constant rate before editing);
// SDGO_GAME_TEST_RECORD_ENCODER replaces the default libx264 output options.
class QuietRecorder {
public:
    QuietRecorder() {
        wchar_t value[MAX_PATH]{};
        DWORD n=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_DIR",value,MAX_PATH);
        if (n && n<MAX_PATH) { dir_=value; enabled_=true; }
        n=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_FFMPEG",value,MAX_PATH);
        if (n && n<MAX_PATH) ffmpeg_=value;
        wchar_t text[1024]{};
        n=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_ENCODER",text,1024);
        encoder_=(n && n<1024) ? text : L"-vf pad=ceil(iw/2)*2:ceil(ih/2)*2 -c:v libx264 -preset veryfast -crf 16 -pix_fmt yuv420p";
        n=GetEnvironmentVariableW(L"SDGO_GAME_TEST_RECORD_FPS",text,1024);
        if (n && n<1024) { const int fps=_wtoi(text); if (fps>=1 && fps<=120) fps_=fps; }
        LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); frequency_=f.QuadPart;
        InitializeCriticalSection(&lock_);
        InitializeConditionVariable(&wake_);
    }
    ~QuietRecorder() {
        if (writer_) {
            EnterCriticalSection(&lock_); quit_=true; LeaveCriticalSection(&lock_);
            WakeConditionVariable(&wake_);
            WaitForSingleObject(writer_,20000); CloseHandle(writer_);
        }
        ReleaseRing();
        DeleteCriticalSection(&lock_);
    }
    QuietRecorder(const QuietRecorder&)=delete;
    QuietRecorder& operator=(const QuietRecorder&)=delete;

    void BeforePresent(IDirect3DDevice9* device) {
        if (!enabled_ || !device) return;
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if (now.QuadPart-lastPoll_>=frequency_/4) { lastPoll_=now.QuadPart; PollTrigger(now.QuadPart); }
        if (!recording_) { if (ringDevice_) ReleaseRing(); return; }
        if (now.QuadPart<nextDue_) return;
        const LONGLONG period=frequency_/fps_;
        nextDue_=(now.QuadPart-nextDue_>period) ? now.QuadPart+period : nextDue_+period;
        Capture(device,now.QuadPart);
    }

    // The ring lives in the default pool, so it must go before the device resets.
    void BeforeReset() { ReleaseRing(); }

private:
    static constexpr size_t kQueueLimit=24;
    static constexpr unsigned kRing=3;
    struct Frame { std::vector<uint8_t> pixels; LONGLONG qpc=0; };
    enum class Command { Frame, Open, Close };
    struct Item { Command command; Frame frame; std::wstring name; UINT width=0,height=0; };
    struct Slot { IDirect3DSurface9* surface=nullptr; LONGLONG qpc=0; bool pending=false; };

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

    void ReleaseRing() {
        for (auto& slot:ring_) { if (slot.surface) slot.surface->Release(); slot={}; }
        if (copy_) { copy_->Release(); copy_=nullptr; }
        ringDevice_=nullptr; ringWidth_=ringHeight_=0; ringFormat_=D3DFMT_UNKNOWN; head_=0;
    }

    void Capture(IDirect3DDevice9* device,LONGLONG qpc) {
        IDirect3DSurface9* source=nullptr;
        D3DSURFACE_DESC desc{};
        HRESULT status=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&source);
        if (SUCCEEDED(status)) status=source->GetDesc(&desc);
        if (SUCCEEDED(status) && (!desc.Width || !desc.Height || desc.Width>4096 || desc.Height>4096 ||
                (desc.Format!=D3DFMT_A8R8G8B8 && desc.Format!=D3DFMT_X8R8G8B8))) status=D3DERR_NOTAVAILABLE;
        if (SUCCEEDED(status) && (ringDevice_!=device || ringWidth_!=desc.Width || ringHeight_!=desc.Height || ringFormat_!=desc.Format)) {
            ReleaseRing();
            for (auto& slot:ring_) {
                status=device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&slot.surface,nullptr);
                if (FAILED(status)) break;
            }
            if (SUCCEEDED(status)) status=device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&copy_,nullptr);
            if (SUCCEEDED(status)) { ringDevice_=device; ringWidth_=desc.Width; ringHeight_=desc.Height; ringFormat_=desc.Format; head_=0; }
            else ReleaseRing();
        }
        if (SUCCEEDED(status)) {
            Slot& fill=ring_[head_];
            if (SUCCEEDED(device->StretchRect(source,nullptr,fill.surface,nullptr,D3DTEXF_NONE))) { fill.qpc=qpc; fill.pending=true; }
            else fill.pending=false;
            head_=(head_+1)%kRing;
            // Read back the slot filled two frames ago: the next one to be overwritten.
            Slot& ready=ring_[head_];
            if (ready.pending) {
                ready.pending=false;
                if (QueueSize()<kQueueLimit && SUCCEEDED(device->GetRenderTargetData(ready.surface,copy_))) Enqueue(desc,ready.qpc);
            }
        }
        if (source) source->Release();
    }

    void Enqueue(const D3DSURFACE_DESC& desc,LONGLONG qpc) {
        D3DLOCKED_RECT pixels{};
        if (FAILED(copy_->LockRect(&pixels,nullptr,D3DLOCK_READONLY))) return;
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

    // ---- writer thread -------------------------------------------------------------------
    struct Output {
        FILE* raw=nullptr; FILE* idx=nullptr;
        HANDLE process=nullptr; HANDLE pipe=nullptr;
        unsigned long long captured=0; bool broken=false;
    };

    void OpenEncoder(Output& out,const std::wstring& base,UINT width,UINT height) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
        HANDLE readEnd=nullptr,writeEnd=nullptr;
        if (!CreatePipe(&readEnd,&writeEnd,&inherit,1u<<22)) { out.broken=true; return; }
        SetHandleInformation(writeEnd,HANDLE_FLAG_INHERIT,0);
        HANDLE log=CreateFileW((base+L".ffmpeg.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&inherit,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        std::wstring command=L"\""+ffmpeg_+L"\" -hide_banner -loglevel warning -y -use_wallclock_as_timestamps 1 -f rawvideo -pix_fmt bgra -s "+
            std::to_wstring(width)+L"x"+std::to_wstring(height)+L" -framerate "+std::to_wstring(fps_)+L" -i pipe:0 "+
            encoder_+L" -fps_mode passthrough \""+base+L".mkv\"";
        std::vector<wchar_t> buffer(command.begin(),command.end()); buffer.push_back(0);
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=readEnd; startup.hStdOutput=log; startup.hStdError=log;
        PROCESS_INFORMATION process{};
        const BOOL started=CreateProcessW(nullptr,buffer.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
        CloseHandle(readEnd);
        if (log!=INVALID_HANDLE_VALUE) CloseHandle(log);
        if (!started) { CloseHandle(writeEnd); out.broken=true; return; }
        CloseHandle(process.hThread);
        out.process=process.hProcess; out.pipe=writeEnd;
    }

    bool Send(Output& out,const std::vector<uint8_t>& pixels) {
        size_t done=0;
        while (done<pixels.size()) {
            DWORD written=0;
            const DWORD chunk=static_cast<DWORD>((std::min)(pixels.size()-done,static_cast<size_t>(1u<<20)));
            if (!WriteFile(out.pipe,pixels.data()+done,chunk,&written,nullptr) || !written) { out.broken=true; return false; }
            done+=written;
        }
        return true;
    }

    void CloseOutput(Output& out) {
        if (out.idx) { fprintf(out.idx,"end frames=%llu\n",out.captured); fclose(out.idx); out.idx=nullptr; }
        if (out.raw) { fclose(out.raw); out.raw=nullptr; }
        if (out.pipe) { CloseHandle(out.pipe); out.pipe=nullptr; }
        if (out.process) {
            if (WaitForSingleObject(out.process,15000)!=WAIT_OBJECT_0) TerminateProcess(out.process,1);
            CloseHandle(out.process); out.process=nullptr;
        }
        out.captured=0; out.broken=false;
    }

    void WriterLoop() {
        Output out;
        for (;;) {
            EnterCriticalSection(&lock_);
            while (queue_.empty() && !quit_) SleepConditionVariableCS(&wake_,&lock_,INFINITE);
            if (queue_.empty() && quit_) { LeaveCriticalSection(&lock_); break; }
            Item item=std::move(queue_.front()); queue_.pop_front();
            LeaveCriticalSection(&lock_);
            if (item.command==Command::Open) {
                CloseOutput(out);
                const std::wstring base=dir_+L"\\"+item.name+L"_"+std::to_wstring(GetCurrentProcessId());
                if (ffmpeg_.empty()) out.raw=_wfopen((base+L".raw").c_str(),L"wb");
                else OpenEncoder(out,base,item.width,item.height);
                out.idx=_wfopen((base+L".idx").c_str(),L"w");
                if (out.idx) fprintf(out.idx,"sdgo-recording 1 width=%u height=%u format=bgra qpc_frequency=%lld\n",item.width,item.height,frequency_);
            } else if (item.command==Command::Close) {
                CloseOutput(out);
            } else if (out.raw) {
                if (fwrite(item.frame.pixels.data(),item.frame.pixels.size(),1,out.raw)==1) Index(out,item.frame.qpc);
            } else if (out.pipe && !out.broken) {
                // Each frame goes once; ffmpeg stamps it with the wall clock as it arrives.
                if (Send(out,item.frame.pixels)) Index(out,item.frame.qpc);
            }
        }
        CloseOutput(out);
    }

    static void Index(Output& out,LONGLONG qpc) {
        if (out.idx) { fprintf(out.idx,"%llu %lld\n",out.captured,qpc); fflush(out.idx); }
        ++out.captured;
    }

    bool enabled_=false,recording_=false,opened_=false,quit_=false;
    std::wstring dir_,clipName_,ffmpeg_,encoder_;
    std::string request_;
    unsigned segment_=0;
    int fps_=60;
    LONGLONG frequency_=1,lastPoll_=0,nextDue_=0;
    Slot ring_[kRing]{};
    unsigned head_=0;
    IDirect3DDevice9* ringDevice_=nullptr;
    IDirect3DSurface9* copy_=nullptr;
    UINT ringWidth_=0,ringHeight_=0,openedWidth_=0,openedHeight_=0;
    D3DFORMAT ringFormat_=D3DFMT_UNKNOWN;
    HANDLE writer_=nullptr;
    CRITICAL_SECTION lock_{};
    CONDITION_VARIABLE wake_{};
    std::deque<Item> queue_;
};
