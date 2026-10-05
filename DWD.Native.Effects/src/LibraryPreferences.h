#pragma once
#include "WallpaperEffects.h"
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

struct LibraryPreference {
    bool favorite{};
    uint64_t lastUsed{};
    unsigned playlists{}; // 位 0 常规、位 1 白天、位 2 夜晚
    WallpaperEffects effects;
    std::wstring Encode() const {
        return std::to_wstring(favorite?1:0)+L","+std::to_wstring(lastUsed)+L","+
            std::to_wstring(playlists)+L","+effects.Encode();
    }
    static LibraryPreference Decode(const std::wstring& value){
        LibraryPreference p;int favorite{};unsigned long long used{};unsigned lists{};int n{};
        if(swscanf_s(value.c_str(),L"%d,%llu,%u,%n",&favorite,&used,&lists,&n)==3 && n>0){
            p.favorite=favorite!=0;p.lastUsed=used;p.playlists=lists&7;p.effects=WallpaperEffects::Decode(value.substr(n));
        }return p;
    }
};
inline unsigned CurrentPlaylist(bool scheduled,unsigned chosen,unsigned hour){
    return scheduled?(hour>=7 && hour<19?1U:2U):(std::min)(chosen,2U);
}
// 候选列表中优先排除当前壁纸；只有一项时不重建播放器。
inline size_t NextPlaylistIndex(const std::vector<size_t>& candidates,size_t current,bool random,uint64_t entropy){
    if(candidates.empty())return SIZE_MAX;
    std::vector<size_t> next;for(auto n:candidates)if(n!=current)next.push_back(n);
    if(next.empty())return SIZE_MAX;
    if(random)return next[entropy%next.size()];
    auto found=std::find(candidates.begin(),candidates.end(),current);
    if(found==candidates.end() || ++found==candidates.end())return candidates.front();return *found;
}
