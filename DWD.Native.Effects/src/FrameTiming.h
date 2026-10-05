#pragma once
#include <algorithm>
#include <cstdint>

// 解码依赖保持不变，只省掉已过期帧的 GPU 缩放/显示；首帧与最多连续五帧后强制显示。
inline bool SkipLatePresentation(int64_t lateMilliseconds, int64_t frameMilliseconds,
                                 bool hasFirstFrame, unsigned skippedFrames) {
    return hasFirstFrame && skippedFrames < 5 &&
        lateMilliseconds > (std::max<int64_t>)(100, frameMilliseconds * 2);
}
