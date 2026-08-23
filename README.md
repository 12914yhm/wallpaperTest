# DWD · 动态壁纸

Windows 桌面动态壁纸应用。当前版本使用 WPF 作为控制界面，并通过内置 mpv 播放器将视频挂载到桌面图标层背后。

## 当前功能

- 导入 MP4 / WebM / AVI / MOV / MKV 视频并预览
- 循环播放和静音播放
- 拖动选择桌面显示区域
- `16:9 · 每屏相同`：两个显示器展示同一选区，并按各自分辨率适配
- `跨屏拼接`：将选区作为一张跨越虚拟桌面的画面
- 多显示器和不同 DPI 坐标适配
- 托盘常驻、单实例启动和安全退出
- mpv 硬件解码；双屏平铺模式使用硬件解码复制帧后进行布局滤镜处理

## 开发环境

- Windows 10/11
- .NET 10 SDK
- Git LFS

## 运行

```powershell
dotnet run
```

项目依赖的 mpv 运行时位于 `player/`。其中 `mpv.exe` 由 Git LFS 管理。

## 发布

```powershell
dotnet publish DWD.csproj -c Release -r win-x64 --self-contained true `
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
  -o publish
```

发布目录和压缩包不会提交到 Git；它们适合作为 Release 附件分发。
