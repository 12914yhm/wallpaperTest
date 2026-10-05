cmake_minimum_required(VERSION 3.24)
project(DWDNative LANGUAGES CXX)

# 使用 C++20，并把 MSVC 运行库静态链接进 EXE。
# 这样发布目录不需要额外附带 Visual C++ 运行库 DLL，更符合“绿色软件”的目标。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Controller 是前台控制器；Player 是同一 EXE 的 --player 后台模式；
# app.rc/app.ico 把图标真正嵌入 PE 资源，而不是只在运行时设置窗口图标。
add_executable(DWDNative WIN32 src/Controller.cpp src/Player.cpp src/EffectsRenderer.cpp src/ProxyTranscoder.cpp src/ProxyTranscoder.h
                               src/PreviewPlayer.cpp src/PreviewPlayer.h
                               src/SharedPreviewRenderer.cpp src/SharedPreviewRenderer.h
                               src/UsageTracker.cpp src/UsageTracker.h
                               src/app.manifest resources/app.rc resources/app.ico
                               resources/line-md--cog.svg)
target_compile_definitions(DWDNative PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 全部依赖都来自 Windows SDK：Media Foundation、D3D11、Shell、Win32 UI 等。
# 项目没有引入 VLC、libmpv 或其他需要随程序分发的第三方播放 DLL。
target_link_libraries(DWDNative PRIVATE comdlg32 comctl32 dwmapi shell32 shlwapi ole32 propsys user32 gdi32 imm32
                                        gdiplus mf mfplat mfreadwrite mfplay mfuuid d3d11 dxgi d3dcompiler windowscodecs wtsapi32 advapi32 psapi version)

target_compile_options(DWDNative PRIVATE /W4 /permissive- /EHsc /utf-8)

install(TARGETS DWDNative RUNTIME DESTINATION .)
install(FILES README.md DESTINATION . RENAME "使用说明.md")

# 离线测试只留在 build 中，发布时仍然只有一个应用 EXE。
include(CTest)
if(BUILD_TESTING)
    add_executable(DWDEffectsIntegrationTests tests/EffectsIntegrationTests.cpp)
    target_include_directories(DWDEffectsIntegrationTests PRIVATE src)
    target_compile_definitions(DWDEffectsIntegrationTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDEffectsIntegrationTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDEffectsIntegrationTests PRIVATE user32)
    add_executable(DWDEffectsTests tests/EffectsTests.cpp src/EffectsRenderer.cpp)
    target_include_directories(DWDEffectsTests PRIVATE src)
    target_compile_definitions(DWDEffectsTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDEffectsTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDEffectsTests PRIVATE d3d11 d3dcompiler)
    add_test(NAME effects-gpu-preferences-playlist COMMAND DWDEffectsTests)
    add_executable(DWDOptimizationTests tests/OptimizationTests.cpp)
    target_include_directories(DWDOptimizationTests PRIVATE src)
    target_compile_definitions(DWDOptimizationTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDOptimizationTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDOptimizationTests PRIVATE user32 gdi32 shlwapi psapi)
    add_test(NAME optimization-storage-memory-scanning COMMAND DWDOptimizationTests)
    add_executable(DWDHostTests tests/WallpaperHostTests.cpp)
    target_include_directories(DWDHostTests PRIVATE src)
    target_compile_definitions(DWDHostTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDHostTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDHostTests PRIVATE user32)
    add_test(NAME wallpaper-host-geometry COMMAND DWDHostTests)
    # 真实双屏/桌面渲染验证需在交互桌面手动运行，不纳入无人桌面的 CTest。
    add_executable(DWDWallpaperTests tests/WallpaperIntegrationTests.cpp)
    target_include_directories(DWDWallpaperTests PRIVATE src)
    target_compile_definitions(DWDWallpaperTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDWallpaperTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDWallpaperTests PRIVATE user32 gdi32)
    add_executable(DWDUsageTests tests/UsageTrackerTests.cpp src/UsageTracker.cpp)
    target_include_directories(DWDUsageTests PRIVATE src)
    target_compile_definitions(DWDUsageTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDUsageTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDUsageTests PRIVATE user32 shlwapi version psapi)
    add_test(NAME usage-history-and-modes COMMAND DWDUsageTests)
    # 真实视频回归手动传入两条路径；不把用户资产路径写入 CTest 或发布目录。
    add_executable(DWDPreviewTests tests/PreviewIntegrationTests.cpp src/PreviewPlayer.cpp)
    target_include_directories(DWDPreviewTests PRIVATE src)
    target_compile_definitions(DWDPreviewTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DWDPreviewTests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_libraries(DWDPreviewTests PRIVATE user32 ole32 propsys mfplat mfplay mfuuid)
endif()
