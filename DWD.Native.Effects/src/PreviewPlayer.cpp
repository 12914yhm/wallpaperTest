#include "PreviewPlayer.h"
#include "RuntimeDiagnostics.h"
#include <mfplay.h>
#include <mfapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>

using Microsoft::WRL::ComPtr;

struct PreviewPlayer::State : std::enable_shared_from_this<State> {
    enum class Kind { Open, Cancel, Play, Pause, Restart, Update, Stop, Quit, Event, Seek, Position };
    struct Job {
        Kind kind{};
        uint64_t generation{};
        uint64_t epoch{};
        std::wstring path;
        std::function<std::wstring()> prepare;
        HWND window{};
        int positionMs{};
        MFP_EVENT_TYPE event{};
        HRESULT result{};
        ComPtr<IMFPMediaItem> item;
    };
    class Callback final : public IMFPMediaPlayerCallback {
    public:
        Callback(std::weak_ptr<State> state,uint64_t epoch):state_(std::move(state)),epoch_(epoch){}
        STDMETHODIMP QueryInterface(REFIID iid, void** object) override {
            if(!object) return E_POINTER;
            if(iid==IID_IUnknown || iid==IID_IMFPMediaPlayerCallback) {
                *object=static_cast<IMFPMediaPlayerCallback*>(this);AddRef();return S_OK;
            }
            *object=nullptr;return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override {return ++references_;}
        STDMETHODIMP_(ULONG) Release() override {
            const ULONG value=--references_;if(!value) delete this;return value;
        }
        void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
            auto state=state_.lock();
            if(!state || state->closing || !event) return;
            Job job;job.kind=Kind::Event;job.event=event->eEventType;job.result=event->hrEvent;
            job.epoch=epoch_;
            switch(event->eEventType) {
            case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
                const auto* data=reinterpret_cast<MFP_MEDIAITEM_CREATED_EVENT*>(event);
                job.generation=data->dwUserData;job.item=data->pMediaItem;break;
            }
            case MFP_EVENT_TYPE_MEDIAITEM_SET:
                job.item=reinterpret_cast<MFP_MEDIAITEM_SET_EVENT*>(event)->pMediaItem;break;
            case MFP_EVENT_TYPE_PLAY:
                job.item=reinterpret_cast<MFP_PLAY_EVENT*>(event)->pMediaItem;break;
            case MFP_EVENT_TYPE_PLAYBACK_ENDED:
                job.item=reinterpret_cast<MFP_PLAYBACK_ENDED_EVENT*>(event)->pMediaItem;break;
            default:break;
            }
            // 回调不访问播放器状态、不在锁内释放媒体源，也不从回调调用 Play/SetMediaItem。
            state->Push(std::move(job));
        }
    private:
        std::weak_ptr<State> state_;
        uint64_t epoch_{};
        std::atomic<ULONG> references_{1};
    };

    std::atomic<HWND> notify{};
    UINT readyMessage{},endedMessage{},errorMessage{};
    std::atomic<uint64_t> latest{};
    std::atomic<bool> closing{};
    std::atomic<LONG> width{},height{};
    std::atomic<int> positionMs{},durationMs{};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    // 以下字段仅由媒体线程读写。
    ComPtr<IMFPMediaPlayer> player;
    ComPtr<Callback> callback;
    ComPtr<IMFPMediaItem> pending,active;
    uint64_t pendingGeneration{},activeGeneration{};
    uint64_t playerEpoch{};
    HWND videoWindow{};

    void Push(Job job) {
        {
            std::lock_guard lock(mutex);
            if(closing && job.kind!=Kind::Quit) return;
            if(job.kind==Kind::Quit) jobs.clear();
            if(job.kind==Kind::Open || job.kind==Kind::Cancel || job.kind==Kind::Stop) {
                std::erase_if(jobs,[](const Job& queued) {
                    return queued.kind==Kind::Open || queued.kind==Kind::Cancel ||
                        queued.kind==Kind::Play || queued.kind==Kind::Pause ||
                        queued.kind==Kind::Restart || queued.kind==Kind::Update;
                });
            }
            if(job.kind==Kind::Update && std::any_of(jobs.begin(),jobs.end(),[](const Job& item) {
                return item.kind==Kind::Update;
            })) return;
            if(job.kind==Kind::Seek || job.kind==Kind::Position)
                std::erase_if(jobs,[&](const Job& queued){return queued.kind==job.kind;});
            jobs.push_back(std::move(job));
        }
        wake.notify_one();
    }
    void Notify(UINT message,uint64_t generation,HRESULT result=S_OK) {
        HWND window=notify.load();
        if(!closing && window && generation==latest.load())
            PostMessageW(window,message,static_cast<WPARAM>(generation),static_cast<LPARAM>(result));
    }
    void Reset() {
        ++playerEpoch;
        pendingGeneration=activeGeneration=0;
        pending.Reset();active.Reset();width=height=0;videoWindow=nullptr;
        positionMs=durationMs=0;
        if(player) {
            const auto start=GetTickCount64();
            player->Shutdown();player.Reset();callback.Reset();
            RuntimeLog(L"Preview shutdown ms="+std::to_wstring(GetTickCount64()-start));
        }
    }
    void CacheSize() {
        SIZE native{},aspect{};
        if(player && SUCCEEDED(player->GetNativeVideoSize(&native,&aspect))) {
            // 第二个返回值是显示宽高比（常见为 16、9），不是像素尺寸。
            // 这里缓存真实视频像素；裁剪比例需要时应单独处理像素宽高比。
            width=native.cx;height=native.cy;
        }
    }
    void Event(const Job& job) {
        // Shutdown 后到达的旧回调，不能被归到新播放器的请求上。
        if(job.epoch!=playerEpoch) return;
        uint64_t generation=job.generation;
        if(!generation && job.item) {
            if(job.item.Get()==pending.Get()) generation=pendingGeneration;
            else if(job.item.Get()==active.Get()) generation=activeGeneration;
        }
        if(FAILED(job.result)) {
            if(!generation && job.event==MFP_EVENT_TYPE_ERROR)
                generation=pendingGeneration?pendingGeneration:activeGeneration;
            // 无法归属的旧播放器错误不能把新请求误判失败。
            if(generation) Notify(errorMessage,generation,job.result);
            return;
        }
        if(job.event==MFP_EVENT_TYPE_MEDIAITEM_CREATED) {
            if(!player || generation!=latest || !job.item) return;
            pending=job.item;pendingGeneration=generation;
            const HRESULT result=player->SetMediaItem(job.item.Get());
            if(FAILED(result)) Notify(errorMessage,generation,result);
        } else if(job.event==MFP_EVENT_TYPE_MEDIAITEM_SET) {
            if(!player || job.item.Get()!=pending.Get() || pendingGeneration!=latest) return;
            active=pending;activeGeneration=pendingGeneration;pending.Reset();pendingGeneration=0;
            PROPVARIANT duration;PropVariantInit(&duration);
            if(SUCCEEDED(active->GetDuration(MFP_POSITIONTYPE_100NS,&duration)) && (duration.vt==VT_I8 || duration.vt==VT_UI8))
                durationMs=static_cast<int>((std::min)(duration.uhVal.QuadPart/10000,ULONGLONG(INT_MAX)));
            PropVariantClear(&duration);
            CacheSize();
            const HRESULT result=player->Play();
            if(FAILED(result)) Notify(errorMessage,activeGeneration,result);
        } else if(job.event==MFP_EVENT_TYPE_PLAY && job.item.Get()==active.Get()) {
            CacheSize();Notify(readyMessage,activeGeneration);
            if(activeGeneration==latest) RuntimeLog(L"Preview ready generation="+std::to_wstring(activeGeneration));
        } else if(job.event==MFP_EVENT_TYPE_PLAYBACK_ENDED && job.item.Get()==active.Get()) {
            Notify(endedMessage,activeGeneration);
        }
    }
    void Execute(const Job& job) {
        if(job.kind==Kind::Event) {Event(job);return;}
        if(job.kind==Kind::Stop) {Reset();return;}
        if(job.generation!=latest) return;
        if(job.kind==Kind::Open) {
            const std::wstring path = job.prepare ? job.prepare() : job.path;
            if (closing || job.generation != latest) return;
            if(player && videoWindow!=job.window) Reset();
            if(!player) {
                callback.Attach(new Callback(weak_from_this(),++playerEpoch));
                HRESULT result=MFPCreateMediaPlayer(nullptr,FALSE,MFP_OPTION_FREE_THREADED_CALLBACK,
                    callback.Get(),job.window,&player);
                if(FAILED(result)) {Notify(errorMessage,job.generation,result);return;}
                videoWindow=job.window;player->SetMute(TRUE);
            }
            pending.Reset();pendingGeneration=job.generation;width=height=0;
            const HRESULT result=player->CreateMediaItemFromURL(path.c_str(),FALSE,
                static_cast<DWORD_PTR>(job.generation),nullptr);
            if(FAILED(result)) Notify(errorMessage,job.generation,result);
            return;
        }
        if(!player) return;
        switch(job.kind) {
        case Kind::Cancel:player->Pause();pending.Reset();pendingGeneration=0;break;
        case Kind::Play:player->Play();break;
        case Kind::Pause:player->Pause();break;
        case Kind::Restart: {
            PROPVARIANT position;PropVariantInit(&position);position.vt=VT_I8;position.hVal.QuadPart=0;
            const HRESULT result=player->SetPosition(MFP_POSITIONTYPE_100NS,&position);
            PropVariantClear(&position);
            if(SUCCEEDED(result)) player->Play();else Notify(errorMessage,job.generation,result);
            break;
        }
        case Kind::Update:player->UpdateVideo();break;
        case Kind::Seek: {
            PROPVARIANT p;PropVariantInit(&p);p.vt=VT_I8;p.hVal.QuadPart=LONGLONG(job.positionMs)*10000;
            player->SetPosition(MFP_POSITIONTYPE_100NS,&p);PropVariantClear(&p);break;
        }
        case Kind::Position: {
            PROPVARIANT p;PropVariantInit(&p);
            if(SUCCEEDED(player->GetPosition(MFP_POSITIONTYPE_100NS,&p)) && (p.vt==VT_I8 || p.vt==VT_UI8))
                positionMs=static_cast<int>((std::min)(p.uhVal.QuadPart/10000,ULONGLONG(INT_MAX)));
            PropVariantClear(&p);break;
        }
        default:break;
        }
    }
    void Run() {
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const HRESULT media=SUCCEEDED(com)?MFStartup(MF_VERSION):com;
        for(;;) {
            Job job;
            {
                std::unique_lock lock(mutex);wake.wait(lock,[this]{return !jobs.empty();});
                job=std::move(jobs.front());jobs.pop_front();
            }
            if(job.kind==Kind::Quit) break;
            if(FAILED(media)) {Notify(errorMessage,job.generation,media);continue;}
            const auto start=GetTickCount64();Execute(job);
            const auto elapsed=GetTickCount64()-start;
            if(elapsed>250) RuntimeLog(L"Preview worker operation="+std::to_wstring(static_cast<int>(job.kind))+
                L" ms="+std::to_wstring(elapsed));
        }
        Reset();
        if(SUCCEEDED(media)) MFShutdown();
        if(SUCCEEDED(com)) CoUninitialize();
    }
};

PreviewPlayer::PreviewPlayer(HWND notify,UINT ready,UINT ended,UINT error):state_(std::make_shared<State>()) {
    state_->notify=notify;state_->readyMessage=ready;state_->endedMessage=ended;state_->errorMessage=error;
    worker_=std::thread([state=state_]{state->Run();});
}
PreviewPlayer::~PreviewPlayer() {
    state_->closing=true;state_->notify=nullptr;
    State::Job job;job.kind=State::Kind::Quit;state_->Push(std::move(job));
    // COM/显卡驱动不返回时，线程仍只持有自己的共享 State，不会引用已经销毁的 UI。
    // 每个媒体线程独立 Startup/Shutdown，主线程可正常退出，不无限 join 等待。
    if(worker_.joinable()) {
        if(WaitForSingleObject(worker_.native_handle(),2000)==WAIT_OBJECT_0) worker_.join();
        else {RuntimeLog(L"Preview worker shutdown exceeded 2s; UI teardown continues");worker_.detach();}
    }
}
uint64_t PreviewPlayer::OpenAsync(const std::wstring& path,HWND window,
                                 std::function<std::wstring()> prepare) {
    if(path.empty() || !window || state_->closing) return 0;
    State::Job job;job.kind=State::Kind::Open;job.generation=++state_->latest;
    const auto request=job.generation;job.path=path;job.window=window;job.prepare=std::move(prepare);
    state_->Push(std::move(job));return request;
}
void PreviewPlayer::CancelPending() {
    State::Job job;job.kind=State::Kind::Cancel;job.generation=++state_->latest;state_->Push(std::move(job));
}
void PreviewPlayer::Stop() {
    State::Job job;job.kind=State::Kind::Stop;job.generation=++state_->latest;
    state_->width=state_->height=0;state_->Push(std::move(job));
}
void PreviewPlayer::Play() {
    State::Job job;job.kind=State::Kind::Play;job.generation=state_->latest;state_->Push(std::move(job));
}
void PreviewPlayer::Pause() {
    State::Job job;job.kind=State::Kind::Pause;job.generation=state_->latest;state_->Push(std::move(job));
}
void PreviewPlayer::Restart() {
    State::Job job;job.kind=State::Kind::Restart;job.generation=state_->latest;state_->Push(std::move(job));
}
void PreviewPlayer::UpdateVideo() {
    State::Job job;job.kind=State::Kind::Update;job.generation=state_->latest;state_->Push(std::move(job));
}
bool PreviewPlayer::GetNativeVideoSize(SIZE& size) const {
    size={state_->width.load(),state_->height.load()};return size.cx>0 && size.cy>0;
}
void PreviewPlayer::Seek(int ms){State::Job job;job.kind=State::Kind::Seek;job.generation=state_->latest;
 job.positionMs=(std::max)(0,ms);state_->Push(std::move(job));}
void PreviewPlayer::PollPosition(){State::Job job;job.kind=State::Kind::Position;job.generation=state_->latest;state_->Push(std::move(job));}
int PreviewPlayer::PositionMs() const{return state_->positionMs;}
int PreviewPlayer::DurationMs() const{return state_->durationMs;}
