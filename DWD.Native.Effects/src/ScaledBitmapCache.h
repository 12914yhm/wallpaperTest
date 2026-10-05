#pragma once
#include <windows.h>
#include <list>

// 仅 UI 线程使用；最多 8 MiB，按实际显示尺寸缓存缩放结果。
// 删除源 HBITMAP 前必须 Forget，避免 Windows 重用句柄后命中旧图片。
class ScaledBitmapCache {
    struct Entry { HBITMAP source{}, scaled{}; int width{}, height{}; size_t bytes{}; };
public:
    ~ScaledBitmapCache() { Clear(); }
    void Clear() { for (auto& e : entries_) DeleteObject(e.scaled); entries_.clear(); bytes_ = 0; }
    void Forget(HBITMAP source) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->source != source) { ++it; continue; }
            bytes_ -= it->bytes; DeleteObject(it->scaled); it = entries_.erase(it);
        }
    }
    HBITMAP Get(HDC reference, HBITMAP source, int width, int height, const RECT& crop) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->source == source && it->width == width && it->height == height) {
                entries_.splice(entries_.begin(), entries_, it); return entries_.front().scaled;
            }
        }
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        if (width <= 0 || height <= 0 || bytes > limit_) return nullptr;
        HBITMAP scaled = CreateCompatibleBitmap(reference, width, height);
        HDC input = CreateCompatibleDC(reference), output = CreateCompatibleDC(reference);
        if (!scaled || !input || !output) {
            if (scaled) DeleteObject(scaled);
            if (input) DeleteDC(input); if (output) DeleteDC(output);
            return nullptr;
        }
        const auto oldInput = SelectObject(input, source), oldOutput = SelectObject(output, scaled);
        SetStretchBltMode(output, HALFTONE); SetBrushOrgEx(output, 0, 0, nullptr);
        const bool ok = StretchBlt(output, 0, 0, width, height, input, crop.left, crop.top,
            crop.right - crop.left, crop.bottom - crop.top, SRCCOPY) != FALSE;
        SelectObject(input, oldInput); SelectObject(output, oldOutput);
        DeleteDC(input); DeleteDC(output);
        if (!ok) { DeleteObject(scaled); return nullptr; }
        while (bytes_ + bytes > limit_ && !entries_.empty()) {
            bytes_ -= entries_.back().bytes; DeleteObject(entries_.back().scaled); entries_.pop_back();
        }
        entries_.push_front({source, scaled, width, height, bytes}); bytes_ += bytes;
        return scaled;
    }
private:
    static constexpr size_t limit_ = 8 * 1024 * 1024;
    size_t bytes_{};
    std::list<Entry> entries_;
};
