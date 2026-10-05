# DWD 代码阅读路线

这份文档配合源码中的中文注释使用。建议先理解数据流和生命周期，再深入 Win32、Media Foundation 与 D3D11 的具体 API。

## 1. 先看程序入口

从 `src/Controller.cpp` 的 `wWinMain` 开始。

入口首先解析命令行：

- 有 `--player`：调用 `RunPlayer`，进入 `Player.cpp` 的后台壁纸模式。
- 有 `--proxy`：调用 `RunProxyTranscoder`，进入 `ProxyTranscoder.cpp` 的后台转码模式。
- 没有 `--player`：创建控制窗口、初始化 COM 和 Media Foundation，进入 Win32 消息循环。

这就是“一个 EXE、三种运行角色”的实现方式。任务管理器中暂时出现多个同名进程，并不代表发布目录里有多个程序文件。

接着看 `WindowProc`。Win32 程序不是按顺序不断执行界面逻辑，而是由系统发送消息：

- `WM_CREATE`：创建预览窗口、按钮和取景框。
- `WM_COMMAND`：接收按钮点击。
- `WM_PAINT`：绘制界面。
- `WM_SIZE` / `WM_DPICHANGED`：重新布局。
- `WM_CLOSE`：标题栏叉号隐藏到托盘；托盘退出设置退出标记后才进入 `WM_DESTROY` 清理资源。
- `WM_APP + n`：程序自定义的线程/进程协作消息。

`MemoryBallProc` 是独立的无边框工具窗口。它通过 `GetProcessMemoryInfo` 每秒读取控制器、
播放器和代理转码器的工作集并合计显示；`WM_NCHITTEST` 返回 `HTCAPTION`，所以整个圆形
区域都可以拖动。双击悬浮球会向主窗口发送恢复消息。

悬浮球圆周使用 `UpdateLayeredWindow` 的每像素 Alpha，而不是 `SetWindowRgn` 的整数像素
裁切。GDI+ 先在 32 位 PARGB 位图上抗锯齿绘制圆和文字，再交给 DWM 合成透明边缘。

## 使用时长统计

`UsageTracker.cpp` 用 `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` 接收前台应用切换事件，
不需要循环扫描进程。主窗口每两秒调用一次 `Tick`，并同时维护两个计数器：活跃时长通过
`GetLastInputInfo` 在连续 60 秒无输入后暂停；观看时长只在锁屏、休眠或会话断开时暂停。
设置中的模式只决定排行榜显示哪个计数器，因此切换模式不会把两种口径混在同一个数字里。
统计器从前台窗口得到进程路径，按应用聚合毫秒数，但不会保存窗口标题、键盘内容或鼠标坐标。

数据以 UTF-16 V2 小文件按天写入 `UsageData`；旧版 V1 活跃记录会自动兼容加载。写盘时先
创建 `.tmp`，成功后再原子替换正式
文件，避免断电留下半个文件。统计窗口只排序当日聚合结果，打开与否不会改变采样开销。

## 屏幕变化与唤醒

曾经用于实验的 `SetDisplayConfig` 拓扑重放及输出数量诊断已撤销；DWD 不对雷电/HDMI
接口断电，也不主动重置用户显示模式。播放器保留正常 `WM_DISPLAYCHANGE` 去抖重建和
锁屏/睡眠后的解码资源恢复。宿主查找规则现在集中在 `WallpaperHost.h`，兼容新版
Progman 内部 WorkerW 与旧版顶层 WorkerW。

## 2. 跟一次视频选择

阅读顺序：

1. `ChooseWallpaperFolder`
2. `ScanWallpaperFolder`
3. `StartThumbnailWorker`
4. `LoadThumbnail`
5. `SelectVideo`
6. `ScheduleCurrentPreview`
7. `PreviewPlayer::OpenAsync`

文件夹扫描只收集路径和标题。封面生成放到工作线程，因为 Shell 提取视频缩略图可能比较慢。

工作线程不能直接修改窗口或 `g_videos`，所以它把 `ThumbnailResult` 指针放进 `kThumbnailReady` 消息。UI 线程在 `WindowProc` 中接管结果并重绘对应卡片。这是典型的“后台线程做重活，主线程更新 UI”模式。

只有被选中的视频会使用实时预览，右侧其他视频只是 320×180 的静态封面。MFPlay
对象会常驻复用，切换时异步替换 MediaItem；120 ms 防抖和请求代次共同避免连续点击时重复初始化或旧回调串入新视频。

`OpenAsync`、`Pause`、`Play`、`Stop` 只入队。独立 MTA 媒体线程按顺序调用 MFPlay；
MF 回调也只入队，不在回调或 UI 线程中调用 `SetMediaItem`/`Shutdown`。State 的
generation 标识最新选择，epoch 标识当前播放器，防止已释放播放器的旧事件串入新实例。
`GetNativeVideoSize` 读取原子缓存，不访问驱动；第二个 MF 返回值是宽高比，不是像素。
控制器额外提供 12 秒加载超时，用户能再次选择重试。

壁纸应用成功后调用 `ActivateSharedPreview`：它先请求异步释放 MFPlay，再通过
`SharedPreviewRenderer::Open` 打开后台播放器创建的命名 D3D11 纹理。后台每当写入一张
共享预览帧，就发送 `kSharedPreviewFrame`；前台收到后执行一次 GPU 复制和 Present。
这样控制窗口仍显示动态视频，但 4K 解码器只保留后台的一份。

普通预览和共享预览分别使用两个 HWND，布局一致且互斥显示，不能把 EVR 与 DXGI flip
交换链混到同一个窗口。共享 Present 使用 DO_NOT_WAIT，忙时跳过一帧。异步交接期间
资源可能短暂重叠，不能据此承诺整个交接过程只有一个解码器。

## 3. 理解封面缓存

阅读顺序：

1. `ThumbnailCacheKey`
2. `ThumbnailCachePath`
3. `NormalizeThumbnail`
4. `SaveBitmapFile`
5. `LoadThumbnail`

缓存键由视频路径、文件大小和最后修改时间共同计算。视频内容被替换后，即使文件名相同，也会自然得到一个新缓存文件。

缓存只保存封面 BMP，不保存视频帧序列，也不会把完整视频加载到内存。统一 320×180 是为了控制磁盘、内存和 GDI 对象占用。

## 4. 跟一次“应用到桌面”

从 `Controller.cpp` 的 `LaunchPlayer` 开始。

主要步骤：

1. 读取原片尺寸，并检查是否已有适配当前屏幕的代理缓存。
2. 暂停前台预览，减少新播放器初始化时的 GPU 竞争。
3. 创建唯一的 `stopEvent` 和 `readyEvent`。
4. 用 `CreateProcessW` 启动同一个 EXE，并传入 `--player`、播放路径和焦点坐标。
5. `WaitForPlayer` 等待新播放器首帧，同时继续处理 UI 消息。
6. 新播放器 ready 后，停止旧播放器并完成交接。
7. 若原片明显大于屏幕且还没有代理，调用 `StartProxyJob` 在后台生成；完成后再次使用同一套首帧安全交接切到代理。

不要把第 5、6 步改成“先停旧播放器，再启动新的”。后者在新视频初始化失败或首帧较慢时会直接暴露黑色 WorkerW，这正是之前随机黑屏的根源。

## 5. 理解屏幕适配代理

阅读顺序：

1. `LoadVideoDimensions`
2. `BuildProxySpec`
3. `StartProxyJob`
4. `RunProxyTranscoder`
5. `kProxyReady`

代理不是把整段未压缩画面放进内存，而是另存一份适合最大显示器的压缩 H.264 MP4。它把 4K/60fps 原片降低到屏幕实际需要的分辨率和 30fps，所以硬件解码器需要保留的参考帧表面更小。只有比例接近屏幕且明显大于屏幕的原片才生成代理，竖屏或超宽素材仍直接播放原文件。

转码写入 `.part.mp4`，成功后才改成正式 `.mp4`；停止任务、切换缓存目录或退出程序都会取消未完成任务。`EnforceProxyCacheLimit` 按最近使用时间清理旧代理，避免缓存无限增长。

## 6. 看后台如何挂到桌面

转到 `src/Player.cpp`，从 `RunPlayer` 开始。

阅读顺序：

1. `FindWallpaperParent`
2. `MonitorRects`
3. 每块显示器的 `CreateWindowExW`
4. `NativePlayer::Open`
5. `CreateRenderTargets`
6. `DecodeLoop`
7. `RenderFrame`

`FindWallpaperParent` 找到位于桌面图标后面的 WorkerW。每块显示器创建一个子窗口，但所有窗口共享同一个 `NativePlayer` 和同一个 Source Reader。

Media Foundation 将视频硬件解码为 NV12 D3D11 纹理。`RenderFrame` 不把纹理复制回 CPU，而是把同一张纹理分别交给每块屏幕的 D3D11 Video Processor。

因此双屏意味着“两套输出”，不是“两套解码”。

## 7. 理解双屏裁剪

控制器中的取景框负责选择焦点：

- `DisplayedVideoRect`：算出视频在预览窗口中真正占据的区域，排除黑边。
- `CropFrameRect`：按当前显示器比例计算紫色框。
- `SetCropFocusFromPoint`：把鼠标位置变成 0~1 焦点。

后台播放器中的 `SourceCrop` 负责真正裁剪：

- 源视频更宽：裁左右。
- 源视频更高：裁上下。
- 每块屏幕使用自己的目标比例。
- 所有屏幕围绕同一个焦点。

所以两个比例不同的显示器不会使用完全相同的像素矩形，但都会尽量保留用户选择的主体。

## 8. 看播放节奏与省电

`NativePlayer::DecodeLoop` 读取视频时间戳，并用 `steady_clock` 等待每帧应显示的时间。没有这层节奏控制，Source Reader 会尽可能快地读取，视频可能高速播放并占满 GPU。

`WallpaperProc` 每秒检查一次：

- 前台是否是全屏窗口；
- 用户会话是否锁定；
- 系统是否挂起。

全屏或锁屏刚发生时只暂停解码，避免短暂切换造成重建开销；连续 30 秒后调用
`NativePlayer::ReleaseDecoder` 进入深度休眠：释放 Source Reader、双屏交换链并销毁
真正承载壁纸的 HWND。一个不显示画面的协调窗口会继续接收系统消息。

恢复时重新创建显示器窗口和解码器，并从暂停前时间点附近继续。只有首帧成功提交后
`kReadyMessage` 才显示壁纸窗口，所以恢复过程中露出的是 Windows 静态壁纸，而不是黑屏。

## 9. 看退出与资源所有权

`WM_CLOSE` 默认只调用 `MinimizeToTray`，不会销毁窗口。托盘右键“退出程序”先设置
`g_forceExit`，再发送 `WM_CLOSE`，这是界面中唯一会进入销毁流程的退出路径。

收回托盘时先隐藏主窗口、两个预览窗口和取景层，随后异步停止 MF 预览；不会等媒体
Shutdown 完成才收起。预览析构最多等 2 秒，超时线程只持有独立 State；后台线程独立
管理 COM/MF 生命周期，不引用已经销毁的 UI 对象。开发 `--exit` 发送退出事件，与托盘
退出走同一清理流程，但不是普通用户的新增界面按钮。

控制器正常退出时：

1. 释放预览播放器。
2. 停止缩略图线程并删除 HBITMAP。
3. 触发后台播放器的 stopEvent。
4. 等待后台释放 D3D11 和 Media Foundation。
5. 通知 Explorer 刷新静态壁纸。

常见资源所有权：

- COM/D3D 接口：`ComPtr` 自动 `Release`。
- `HANDLE`：必须 `CloseHandle`。
- `HBITMAP`、字体、画刷、画笔：必须 `DeleteObject`。
- `HDC`：`CreateCompatibleDC` 对应 `DeleteDC`。
- `new ThumbnailResult`：由接收消息的 UI 线程用 `unique_ptr` 接管。

## 推荐学习顺序

1. 熟悉 C++ RAII、智能指针和线程原子变量。
2. 学习 Win32 窗口类、消息循环和 GDI 绘制。
3. 学习 COM 与 HRESULT 错误处理。
4. 了解 Media Foundation Source Reader。
5. 了解 D3D11 Texture、SwapChain 和 Video Processor。
6. 最后研究 WorkerW；它属于 Explorer 的非正式桌面实现细节，不应作为最先学习的部分。

## 新增功能的学习入口

- `DirectoryWatcher.h`：后台线程等待系统目录事件，通过 PostMessage 通知 UI。窗口线程处理 `kDirectoryTimer` 做去抖和文件稳定检查。
- `RebuildGallery`：搜索/排序只调整 `g_galleryIndices`，实际 `VideoItem` 不搬动；分页和封面任务根据索引选择。
- `SaveConfig / SaveVideoFocus / ScreenFocus`：UTF-16 便携 INI 原子替换。公共焦点兼容旧版，屏幕专属焦点用“视频路径哈希_显示设备哈希”区分。
- `LayoutControls / SettingActionLabel / ApplyTheme`：统一控件布局、可访问名称与主题对比度；`g_cropEditing` 决定取景框是否出现。
- `ToggleWallpaperPause`：控制器设置/复位命名事件，后台在节能计时器中读取；无需键鼠模拟或重复启动播放器。
- `MonitorRects / CreateRenderTargets / SourceCrop`：目标屏幕筛选、单解码多输出，以及每个输出独立的归一化取景位置。
- `RecoverWallpaper`：遇到解码/显卡错误时重新创建播放器，保留原来的首帧交接保护，限制连续重试次数。
- `CacheFiles / ManagedCacheFile / ClearCache`：只清理程序生成的缓存，保护当前播放文件；容量预算不是对用户整个文件夹的递归清理。
- `UsageTracker::RecordsForDay / AvailableDays / SetSuspended`：历史聚合数据、两种口径和睡眠排除；不是 SRUM 历史推断。
- `tests/UsageTrackerTests.cpp`：Windows 隐藏通知窗口的离线测试，写入自有固定格式测试数据，验证后删除明确创建的文件。
# 优化副本导读补充

本目录新增的 `AsyncFileStore.h`（后台保存）、`LibraryScanner.h`（后台扫描）、
`ProcessCleanup.h`（进程清理）、`MemoryMetrics.h`（私有内存）、`ScaledBitmapCache.h`
（缩放封面缓存）和 `FrameTiming.h`（播放节奏）各自负责一件事。
先读 `OPTIMIZATION_NOTES.md`，再结合下方原版导读理解主流程；原版的同步保存/扫描描述已被替换。
