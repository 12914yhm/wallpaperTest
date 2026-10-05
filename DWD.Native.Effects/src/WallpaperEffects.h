#pragma once
#include <windows.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string>

// 全部参数使用整数保存；INI、跨进程命令和 GPU 参数拥有同一套范围规则。
struct WallpaperEffects {
    int kind{}; // 0 无、1 萤火虫、2 细雨、3 飘雪
    int density{35}, speed{50}, opacity{45};
    int brightness{100}, saturation{100}, warmth{};
    void Clamp() {
        kind=std::clamp(kind,0,3);density=std::clamp(density,0,100);
        speed=std::clamp(speed,1,100);opacity=std::clamp(opacity,0,100);
        brightness=std::clamp(brightness,30,160);saturation=std::clamp(saturation,0,180);
        warmth=std::clamp(warmth,-50,50);
    }
    bool Particles() const {return kind && density && opacity;}
    bool Enabled() const {return Particles() || brightness!=100 || saturation!=100 || warmth!=0;}
    bool operator==(const WallpaperEffects&) const = default;
    std::wstring Encode() const {
        return std::to_wstring(kind)+L","+std::to_wstring(density)+L","+std::to_wstring(speed)+L","+
            std::to_wstring(opacity)+L","+std::to_wstring(brightness)+L","+
            std::to_wstring(saturation)+L","+std::to_wstring(warmth);
    }
    static WallpaperEffects Decode(const std::wstring& text) {
        WallpaperEffects result, parsed;
        if(swscanf_s(text.c_str(),L"%d,%d,%d,%d,%d,%d,%d",&parsed.kind,&parsed.density,
            &parsed.speed,&parsed.opacity,&parsed.brightness,&parsed.saturation,&parsed.warmth)==7) result=parsed;
        result.Clamp();return result;
    }
};

// 一个控制器写参数，一个播放器读；互斥锁采用零等待，争用时复用上一帧，绝不堵住 UI。
// 位置/时长单独使用原子整数（毫秒），不把 C++ 对象/指针传入另一进程。
struct EffectSharedData {
    WallpaperEffects effects;
    volatile LONG positionMs{}, durationMs{}, seekMs{-1};
    volatile LONG frames{},error{},energyPaused{};
};
class EffectChannel {
public:
    EffectChannel()=default;
    ~EffectChannel(){Close();}
    EffectChannel(const EffectChannel&)=delete;
    EffectChannel& operator=(const EffectChannel&)=delete;
    bool Open(const std::wstring& name,bool create) {
        Close();
        lock_=create?CreateMutexW(nullptr,FALSE,(name+L"_Lock").c_str()):
            OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L"_Lock").c_str());
        mapping_=create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,
            sizeof(EffectSharedData),name.c_str()):OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name.c_str());
        if(mapping_) data_=static_cast<EffectSharedData*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(EffectSharedData)));
        if(!lock_ || !data_){Close();return false;}
        if(create) {data_->effects={};data_->positionMs=data_->durationMs=data_->frames=data_->error=data_->energyPaused=0;data_->seekMs=-1;}
        return true;
    }
    void Close(){if(data_)UnmapViewOfFile(data_);if(mapping_)CloseHandle(mapping_);if(lock_)CloseHandle(lock_);
        data_=nullptr;mapping_=lock_=nullptr;}
    bool Write(WallpaperEffects value) {
        if(!Acquire())return false;value.Clamp();data_->effects=value;ReleaseMutex(lock_);return true;
    }
    WallpaperEffects Read(WallpaperEffects fallback) {
        if(!Acquire())return fallback;auto value=data_->effects;ReleaseMutex(lock_);value.Clamp();return value;
    }
    EffectSharedData* Data() const{return data_;}
private:
    bool Acquire(){if(!data_ || !lock_)return false;const auto r=WaitForSingleObject(lock_,0);return r==WAIT_OBJECT_0 || r==WAIT_ABANDONED;}
    HANDLE mapping_{},lock_{};EffectSharedData* data_{};
};
