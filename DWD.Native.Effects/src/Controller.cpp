#include <windows.h>
#include <windowsx.h>
#include <wtsapi32.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <imm.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propkey.h>
#include <propsys.h>
#include <psapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include "PreviewPlayer.h"
#include "ProxyTranscoder.h"
#include "SharedPreviewRenderer.h"
#include "UsageTracker.h"
#include "DirectoryWatcher.h"
#include "RuntimeDiagnostics.h"
#include "AsyncFileStore.h"
#include "MemoryMetrics.h"
#include "ProcessCleanup.h"
#include "LibraryScanner.h"
#include "ScaledBitmapCache.h"
#include "LibraryPreferences.h"
#include "ImageSource.h"
#include <map>
#include <set>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 本文件是程序的前台控制器，也是应用的主入口。它负责：
// 1. 原生 Win32 界面、视频库、设置抽屉和常驻托盘入口；
// 2. MFPlay 视频预览与可拖动取景框；
// 3. 便携 INI、封面磁盘缓存、屏幕适配视频代理和异步缩略图；
// 4. 启动同一个 EXE 的 --player 后台模式，并安全交接新旧壁纸进程。
//
// Controller 不直接解码桌面壁纸，原因是把后台播放器隔离成独立进程后，切换失败
// 不会破坏当前壁纸，控制窗口退出时也能通过事件可靠地通知播放器清理资源。
int RunPlayer(HINSTANCE instance);

namespace {
constexpr wchar_t kWindowClass[] = L"DWDNativeController";
constexpr wchar_t kCropOverlayClass[] = L"DWDNativeCropOverlay";
constexpr wchar_t kMemoryBallClass[] = L"DWDMemoryBall";
constexpr wchar_t kUsageWindowClass[] = L"DWDUsageWindow";
constexpr int kAppIconResource = 101;
constexpr int kSettingsButton = 1001;
constexpr int kApplyButton = 1002;
constexpr int kChooseFolderButton = 1003;
constexpr int kChooseCacheButton = 1004;
constexpr int kRefreshButton = 1005;
constexpr int kChooseBackgroundButton = 1006;
constexpr int kUsageButton = 1007;
constexpr int kActiveModeButton = 1008;
constexpr int kViewingModeButton = 1009;
constexpr int kTrayOpen = 1101;
constexpr int kTrayExit = 1102;
constexpr int kTrayUsage = 1103;
constexpr int kPauseButton = 1010, kStopButton = 1011, kCropButton = 1012;
constexpr int kResetCropButton = 1013, kPreviousPage = 1014, kNextPage = 1015;
constexpr int kSearchEdit = 1016, kSortCombo = 1017, kAutoRestore = 1018;
constexpr int kBallToggle = 1019, kUsageToggle = 1020, kClearThumbnails = 1021;
constexpr int kClearProxies = 1022, kCacheLimit = 1023, kFullscreenRule = 1024;
constexpr int kQuality = 1025, kTabFirst = 1200, kThemeFirst = 1210;
constexpr int kScreenTarget=1026,kCropMonitor=1027;
constexpr int kFavorite=1400,kLibraryFilter=1401,kEffectsButton=1402,kMoreButton=1403,kSeekBar=1404;
constexpr UINT_PTR kExperienceTimer=13,kEffectPreviewTimer=14;
constexpr int kUsageOlder=1300,kUsageNewer=1301,kUsageToday=1302,kUsageActive=1303,kUsageViewing=1304;
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kRestoreMessage = WM_APP + 11;
constexpr UINT kThumbnailReady = WM_APP + 12;
constexpr UINT kPreviewEnded = WM_APP + 13;
constexpr UINT kPreviewError = WM_APP + 14;
constexpr UINT kPreviewReady = WM_APP + 15;
constexpr UINT kSharedPreviewFrame = WM_APP + 16;
constexpr UINT kProxyReady = WM_APP + 17;
constexpr UINT kForegroundChanged = WM_APP + 18;
constexpr UINT kDirectoryChanged = WM_APP + 20;
constexpr UINT kWallpaperError = WM_APP + 21;
constexpr UINT kLibraryReady = WM_APP + 22;
constexpr UINT_PTR kSharedIdleTimer = 9;
constexpr UINT_PTR kProxyIdleTimer = 10;
constexpr UINT_PTR kLibraryApplyTimer = 11;
constexpr UINT_PTR kSearchTimer = 12;
constexpr UINT_PTR kDirectoryTimer = 6;
constexpr UINT_PTR kRecoveryTimer = 7;
constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kOverlayTimer = 2;
constexpr UINT_PTR kPreviewSwitchTimer = 3;
constexpr UINT_PTR kPreviewTimeoutTimer = 8;
constexpr UINT_PTR kMemoryTimer = 4;
constexpr UINT_PTR kUsageTimer = 5;
HANDLE g_developmentExit{};
constexpr UINT kPreviewSwitchDelayMs = 120;
constexpr int kGalleryPageSize = 6;
constexpr uint64_t kProxyCacheLimitBytes = 2ULL * 1024 * 1024 * 1024;
// 新主题使用“墨蓝画布 + 蓝灰卡片”，避免纯黑背景把界面压成一整块。
constexpr COLORREF kBackground = RGB(22, 28, 38);
COLORREF kPanel = RGB(34, 43, 57);
COLORREF kPanelHover = RGB(45, 57, 75);
COLORREF kBorder = RGB(64, 78, 101);
COLORREF kText = RGB(242, 245, 250);
COLORREF kMuted = RGB(153, 164, 182);
COLORREF kAccent = RGB(111, 124, 255);
COLORREF kAccentHover = RGB(134, 146, 255);
COLORREF kSuccess = RGB(72, 211, 160);
COLORREF kWarning = RGB(244, 190, 73);
COLORREF kError = RGB(242, 100, 116);
COLORREF kPreviewSurface = RGB(11, 16, 23);
COLORREF kControlSurface = RGB(28, 36, 49);
COLORREF kDrawerSurface = RGB(25, 32, 43);

struct PlayerInstance {
    // 控制器持有后台播放器进程和两个命名事件：
    // stopEvent 让播放器优雅退出；readyEvent 表示新播放器首帧已显示。
    HANDLE process{};
    HANDLE stopEvent{};
    HANDLE readyEvent{};
    HANDLE pauseEvent{};
    std::wstring previewTextureName;
    std::wstring videoPath;
    std::wstring playbackPath;
    std::wstring screenDevice,screenFocus;
    double focusX{0.5};
    double focusY{0.5};
    int quality{},fullscreenRule{};
    std::shared_ptr<EffectChannel> channel;
    WallpaperEffects effects;
};

struct VideoItem {
    std::wstring path;
    std::wstring title;
    HBITMAP thumbnail{};
    UINT width{};
    UINT height{};
    // 取景焦点使用 0~1 归一化坐标；0.5/0.5 表示视频中心。
    // 旧版公共焦点用作默认值；新的每屏焦点保存在便携 Focus 配置中。
    double focusX{0.5};
    double focusY{0.5};
    uint64_t modified{};
    uint64_t fileSize{}, thumbnailUse{};
    std::wstring searchTitle;
};

struct ThumbnailResult {
    // 后台缩略图线程不能直接改 UI 数据，完成后把这个结果通过 PostMessage 交给主线程。
    unsigned generation{};
    size_t index{};
    HBITMAP bitmap{};
    UINT width{};
    UINT height{};
};

struct ThumbnailTask {
    unsigned generation{};
    size_t index{};
    std::wstring path;
    std::wstring cacheFolder;
};

struct ProxySpec {
    bool eligible{};
    UINT width{};
    UINT height{};
    UINT fps{30};
    UINT bitrate{};
    std::wstring finalPath;
    std::wstring temporaryPath;
};

struct ProxyResult {
    unsigned generation{};
    DWORD exitCode{};
    std::wstring sourcePath;
    std::wstring finalPath;
    std::wstring temporaryPath;
};

struct ProxyJob {
    HANDLE process{};
    HANDLE stopEvent{};
    std::thread waiter;
    unsigned generation{};
    std::wstring sourcePath;
    std::wstring finalPath;
    std::wstring temporaryPath;
};

std::wstring g_videoPath;
std::wstring g_wallpaperFolder;
std::wstring g_cacheFolder;
std::wstring g_statusText = L"请从设置中选择壁纸文件夹";
COLORREF g_statusColor = kMuted;
PlayerInstance g_player;
std::vector<VideoItem> g_videos;
// 图库索引与视频对象分离：搜索和排序只移动小整数，不重建解码器或封面。
std::vector<size_t> g_galleryIndices;
std::map<std::wstring, std::pair<double, double>> g_savedFocus;
std::map<int, HWND> g_extraButtons;
HWND g_searchEdit{}, g_sortCombo{}, g_tooltip{};
HBRUSH g_controlBrush{};
int g_settingsTab{}, g_theme{}, g_sortOrder{}, g_quality{}, g_fullscreenRule{};
bool g_cropEditing{}, g_wallpaperPaused{}, g_showMemoryBall{true};
bool g_autoRestore{}, g_usageEnabled{true};
uint64_t g_cacheLimitBytes{kProxyCacheLimitBytes}, g_cacheUsageBytes{};
COLORREF g_customBackground{kBackground};
std::wstring g_lastWallpaper, g_searchText, g_usageDate;
struct ScreenOption {std::wstring device;RECT rect{};bool primary{};};
std::vector<ScreenOption> g_screens;
std::wstring g_targetMonitor,g_cropMonitor;
void RefreshScreenOptions();
std::wstring CurrentCropDevice();
std::wstring BuildScreenFocusMap(const std::wstring& video);

size_t g_usageScroll{};
std::wstring g_historyDate;
std::vector<UsageRecord> g_historyRecords;
DirectoryWatcher g_directoryWatcher;
LibraryScanner g_libraryScanner;
ScaledBitmapCache g_scaledBitmaps;
std::wstring g_requestedFolder;
bool g_scanPending{}, g_autoApplyPending{};
unsigned g_libraryGeneration{};
std::unique_ptr<LibrarySnapshot> g_deferredLibrary;
uint64_t g_thumbnailUse{};
ULONGLONG g_lastInteraction{};
std::wstring g_pendingProxySource;
ProxySpec g_pendingProxySpec;
uint64_t g_directorySignature{};
unsigned g_recoveryAttempts{};
void SaveVideoFocus();
void ApplyTheme(HWND owner);
void RebuildGallery(HWND owner, bool preservePage = false);
void RefreshCacheUsage();
bool SaveConfig();
bool ManagedCacheFile(const std::filesystem::path& path,bool proxy);
bool LaunchPlayer(HWND owner);
void ToggleWallpaperPause(HWND owner);
void StopWallpaper(HWND owner);

std::unique_ptr<PreviewPlayer> g_previewPlayer;
std::unique_ptr<SharedPreviewRenderer> g_sharedPreview;
HWND g_mainWindow{};
HWND g_previewWindow{};
// MFPlay/EVR 与 DXGI flip 模型必须拥有各自的 HWND，不能轮流接管同一窗口。
HWND g_sharedPreviewWindow{};
HWND g_cropOverlay{};
HWND g_settingsButton{};
HWND g_applyButton{};
HWND g_chooseFolderButton{};
HWND g_chooseCacheButton{};
HWND g_chooseBackgroundButton{};
HWND g_usageButton{};
HWND g_activeModeButton{};
HWND g_viewingModeButton{};
HWND g_refreshButton{};
HWND g_memoryBall{};
HWND g_ballTooltip{};
HWND g_usageWindow{};
HWND g_hoverButton{};
HFONT g_titleFont{};
HFONT g_subtitleFont{};
HFONT g_bodyFont{};
HFONT g_smallFont{};
HFONT g_buttonFont{};
HFONT g_usageControlsFont{};
UINT g_usageControlsDpi{};
// 主界面复用一张后备位图。窗口尺寸不变时，悬停重绘无需反复申请整窗像素内存。
HDC g_backBufferDc{};
HBITMAP g_backBufferBitmap{};
HGDIOBJ g_backBufferPrevious{};
SIZE g_backBufferSize{};
UINT g_dpi = 96;
unsigned long g_generation{};
bool g_minimizeAfterApply{};
bool g_applying{};
bool g_trayIconVisible{};
// 主窗口隐藏到托盘后，拥有者窗口上的 WS_POPUP 不一定会自动隐藏。
// 单独记录该状态，布局函数也据此禁止重新显示取景框，避免定时器/布局消息造成残留。
bool g_inTray{};
bool g_forceExit{};
bool g_previewActive{};
bool g_previewLoading{};
uint64_t g_previewRequest{};
bool g_sharedPreviewActive{};
bool g_settingsOpen{};
bool g_cropDragging{};
UINT g_taskbarCreated{};
size_t g_selectedVideo{};
size_t g_galleryPage{};
int g_hoverThumbnail{-1};
std::thread g_thumbnailThread;
std::atomic_bool g_stopThumbnails{false};
// 切换预览的短暂窗口内，暂缓后台封面解码，避免它和当前视频争用磁盘、CPU 与硬件解码器。
std::atomic_bool g_pauseThumbnailWork{false};
std::atomic_uint g_thumbnailGeneration{};
std::mutex g_thumbnailTaskMutex;
std::condition_variable g_thumbnailTaskReady;
std::deque<ThumbnailTask> g_thumbnailTasks;
std::set<size_t> g_residentThumbnails, g_wantedThumbnails;
void PauseThumbnailWork(bool pause) {
    { std::lock_guard lock(g_thumbnailTaskMutex); g_pauseThumbnailWork = pause; }
    g_thumbnailTaskReady.notify_all();
}
ProxyJob g_proxyJob;
unsigned g_proxyGeneration{};
// 仅改变控制窗口的底层背景；视频、卡片和设置抽屉仍使用各自的颜色。
COLORREF g_backgroundColor = kBackground;
SIZE_T g_totalWorkingSet{};
DWORD g_systemMemoryLoad{};
ULONGLONG g_systemMemoryUsed{};
ULONGLONG g_systemMemoryTotal{};
ULONG_PTR g_gdiplusToken{};
UsageTracker g_usageTracker;
AsyncFileStore g_configStore;
ProcessCleanup g_processCleanup;
std::wstring g_lastConfigSnapshot;
bool g_memorySampleValid{true};
std::map<std::wstring,LibraryPreference> g_preferences;
std::vector<std::wstring> g_externalFiles;
int g_libraryFilter{};
bool g_accentFromCover{},g_compareOriginal{},g_splitDragging{};
double g_previewRatio{0.60};
PlayerInstance g_effectPreview;
HWND g_effectPreviewWindow{},g_experienceWindow{},g_seekBar{};
int g_experienceTab{},g_playlistGroup{},g_playlistMinutes{15};
bool g_playlistEnabled{},g_playlistRandom{},g_playlistSchedule{},g_playlistBusy{};
ULONGLONG g_nextPlaylist{},g_effectPreviewDeadline{};
unsigned g_lastPlaylistGroup{};
bool g_seekDragging{},g_previewOnlyPaused{},g_experienceLoading{};
int g_pendingSeek{-1};
int g_ballSize{116};bool g_ballAutoHide{},g_ballCollapsed{};RECT g_ballExpanded{};
bool g_sessionSuspended{};
void StopEffectPreview();
bool StartEffectPreview(HWND owner);
void UpdateExperience(HWND owner);
void ShowExperience(HWND owner,int tab);
bool ExperienceCommand(HWND owner,int id);
void TickExperience(HWND owner);
void UpdateCoverAccent(HWND owner);
void RefreshExperienceTheme();
void LoadExperience();
std::wstring SaveExperience();
void AcceptWallpaperFiles(HWND owner,const std::vector<std::wstring>& paths);
RECT SplitterRect(HWND owner);
SIZE CurrentVideoSize();
WallpaperEffects CurrentEffects(){
    if(g_compareOriginal || g_videoPath.empty())return {};
    uint64_t hash=1469598103934665603ULL;for(wchar_t c:g_videoPath){hash^=towlower(c);hash*=1099511628211ULL;}
    wchar_t key[24]{};swprintf_s(key,L"%016llX",static_cast<unsigned long long>(hash));
    const auto found=g_preferences.find(key);return found==g_preferences.end()?WallpaperEffects{}:found->second.effects;
}
// 两种统计数据会始终同时累计；这个配置只决定排行榜当前展示哪一种口径。
UsageMode g_usageMode = UsageMode::Active;

double CurrentFocusX();
double CurrentFocusY();
void LayoutControls(HWND hwnd);
void ScheduleCurrentPreview(HWND owner, bool immediate = false);
bool ActivateSharedPreview(HWND owner, const std::wstring& textureName);
bool RestoreAppliedSharedPreview(HWND owner);
bool IsAppliedVideo(const std::wstring& path);
LRESULT CALLBACK MemoryBallProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK UsageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

int Scale(int value) {
    // 所有布局常量按 96 DPI 设计，Per-Monitor V2 下随窗口所在显示器动态缩放。
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

void DeleteFonts() {
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_subtitleFont) DeleteObject(g_subtitleFont);
    if (g_bodyFont) DeleteObject(g_bodyFont);
    if (g_smallFont) DeleteObject(g_smallFont);
    if (g_buttonFont) DeleteObject(g_buttonFont);
    g_titleFont = g_subtitleFont = g_bodyFont = g_smallFont = g_buttonFont = nullptr;
}

void DeleteBackBuffer() {
    if (g_backBufferDc && g_backBufferPrevious) {
        SelectObject(g_backBufferDc, g_backBufferPrevious);
    }
    if (g_backBufferBitmap) DeleteObject(g_backBufferBitmap);
    if (g_backBufferDc) DeleteDC(g_backBufferDc);
    g_backBufferDc = nullptr;
    g_backBufferBitmap = nullptr;
    g_backBufferPrevious = nullptr;
    g_backBufferSize = {};
}

bool EnsureBackBuffer(HDC reference, int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (g_backBufferDc && g_backBufferBitmap &&
        g_backBufferSize.cx == width && g_backBufferSize.cy == height) {
        return true;
    }

    DeleteBackBuffer();
    g_backBufferDc = CreateCompatibleDC(reference);
    g_backBufferBitmap = CreateCompatibleBitmap(reference, width, height);
    if (!g_backBufferDc || !g_backBufferBitmap) {
        DeleteBackBuffer();
        return false;
    }
    g_backBufferPrevious = SelectObject(g_backBufferDc, g_backBufferBitmap);
    g_backBufferSize = SIZE{width, height};
    return true;
}

HFONT MakeFont(int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                       L"Microsoft YaHei UI");
}

void CreateFonts() {
    DeleteFonts();
    g_titleFont = MakeFont(21, FW_BOLD);
    g_subtitleFont = MakeFont(10, FW_NORMAL);
    g_bodyFont = MakeFont(11, FW_NORMAL);
    g_smallFont = MakeFont(9, FW_NORMAL);
    g_buttonFont = MakeFont(11, FW_SEMIBOLD);
    if (g_settingsButton) SendMessageW(g_settingsButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    if (g_applyButton) SendMessageW(g_applyButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    if (g_usageButton) SendMessageW(g_usageButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    if (g_activeModeButton) SendMessageW(g_activeModeButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    if (g_viewingModeButton) SendMessageW(g_viewingModeButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
    for(HWND button:{g_chooseFolderButton,g_chooseCacheButton,g_chooseBackgroundButton,g_refreshButton})
        if(button) SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(g_buttonFont),TRUE);
    for(const auto& [id,button]:g_extraButtons)
        SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(g_buttonFont),TRUE);
    if(g_searchEdit) SendMessageW(g_searchEdit,WM_SETFONT,reinterpret_cast<WPARAM>(g_bodyFont),TRUE);
    if(g_sortCombo) SendMessageW(g_sortCombo,WM_SETFONT,reinterpret_cast<WPARAM>(g_smallFont),TRUE);
    if(g_experienceWindow)EnumChildWindows(g_experienceWindow,[](HWND child,LPARAM font)->BOOL {
        SendMessageW(child,WM_SETFONT,static_cast<WPARAM>(font),TRUE);return TRUE;
    },reinterpret_cast<LPARAM>(g_smallFont));
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    if (!RectVisible(dc, &rect)) return;
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    if (!RectVisible(dc, &rect)) return;
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawLabel(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
               UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    if (text.empty() || !RectVisible(dc, &rect)) return;
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, oldFont);
}

// Line MD cog 的 SVG 使用 24x24 坐标与三次贝塞尔曲线。这里直接绘制它的
// 最终静态轮廓，既保持 SVG 的圆角线条，也不用为一个小图标引入 SVG 运行库。
struct SvgCubic {
    double control1X;
    double control1Y;
    double control2X;
    double control2Y;
    double endX;
    double endY;
};

void AddRelativeSvgPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& bounds,
                        double startX, double startY,
                        std::initializer_list<SvgCubic> curves) {
    auto point = [&](double x, double y) {
        return Gdiplus::PointF{
            bounds.X + static_cast<Gdiplus::REAL>(x * bounds.Width / 24.0),
            bounds.Y + static_cast<Gdiplus::REAL>(y * bounds.Height / 24.0),
        };
    };

    path.StartFigure();
    double currentX = startX;
    double currentY = startY;
    for (const SvgCubic& curve : curves) {
        const Gdiplus::PointF start = point(currentX, currentY);
        const Gdiplus::PointF control1 = point(currentX + curve.control1X,
                                               currentY + curve.control1Y);
        const Gdiplus::PointF control2 = point(currentX + curve.control2X,
                                               currentY + curve.control2Y);
        const Gdiplus::PointF end = point(currentX + curve.endX,
                                         currentY + curve.endY);
        path.AddBezier(start, control1, control2, end);
        currentX += curve.endX;
        currentY += curve.endY;
    }
}

void DrawCogIcon(HDC dc, const RECT& buttonRect, COLORREF color) {
    const int buttonWidth = buttonRect.right - buttonRect.left;
    const int buttonHeight = buttonRect.bottom - buttonRect.top;
    const int iconSize = (std::min)(buttonWidth, buttonHeight) - Scale(16);
    if (!g_gdiplusToken) {
        DrawLabel(dc, L"⚙", buttonRect, g_titleFont, color,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        return;
    }

    const Gdiplus::RectF icon{
        static_cast<Gdiplus::REAL>(buttonRect.left + (buttonWidth - iconSize) / 2.0),
        static_cast<Gdiplus::REAL>(buttonRect.top + (buttonHeight - iconSize) / 2.0),
        static_cast<Gdiplus::REAL>(iconSize),
        static_cast<Gdiplus::REAL>(iconSize),
    };
    Gdiplus::GraphicsPath path;
    // 中心圆对应 SVG 中半径为 3 的第一条路径。
    path.AddEllipse(icon.X + icon.Width * 9.0f / 24.0f,
                    icon.Y + icon.Height * 9.0f / 24.0f,
                    icon.Width * 6.0f / 24.0f,
                    icon.Height * 6.0f / 24.0f);

    AddRelativeSvgPath(path, icon, 15.24, 6.37, {
        {0.41, 0.23, 0.80, 0.51, 1.14, 0.83}, {0, 0, 2.62, -1.08, 2.63, -1.06},
        {0, 0, 1.56, 2.70, 1.56, 2.70}, {0.01, 0.03, -2.22, 1.75, -2.22, 1.75},
        {0.10, 0.45, 0.15, 0.93, 0.15, 1.41},
    });
    AddRelativeSvgPath(path, icon, 18.50, 11.99, {
        {0.01, 0.47, -0.04, 0.95, -0.15, 1.40}, {0, 0, 2.25, 1.73, 2.23, 1.75},
        {0, 0, -1.56, 2.70, -1.56, 2.70}, {-0.02, 0.02, -2.63, -1.05, -2.63, -1.05},
        {-0.34, 0.31, -0.73, 0.59, -1.15, 0.83},
    });
    AddRelativeSvgPath(path, icon, 15.26, 17.62, {
        {-0.40, 0.24, -0.84, 0.44, -1.29, 0.57}, {0, 0, -0.37, 2.81, -0.40, 2.81},
        {0, 0, -3.12, 0, -3.12, 0}, {-0.03, -0.01, -0.41, -2.80, -0.41, -2.80},
        {-0.44, -0.14, -0.88, -0.34, -1.30, -0.58},
    });
    AddRelativeSvgPath(path, icon, 8.76, 17.63, {
        {-0.41, -0.23, -0.80, -0.51, -1.14, -0.83}, {0, 0, -2.62, 1.08, -2.63, 1.06},
        {0, 0, -1.56, -2.70, -1.56, -2.70}, {-0.01, -0.03, 2.22, -1.75, 2.22, -1.75},
        {-0.10, -0.45, -0.15, -0.93, -0.15, -1.41},
    });
    AddRelativeSvgPath(path, icon, 5.50, 12.01, {
        {-0.01, -0.47, 0.04, -0.95, 0.15, -1.40}, {0, 0, -2.25, -1.73, -2.23, -1.75},
        {0, 0, 1.56, -2.70, 1.56, -2.70}, {0.02, -0.02, 2.63, 1.05, 2.63, 1.05},
        {0.34, -0.31, 0.73, -0.59, 1.15, -0.83},
    });
    AddRelativeSvgPath(path, icon, 8.74, 6.38, {
        {0.40, -0.24, 0.84, -0.44, 1.29, -0.57}, {0, 0, 0.37, -2.81, 0.40, -2.81},
        {0, 0, 3.12, 0, 3.12, 0}, {0.03, 0.01, 0.41, 2.80, 0.41, 2.80},
        {0.44, 0.14, 0.88, 0.34, 1.30, 0.58},
    });

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)),
                     static_cast<Gdiplus::REAL>(iconSize) * 2.0f / 24.0f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'\"') result += L'\\';
        result += ch;
    }
    result += L"\"";
    return result;
}

std::wstring ExeDirectory() {
    std::vector<wchar_t> buffer(32768);
    GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    PathRemoveFileSpecW(buffer.data());
    return buffer.data();
}

void RestoreStaticWallpaper() {
    // 动态壁纸实际位于 WorkerW；退出时重新设置一次 Windows 当前静态壁纸，
    // 目的是让 Explorer 刷新桌面层，而不是维护另一份壁纸路径。
    wchar_t staticWallpaper[32768]{};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(std::size(staticWallpaper)),
                              staticWallpaper, 0) && staticWallpaper[0]) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, staticWallpaper, SPIF_SENDCHANGE);
    } else {
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0,
                            SMTO_ABORTIFHUNG, 1000, nullptr);
    }
}

HICON LoadAppIcon(int width, int height) {
    return reinterpret_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
}

void AddTrayIcon(HWND hwnd) {
    // 托盘图标在控制窗口显示期间也始终存在；重复收到 Explorer 重建或布局消息时
    // 不再重复 NIM_ADD，避免通知区出现残留/重复图标。
    if (g_trayIconVisible) return;
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = hwnd;
    icon.uID = kTrayIconId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!icon.hIcon) icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(icon.szTip, L"DWD · 动态壁纸");
    if (Shell_NotifyIconW(NIM_ADD, &icon)) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
        g_trayIconVisible = true;
    }
}

void RemoveTrayIcon(HWND hwnd) {
    if (!g_trayIconVisible) return;
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = hwnd;
    icon.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    g_trayIconVisible = false;
}

SIZE_T CurrentDwdWorkingSet() {
    // 与任务管理器的私有工作集对齐；包括尚在优雅退出的旧播放器，避免切换时漏算。
    SIZE_T total{};
    g_memorySampleValid = true;
    for (HANDLE process : {GetCurrentProcess(), g_player.process, g_proxyJob.process,g_effectPreview.process}) {
        SIZE_T bytes{};
        g_memorySampleValid = PrivateResidentBytes(process, bytes) && g_memorySampleValid;
        total += bytes;
    }
    g_memorySampleValid = g_processCleanup.AddPrivateBytes(total) && g_memorySampleValid;
    return total;
}

std::wstring FormatMemoryValue(SIZE_T bytes) {
    const double megabytes = static_cast<double>(bytes) / (1024.0 * 1024.0);
    wchar_t text[32]{};
    if (megabytes >= 1000.0) {
        swprintf_s(text, L"%.1f GB", megabytes / 1024.0);
    } else {
        swprintf_s(text, L"%.0f MB", megabytes);
    }
    return text;
}

std::wstring FormatSystemMemoryValue(ULONGLONG used, ULONGLONG total) {
    constexpr double gibibyte = 1024.0 * 1024.0 * 1024.0;
    wchar_t text[48]{};
    swprintf_s(text, L"%.1f / %.1f GB",
               static_cast<double>(used) / gibibyte,
               static_cast<double>(total) / gibibyte);
    return text;
}

int ScaleForWindow(HWND hwnd, int value) {
    return MulDiv(value, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

void DrawMemoryBallText(Gdiplus::Graphics& graphics, const std::wstring& text,
                        const Gdiplus::RectF& bounds, Gdiplus::REAL pixels,
                        INT style, const Gdiplus::Color& color) {
    Gdiplus::Font font(L"Microsoft YaHei UI", pixels, style, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(color);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), &font, bounds,
                        &format, &brush);
}

void RenderMemoryBall(HWND hwnd) {
    if (!hwnd || !g_gdiplusToken) return;
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    // SetWindowRgn 只能按整像素裁切，圆周必然呈台阶。本窗口改用 32 位 PARGB 位图和
    // UpdateLayeredWindow：最外圈像素可以拥有连续透明度，交给 DWM 合成后才真正平滑。
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* pixels{};
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!memory || !dib || !pixels) {
        if (dib) DeleteObject(dib);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ previous = SelectObject(memory, dib);
    Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB,
                            static_cast<BYTE*>(pixels));
    Gdiplus::Graphics graphics(&surface);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const Gdiplus::REAL dpiScale = static_cast<Gdiplus::REAL>(width) / 116.0f;
    const Gdiplus::REAL inset = 2.5f * dpiScale;
    const Gdiplus::RectF circle(inset, inset, width - inset * 2.0f, height - inset * 2.0f);
    Gdiplus::SolidBrush fill(Gdiplus::Color(238, 27, 36, 50));
    Gdiplus::Pen border(Gdiplus::Color(255, 111, 124, 255), 2.0f * dpiScale);
    graphics.FillEllipse(&fill, circle);
    graphics.DrawEllipse(&border, circle);

    const auto row = [&](float top, float bottom) {
        return Gdiplus::RectF(5.0f * dpiScale, top * dpiScale,
                              width - 10.0f * dpiScale, (bottom - top) * dpiScale);
    };
    if(g_ballCollapsed){
        DrawMemoryBallText(graphics,L"D",Gdiplus::RectF(0,0,float(width),float(height)),float(width)*.5f,
            Gdiplus::FontStyleBold,Gdiplus::Color(255,242,245,250));
    }else {
    DrawMemoryBallText(graphics, L"DWD", row(7.0f, 26.0f), 15.0f * dpiScale,
                       Gdiplus::FontStyleRegular, Gdiplus::Color(255, 242, 245, 250));
    DrawMemoryBallText(graphics, g_memorySampleValid ? FormatMemoryValue(g_totalWorkingSet) : L"暂不可用", row(24.0f, 47.0f),
                       15.0f * dpiScale, Gdiplus::FontStyleRegular,
                       Gdiplus::Color(255, 72, 211, 160));
    wchar_t systemLabel[32]{};
    swprintf_s(systemLabel, L"系统 %lu%%", g_systemMemoryLoad);
    DrawMemoryBallText(graphics, systemLabel, row(46.0f, 69.0f), 15.0f * dpiScale,
                       Gdiplus::FontStyleRegular, Gdiplus::Color(255, 242, 245, 250));
    DrawMemoryBallText(graphics,
                       FormatSystemMemoryValue(g_systemMemoryUsed, g_systemMemoryTotal),
                       row(68.0f, 88.0f), 11.0f * dpiScale, Gdiplus::FontStyleRegular,
                       Gdiplus::Color(255, 153, 164, 182));
    }
    graphics.Flush(Gdiplus::FlushIntentionSync);

    RECT window{};
    GetWindowRect(hwnd, &window);
    POINT destination{window.left, window.top};
    POINT source{};
    SIZE size{width, height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &destination, &size, memory, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memory, previous);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void RefreshMemoryBall(HWND hwnd) {
    const bool wasValid = g_memorySampleValid;
    const SIZE_T memory = CurrentDwdWorkingSet();
    const SIZE_T oldMegabytes = (g_totalWorkingSet + 512 * 1024) / (1024 * 1024);
    const SIZE_T newMegabytes = (memory + 512 * 1024) / (1024 * 1024);

    const DWORD oldSystemLoad = g_systemMemoryLoad;
    const ULONGLONG oldUsedTenths = (g_systemMemoryUsed * 10 + (1ULL << 29)) >> 30;
    MEMORYSTATUSEX status{sizeof(status)};
    if (GlobalMemoryStatusEx(&status)) {
        g_systemMemoryLoad = status.dwMemoryLoad;
        g_systemMemoryTotal = status.ullTotalPhys;
        g_systemMemoryUsed = status.ullTotalPhys - status.ullAvailPhys;
    }
    const ULONGLONG newUsedTenths = (g_systemMemoryUsed * 10 + (1ULL << 29)) >> 30;
    // 显示路径查询只在悬浮球的一秒低频计时器中进行，不扫描设备树、不访问驱动。
    g_totalWorkingSet = memory;
    if (wasValid != g_memorySampleValid || oldMegabytes != newMegabytes || oldSystemLoad != g_systemMemoryLoad ||
        oldUsedTenths != newUsedTenths) {
        RenderMemoryBall(hwnd);
    }
}

void KeepMemoryBallTopmost(HWND hwnd, bool show = true) {
    if (!g_showMemoryBall) return;
    if (!hwnd) return;
    // 悬浮球是独立工具窗口，不依赖主窗口的拥有者层级。每次显示或主窗口状态改变后
    // 都显式重新放入 TOPMOST 层，保证普通浏览器、资源管理器等窗口不会盖住它。
    UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (show) flags |= SWP_SHOWWINDOW;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
}

LRESULT CALLBACK MemoryBallProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_ballTooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
            0,0,0,0,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(g_ballTooltip){TOOLINFOW tool{sizeof(tool)};tool.hwnd=hwnd;tool.uId=1;tool.uFlags=TTF_TRACK|TTF_ABSOLUTE;
            tool.lpszText=const_cast<wchar_t*>(L"DWD：全部进程的私有物理内存，不含 GPU 专用显存。\n每秒采样，和任务管理器会有时间差。右键可调整大小或隐藏。");
            SendMessageW(g_ballTooltip,TTM_SETMAXTIPWIDTH,0,ScaleForWindow(hwnd,300));
            SendMessageW(g_ballTooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));}
        SetTimer(hwnd, kMemoryTimer, 1000, nullptr);
        RefreshMemoryBall(hwnd);
        RenderMemoryBall(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == kMemoryTimer) {
            RefreshMemoryBall(hwnd);
            if(g_ballAutoHide&&!g_ballCollapsed&&GetCapture()!=hwnd){
                RECT r{};GetWindowRect(hwnd,&r);POINT p{};GetCursorPos(&p);
                MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&mi);
                if(!PtInRect(&r,p)&&(abs(r.left-mi.rcWork.left)<16||abs(r.right-mi.rcWork.right)<16)){
                    g_ballExpanded=r;g_ballCollapsed=true;const int size=ScaleForWindow(hwnd,30);
                    SetWindowPos(hwnd,nullptr,r.left-mi.rcWork.left<16?mi.rcWork.left:mi.rcWork.right-size,r.top,size,size,SWP_NOZORDER|SWP_NOACTIVATE);
                    RenderMemoryBall(hwnd);
                }
            }
            // 某些 Shell/显示器切换会重建窗口层级；只在 TOPMOST 标记丢失时恢复，
            // 不持续抢占其他应用有意创建的置顶窗口。
            if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
                KeepMemoryBallTopmost(hwnd, false);
            }
        }
        return 0;
    case WM_NCMOUSEMOVE:
        {TRACKMOUSEEVENT tracking{sizeof(tracking),TME_NONCLIENT|TME_HOVER|TME_LEAVE,hwnd,800};TrackMouseEvent(&tracking);}
        if(g_ballCollapsed){g_ballCollapsed=false;SetWindowPos(hwnd,nullptr,g_ballExpanded.left,g_ballExpanded.top,
            g_ballExpanded.right-g_ballExpanded.left,g_ballExpanded.bottom-g_ballExpanded.top,SWP_NOZORDER|SWP_NOACTIVATE);RenderMemoryBall(hwnd);}
        return 0;
    case WM_NCMOUSEHOVER:case WM_NCMOUSELEAVE:
        if(g_ballTooltip){TOOLINFOW tool{sizeof(tool)};tool.hwnd=hwnd;tool.uId=1;
            if(message==WM_NCMOUSEHOVER){POINT p{};GetCursorPos(&p);SendMessageW(g_ballTooltip,TTM_TRACKPOSITION,0,MAKELPARAM(p.x+12,p.y+18));}
            SendMessageW(g_ballTooltip,TTM_TRACKACTIVATE,message==WM_NCMOUSEHOVER,reinterpret_cast<LPARAM>(&tool));}return 0;
    case WM_CONTEXTMENU:case WM_NCRBUTTONUP:{
        HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING|MF_DISABLED,1,L"DWD：全部进程的私有物理内存");
        AppendMenuW(menu,MF_STRING|MF_DISABLED,2,L"不含 GPU 专用显存；采样间隔 1 秒");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_STRING,10,L"小号");AppendMenuW(menu,MF_STRING,11,L"标准");AppendMenuW(menu,MF_STRING,12,L"大号");
        AppendMenuW(menu,MF_STRING,13,g_ballAutoHide?L"关闭贴边收起":L"开启贴边收起");AppendMenuW(menu,MF_STRING,14,L"隐藏悬浮球");
        POINT p{};GetCursorPos(&p);SetForegroundWindow(hwnd);const UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr);DestroyMenu(menu);
        if(choice>=10&&choice<=12){g_ballSize=choice==10?92:choice==11?116:148;g_ballCollapsed=false;
            const int size=ScaleForWindow(hwnd,g_ballSize);SetWindowPos(hwnd,nullptr,0,0,size,size,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);RenderMemoryBall(hwnd);}
        if(choice==13)g_ballAutoHide=!g_ballAutoHide;
        if(choice==14){g_showMemoryBall=false;KillTimer(hwnd,kMemoryTimer);ShowWindow(hwnd,SW_HIDE);}
        if(choice)SaveConfig();return 0;
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        RECT client{};
        GetClientRect(hwnd, &client);
        const double centerX = client.right / 2.0;
        const double centerY = client.bottom / 2.0;
        const double radius = (std::min)(client.right, client.bottom) / 2.0 -
                              ScaleForWindow(hwnd, 2);
        const double x = point.x - centerX;
        const double y = point.y - centerY;
        if (x * x + y * y > radius * radius) return HTTRANSPARENT;
        const LRESULT hit = DefWindowProcW(hwnd, message, wParam, lParam);
        // 把圆形客户区当作标题栏，用户可在悬浮球任意位置按住拖动。
        return hit == HTCLIENT ? HTCAPTION : hit;
    }
    case WM_NCLBUTTONDBLCLK:
        // 双击悬浮球与双击托盘图标一致：恢复并前置主窗口。
        if (g_mainWindow) PostMessageW(g_mainWindow, kRestoreMessage, 0, 0);
        return 0;
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE);
        RenderMemoryBall(hwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        RenderMemoryBall(hwnd);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, kMemoryTimer);
        if(g_ballTooltip){DestroyWindow(g_ballTooltip);g_ballTooltip=nullptr;}
        if (g_memoryBall == hwnd) g_memoryBall = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateMemoryBall(HINSTANCE instance) {
    const UINT dpi = GetDpiForSystem();
    const int diameter = MulDiv(g_ballSize, static_cast<int>(dpi), 96);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND ball = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                                    WS_EX_LAYERED,
                                kMemoryBallClass, L"DWD 内存",
                                WS_POPUP,
                                work.right - diameter - MulDiv(24, static_cast<int>(dpi), 96),
                                work.bottom - diameter - MulDiv(24, static_cast<int>(dpi), 96),
                                diameter, diameter, nullptr, nullptr, instance, nullptr);
    if (!ball) return nullptr;
    KeepMemoryBallTopmost(ball);
    RenderMemoryBall(ball);
    UpdateWindow(ball);
    return ball;
}

std::wstring FormatUsageDuration(std::uint64_t milliseconds) {
    const std::uint64_t totalMinutes = milliseconds / 60'000;
    if (!totalMinutes) return L"少于 1 分钟";
    const std::uint64_t hours = totalMinutes / 60;
    const std::uint64_t minutes = totalMinutes % 60;
    if (!hours) return std::to_wstring(minutes) + L" 分钟";
    if (!minutes) return std::to_wstring(hours) + L" 小时";
    return std::to_wstring(hours) + L" 小时 " + std::to_wstring(minutes) + L" 分钟";
}

HFONT MakeWindowFont(HWND hwnd, int points, int weight) {
    return CreateFontW(-MulDiv(points, static_cast<int>(GetDpiForWindow(hwnd)), 72),
                       0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Microsoft YaHei UI");
}

void PaintUsageWindow(HWND hwnd, HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto sx = [&](int value) { return ScaleForWindow(hwnd, value); };
    HBRUSH background = CreateSolidBrush(g_backgroundColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    HFONT headingFont = MakeWindowFont(hwnd, 19, FW_BOLD);
    HFONT bodyFont = MakeWindowFont(hwnd, 11, FW_NORMAL);
    HFONT smallFont = MakeWindowFont(hwnd, 9, FW_NORMAL);
    HFONT valueFont = MakeWindowFont(hwnd, 10, FW_SEMIBOLD);

    const bool activeMode = g_usageMode == UsageMode::Active;
    const auto recordMilliseconds = [activeMode](const UsageRecord& record) {
        return activeMode ? record.activeMilliseconds : record.viewingMilliseconds;
    };
    RECT heading{sx(24), sx(18), client.right - sx(24), sx(54)};
    DrawLabel(dc, activeMode ? L"活跃前台时长" : L"观看前台时长",
              heading, headingFont, kText);
    RECT date{sx(24), sx(52), client.right - sx(24), sx(78)};
    const std::wstring selectedDate=g_usageDate.empty()?g_usageTracker.TodayKey():g_usageDate;
    if(!g_usageDate.empty() && g_historyDate!=g_usageDate) {
        g_historyDate=g_usageDate;g_historyRecords=g_usageTracker.RecordsForDay(g_usageDate,g_usageMode);
    }
    const auto records=g_usageDate.empty()?g_usageTracker.TodayRecords(g_usageMode):g_historyRecords;
    uint64_t total{};for(const auto& record:records) total+=recordMilliseconds(record);
    DrawLabel(dc, selectedDate +
                  (activeMode ? L"  ·  活跃合计 " : L"  ·  前台合计 ") +
                  FormatUsageDuration(total),
              date, bodyFont, kSuccess);
    RECT rule{sx(24), sx(78), client.right - sx(24), sx(110)};
    DrawLabel(dc,
              activeMode
                  ? L"活跃模式 · 连续 60 秒没有键盘或鼠标输入后暂停累计"
                  : L"观看模式 · 前台应用持续累计，锁屏、休眠或退出 DWD 后暂停",
              rule, smallFont, kMuted);

    const std::uint64_t maximum = records.empty() ? 1 : recordMilliseconds(records.front());
    const int listTop = sx(164);
    const int rowHeight = sx(48);
    const int gap = sx(8);
    const int bottomReserve = sx(48);
    const int available = static_cast<int>(client.bottom) - bottomReserve - listTop;
    const int capacity = (std::max)(0, available / (rowHeight + gap));
    const size_t maxScroll=records.size()>static_cast<size_t>(capacity)?records.size()-capacity:0;
    g_usageScroll=(std::min)(g_usageScroll,maxScroll);
    SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE|SIF_POS};
    scroll.nMax=static_cast<int>(records.size())-1;scroll.nPage=capacity;scroll.nPos=static_cast<int>(g_usageScroll);
    SetScrollInfo(hwnd,SB_VERT,&scroll,TRUE);
    const int visible = (std::min)(capacity, static_cast<int>(records.size()-g_usageScroll));
    for (int index = 0; index < visible; ++index) {
        const auto& record=records[g_usageScroll+index];
        const int top = listTop + index * (rowHeight + gap);
        RECT card{sx(24), top, client.right - sx(24), top + rowHeight};
        FillRounded(dc, card, sx(10), kPanel);
        RECT name{card.left + sx(14), card.top + sx(4), card.right - sx(130),
                  card.top + sx(28)};
        DrawLabel(dc, record.displayName, name, bodyFont, kText,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT value{card.right - sx(126), card.top + sx(4), card.right - sx(14),
                   card.top + sx(28)};
        DrawLabel(dc, FormatUsageDuration(recordMilliseconds(record)), value,
                  valueFont, kText, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        RECT track{card.left + sx(14), card.bottom - sx(13), card.right - sx(14),
                   card.bottom - sx(8)};
        FillRounded(dc, track, sx(3), RGB(48, 59, 76));
        const double ratio = static_cast<double>(recordMilliseconds(record)) / maximum;
        RECT progress = track;
        progress.right = progress.left + static_cast<LONG>((track.right - track.left) * ratio);
        if (progress.right > progress.left) FillRounded(dc, progress, sx(3), kAccent);
    }
    if (records.empty()) {
        RECT empty{sx(24), listTop, client.right - sx(24), listTop + sx(120)};
        FillRounded(dc, empty, sx(12), kPanel);
        DrawLabel(dc, g_usageDate.empty()?L"当天还没有记录\nDWD 运行期间才会累计使用时长":L"该日期没有这个口径的记录",
                  empty, bodyFont, kMuted,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    }

    RECT privacy{sx(24), client.bottom - sx(38), client.right - sx(24),
                 client.bottom - sx(14)};
    DrawLabel(dc, L"数据仅保存在程序旁的 UsageData 文件夹，不记录操作内容。",
              privacy, smallFont, kMuted, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    DeleteObject(headingFont);
    DeleteObject(bodyFont);
    DeleteObject(smallFont);
    DeleteObject(valueFont);
}

void LayoutUsageControls(HWND hwnd) {
    const UINT dpi=GetDpiForWindow(hwnd);
    if(!g_usageControlsFont || dpi!=g_usageControlsDpi) {
        if(g_usageControlsFont) DeleteObject(g_usageControlsFont);
        g_usageControlsFont=MakeWindowFont(hwnd,9,FW_NORMAL);g_usageControlsDpi=dpi;
    }
    const auto sx=[&](int v){return ScaleForWindow(hwnd,v);};
    int x=24;
    for(const auto& [id,width] : std::initializer_list<std::pair<int,int>>{
        {kUsageOlder,82},{kUsageNewer,82},{kUsageToday,56},{kUsageActive,90},{kUsageViewing,90}}) {
        HWND button=GetDlgItem(hwnd,id);
        SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(g_usageControlsFont),TRUE);
        MoveWindow(button,sx(x),sx(116),sx(width),sx(32),TRUE);x+=width+6;
    }
}

LRESULT CALLBACK UsageWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch(message) {
    case WM_CREATE:
        for(const auto& [id,label] : std::initializer_list<std::pair<int,const wchar_t*>>{
            {kUsageOlder,L"上一记录日"},{kUsageNewer,L"下一记录日"},{kUsageToday,L"今天"},
            {kUsageActive,L"活跃模式"},{kUsageViewing,L"观看模式"}}) {
            HWND button=CreateWindowW(L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
            SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(g_smallFont),TRUE);
        }
        LayoutUsageControls(hwnd);return 0;
    case WM_SIZE:
        LayoutUsageControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_DPICHANGED: {
        const auto* r=reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
        LayoutUsageControls(hwnd);InvalidateRect(hwnd,nullptr,TRUE);return 0;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wParam);
        if(id==kUsageToday) g_usageDate.clear();
        else if(id==kUsageActive || id==kUsageViewing) {
            g_usageMode=id==kUsageActive?UsageMode::Active:UsageMode::Viewing;
            g_historyDate.clear();SaveConfig();
            RedrawWindow(g_mainWindow,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        } else if(id==kUsageOlder || id==kUsageNewer) {
            const auto days=g_usageTracker.AvailableDays();
            const auto date=g_usageDate.empty()?g_usageTracker.TodayKey():g_usageDate;
            const auto found=std::find(days.begin(),days.end(),date);
            if(found!=days.end()) {
                auto n=static_cast<size_t>(found-days.begin());
                if(id==kUsageOlder && n+1<days.size()) g_usageDate=days[n+1];
                else if(id==kUsageNewer && n) g_usageDate=days[n-1]==g_usageTracker.TodayKey()?L"":days[n-1];
            }
        }
        g_usageScroll=0;InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    case WM_MOUSEWHEEL:
        if(GET_WHEEL_DELTA_WPARAM(wParam)<0) g_usageScroll+=3;
        else g_usageScroll=g_usageScroll>3?g_usageScroll-3:0;
        InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&info);
        int position=info.nPos;
        switch(LOWORD(wParam)) {
            case SB_LINEUP:--position;break;case SB_LINEDOWN:++position;break;
            case SB_PAGEUP:position-=info.nPage;break;case SB_PAGEDOWN:position+=info.nPage;break;
            case SB_THUMBTRACK:position=info.nTrackPos;break;
        }
        g_usageScroll=static_cast<size_t>((std::max)(0,position));InvalidateRect(hwnd,nullptr,FALSE);return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};HDC dc=BeginPaint(hwnd,&paint);PaintUsageWindow(hwnd,dc);EndPaint(hwnd,&paint);return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_CLOSE:DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        if(g_usageControlsFont) DeleteObject(g_usageControlsFont);
        g_usageControlsFont=nullptr;g_usageControlsDpi=0;
        if(g_usageWindow==hwnd) g_usageWindow=nullptr;return 0;
    }
    return DefWindowProcW(hwnd,message,wParam,lParam);
}

void ShowUsageWindow() {
    g_usageTracker.Tick();
    if (g_usageWindow) {
        ShowWindow(g_usageWindow, SW_RESTORE);
        SetForegroundWindow(g_usageWindow);
        InvalidateRect(g_usageWindow, nullptr, FALSE);
        return;
    }
    const UINT dpi = g_mainWindow ? GetDpiForWindow(g_mainWindow) : GetDpiForSystem();
    RECT windowRect{0, 0, MulDiv(520, dpi, 96), MulDiv(610, dpi, 96)};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL;
    AdjustWindowRectExForDpi(&windowRect, style, FALSE, 0, dpi);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    g_usageWindow = CreateWindowExW(0, kUsageWindowClass,
                                    L"DWD · 使用时长", style,
                                    info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2,
                                    info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2,
                                    width, height, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    if (!g_usageWindow) return;
    ApplyTheme(g_mainWindow);
    ShowWindow(g_usageWindow, SW_SHOW);
    UpdateWindow(g_usageWindow);
}

void StopSharedPreview(HWND owner, bool release = true) {
    ShowWindow(g_sharedPreviewWindow, SW_HIDE);
    g_sharedPreviewActive = false;
    KillTimer(owner, kSharedIdleTimer);
    if (release) { if (g_sharedPreview) g_sharedPreview->Stop(); }
    else SetTimer(owner, kSharedIdleTimer, 10000, nullptr);
}

bool ActivateSharedPreview(HWND owner, const std::wstring& textureName) {
    StopEffectPreview();
    KillTimer(owner, kPreviewTimeoutTimer);
    KillTimer(owner, kSharedIdleTimer);
    ShowWindow(g_previewWindow, SW_HIDE);
    if(g_sharedPreview)g_sharedPreview->SetEffects(CurrentEffects(),CurrentVideoSize());
    if (!g_sharedPreview || textureName.empty() || !g_sharedPreviewWindow ||
        !g_sharedPreview->Open(textureName, g_sharedPreviewWindow)) {
        StopSharedPreview(owner);
        return false;
    }
    g_sharedPreviewActive = true;
    g_previewActive = true;
    g_previewLoading = false;
    g_previewRequest = 0;
    const bool visible = !g_settingsOpen && !g_inTray;
    ShowWindow(g_sharedPreviewWindow, visible ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_cropOverlay, visible && g_cropEditing ? SW_SHOWNA : SW_HIDE);
    LayoutControls(owner);
    return true;
}

void RestoreFromTray(HWND hwnd) {
    g_inTray = false;
    // 托盘图标是程序的常驻入口，恢复窗口时不能删除。
    ShowWindow(hwnd, SW_RESTORE);
    KeepMemoryBallTopmost(g_memoryBall);
    SetForegroundWindow(hwnd);
    if (IsAppliedVideo(g_videoPath) && !g_player.previewTextureName.empty() &&
        ActivateSharedPreview(hwnd, g_player.previewTextureName)) {
        return;
    }
    if (!g_videoPath.empty()) ScheduleCurrentPreview(hwnd, true);
}

void MinimizeToTray(HWND hwnd) {
    // 壁纸由独立进程继续运行；这里只释放前台预览解码器，从而降低常驻内存。
    // 取景框是独立的 WS_POPUP 覆盖窗口，必须显式隐藏；仅隐藏主窗口并不可靠。
    g_inTray = true;
    if(g_experienceWindow)ShowWindow(g_experienceWindow,SW_HIDE);
    StopEffectPreview();
    // 先完成用户可见的收起，再释放资源。驱动释放即使较慢也不让窗口“关不掉”。
    ShowWindow(g_cropOverlay, SW_HIDE);
    ShowWindow(g_previewWindow, SW_HIDE);
    ShowWindow(g_sharedPreviewWindow, SW_HIDE);
    ShowWindow(hwnd, SW_HIDE);
    RuntimeLog(L"controller: window hidden to tray");
    KillTimer(hwnd, kOverlayTimer);
    KillTimer(hwnd, kPreviewSwitchTimer);
    KillTimer(hwnd, kPreviewTimeoutTimer);
    StopSharedPreview(hwnd, false);
    PauseThumbnailWork(false);
    if (g_cropDragging && GetCapture() == g_cropOverlay) ReleaseCapture();
    g_cropDragging = false;
    ShowWindow(g_cropOverlay, SW_HIDE);
    if (g_previewPlayer) g_previewPlayer->Stop();
    g_previewActive = false;
    g_previewLoading = false;
    g_previewRequest = 0;
    ShowWindow(g_previewWindow, SW_HIDE);
    AddTrayIcon(hwnd);
    ShowWindow(hwnd, SW_HIDE);
    // 悬浮球是独立工具窗口，主界面隐藏后仍保持在普通窗口上方。
    KeepMemoryBallTopmost(g_memoryBall);
}

void ShowTrayMenu(HWND hwnd) {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, kTrayOpen, L"打开 DWD");
    AppendMenuW(menu, MF_STRING, kTrayUsage, L"使用时长");
    AppendMenuW(menu, MF_STRING | (g_player.process ? 0 : MF_GRAYED),
                kPauseButton, g_wallpaperPaused ? L"继续播放壁纸" : L"暂停壁纸");
    AppendMenuW(menu, MF_STRING | (g_player.process ? 0 : MF_GRAYED),
                kStopButton, L"恢复静态壁纸");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, L"退出程序");
    SetForegroundWindow(hwnd);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                        cursor.x, cursor.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (command == kTrayOpen) RestoreFromTray(hwnd);
    else if (command == kTrayUsage) ShowUsageWindow();
    else if (command == kPauseButton) ToggleWallpaperPause(hwnd);
    else if (command == kStopButton) StopWallpaper(hwnd);
    else if (command == kTrayExit) {
        g_forceExit = true;
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void CloseHandles(PlayerInstance& player) {
    if (player.process) CloseHandle(player.process);
    if (player.stopEvent) CloseHandle(player.stopEvent);
    if (player.readyEvent) CloseHandle(player.readyEvent);
    if (player.pauseEvent) CloseHandle(player.pauseEvent);
    player = {};
}

void StopPlayer(PlayerInstance& player, bool restoreWallpaper) {
    g_processCleanup.Retire(player.process, player.stopEvent, player.readyEvent, player.pauseEvent);
    player = {};
    if (restoreWallpaper) RestoreStaticWallpaper();
}

void SetStatus(HWND owner, const std::wstring& text, COLORREF color) {
    if (g_statusText == text && g_statusColor == color) return;
    g_statusText = text;
    g_statusColor = color;
    RECT status{}; GetClientRect(owner, &status);
    status.top = status.bottom - Scale(68);
    InvalidateRect(owner, &status, FALSE);
}

std::wstring ConfigPath() {
    return ExeDirectory() + L"\\DWDNative.ini";
}

std::wstring DefaultCacheFolder() {
    return ExeDirectory() + L"\\Cache";
}

std::wstring ColorToConfig(COLORREF color) {
    wchar_t value[8]{};
    swprintf_s(value, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return value;
}

bool ParseConfigColor(const std::wstring& value, COLORREF& color) {
    if (value.size() != 7 || value[0] != L'#') return false;
    for (size_t index = 1; index < value.size(); ++index) {
        if (!iswxdigit(value[index])) return false;
    }
    const unsigned rgb = wcstoul(value.c_str() + 1, nullptr, 16);
    color = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return true;
}

bool BackgroundIsLight() {
    // 简单的感知亮度计算，用来保证标题在浅色和深色背景上都有足够对比度。
    return GetRValue(g_backgroundColor) * 299 + GetGValue(g_backgroundColor) * 587 +
           GetBValue(g_backgroundColor) * 114 >= 150000;
}

COLORREF BackgroundTextColor() {
    return BackgroundIsLight() ? RGB(24, 27, 35) : kText;
}

COLORREF BackgroundMutedColor() {
    return BackgroundIsLight() ? RGB(75, 80, 92) : kMuted;
}

bool SaveConfig() {
    std::wstring content = L"[Library]\r\nWallpaperFolder=" + g_wallpaperFolder +
        L"\r\nLastWallpaper=" + g_lastWallpaper +
        L"\r\nAutoRestore=" + (g_autoRestore ? L"1" : L"0") +
        L"\r\nSort=" + std::to_wstring(g_sortOrder) +
        L"\r\n\r\n[Cache]\r\nDirectory=" + (_wcsicmp(g_cacheFolder.c_str(),DefaultCacheFolder().c_str())==0?std::wstring(L"Cache"):g_cacheFolder) +
        L"\r\nLimitMB=" + std::to_wstring(g_cacheLimitBytes / (1024 * 1024)) +
        L"\r\n\r\n[Appearance]\r\nBackgroundColor=" + ColorToConfig(g_customBackground) +
        L"\r\nTheme=" + std::to_wstring(g_theme) +
        L"\r\nMemoryBall=" + (g_showMemoryBall ? L"1" : L"0") +
        L"\r\n\r\n[Playback]\r\nQuality=" + std::to_wstring(g_quality) +
        L"\r\nFullscreenRule=" + std::to_wstring(g_fullscreenRule) +
        L"\r\nTargetMonitor=" + g_targetMonitor + L"\r\nCropMonitor=" + g_cropMonitor +
        L"\r\n\r\n[Usage]\r\nMode=" +
        (g_usageMode == UsageMode::Viewing ? std::wstring(L"Viewing") : std::wstring(L"Active")) +
        L"\r\nEnabled=" + (g_usageEnabled ? L"1" : L"0") + L"\r\n\r\n[Focus]\r\n";
    for (const auto& [key, focus] : g_savedFocus) {
        content += key + L"=" + std::to_wstring(focus.first) + L"," +
                   std::to_wstring(focus.second) + L"\r\n";
    }
    content+=SaveExperience();
    if (content == g_lastConfigSnapshot) return true;
    g_lastConfigSnapshot = content;
    g_configStore.Submit(ConfigPath(), std::move(content));
    // 返回“已排队”；真正的写入失败由低频定时器反馈，并允许下次重试。
    return true;
}

std::wstring LoadConfigValue(const wchar_t* section, const wchar_t* key) {
    const std::wstring configPath = ConfigPath();
    std::vector<wchar_t> value(32768);
    const DWORD length = GetPrivateProfileStringW(section, key, L"",
                                                   value.data(), static_cast<DWORD>(value.size()),
                                                   configPath.c_str());
    return std::wstring(value.data(), length);
}

// 焦点缓存键只含规范路径，不含文件时间，重新复制同名视频仍可记住取景。
std::wstring FocusKey(const std::wstring& path) {
    uint64_t hash = 1469598103934665603ULL;
    for (wchar_t ch : path) { hash ^= static_cast<uint64_t>(towlower(ch)); hash *= 1099511628211ULL; }
    wchar_t text[24]{};
    swprintf_s(text, L"%016llX", static_cast<unsigned long long>(hash));
    return text;
}

void RefreshScreenOptions() {
    g_screens.clear();
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM)->BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info)) g_screens.push_back(
            {info.szDevice,info.rcMonitor,(info.dwFlags&MONITORINFOF_PRIMARY)!=0});
        return TRUE;
    },0);
    std::stable_sort(g_screens.begin(),g_screens.end(),[](const auto& a,const auto& b) {
        return a.primary && !b.primary;
    });
}

std::wstring CurrentCropDevice() {
    const auto found=std::find_if(g_screens.begin(),g_screens.end(),[](const auto& screen) {
        return screen.device==g_cropMonitor;
    });
    return found!=g_screens.end()?found->device:(g_screens.empty()?L"":g_screens.front().device);
}

std::pair<double,double> ScreenFocus(const std::wstring& video,const std::wstring& device,
                                     double defaultX=0.5,double defaultY=0.5) {
    const auto found=g_savedFocus.find(FocusKey(video)+L"_"+FocusKey(device));
    return found!=g_savedFocus.end()?found->second:std::make_pair(defaultX,defaultY);
}

std::wstring BuildScreenFocusMap(const std::wstring& path) {
    double defaultX=0.5,defaultY=0.5;
    const auto common=g_savedFocus.find(FocusKey(path));
    if(common!=g_savedFocus.end()) {defaultX=common->second.first;defaultY=common->second.second;}
    std::wstring text;
    for(const auto& screen:g_screens) {
        const auto focus=ScreenFocus(path,screen.device,defaultX,defaultY);
        text+=screen.device+L"="+std::to_wstring(focus.first)+L","+std::to_wstring(focus.second)+L";";
    }
    return text;
}

std::wstring ScreenLabel(const std::wstring& device,bool all) {
    if(device.empty() && all) return L"全部屏幕";
    // 下拉列表会依次传入每块屏幕的 device；不能用当前取景屏幕代替，
    // 否则双屏时每一行都会显示同一块屏幕的名称和分辨率。
    const auto actual=(!all && device.empty())?CurrentCropDevice():device;
    for(size_t n=0;n<g_screens.size();++n) if(g_screens[n].device==actual) {
        const auto& rect=g_screens[n].rect;
        return L"屏幕 "+std::to_wstring(n+1)+(g_screens[n].primary?L"（主屏）":L"")+
            L" · "+std::to_wstring(rect.right-rect.left)+L"×"+std::to_wstring(rect.bottom-rect.top);
    }
    return L"未连接（暂用主屏）";
}

void SaveVideoFocus() {
    if (g_selectedVideo >= g_videos.size()) return;
    const auto& video = g_videos[g_selectedVideo];
    const auto key = FocusKey(video.path);
    const auto focus = std::make_pair(video.focusX, video.focusY);
    const auto previous = g_savedFocus.find(key);
    if ((previous != g_savedFocus.end() && previous->second != focus) ||
        (previous == g_savedFocus.end() && focus != std::make_pair(0.5, 0.5)))
        g_savedFocus[key] = focus;
    // 每屏焦点也可能单独改变；完整快照的去重由 SaveConfig 处理。
    SaveConfig();
}

void ApplyTheme(HWND owner) {
    const COLORREF oldSuccess=kSuccess,oldWarning=kWarning,oldError=kError,oldMuted=kMuted;
    bool light = g_theme == 1;
    if (g_theme == 2) {
        DWORD appsLight = 1, bytes = sizeof(appsLight);
        // 这里只读 Windows 主题偏好。DWD 自己的配置仍全部写在便携 INI 中。
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsLight, &bytes);
        light = appsLight != 0;
    }
    if (g_theme == 3) light = GetRValue(g_customBackground) * 299 +
        GetGValue(g_customBackground) * 587 + GetBValue(g_customBackground) * 114 >= 150000;
    const auto blend = [](COLORREF a, COLORREF b, int percentage) {
        return RGB((GetRValue(a)*(100-percentage)+GetRValue(b)*percentage)/100,
                   (GetGValue(a)*(100-percentage)+GetGValue(b)*percentage)/100,
                   (GetBValue(a)*(100-percentage)+GetBValue(b)*percentage)/100);
    };
    g_backgroundColor = g_theme == 3 ? g_customBackground :
        (light ? RGB(241,244,249) : kBackground);
    kPanel = g_theme == 3 ? blend(g_backgroundColor, light ? RGB(255,255,255) : RGB(100,115,140), 16) :
        (light ? RGB(255,255,255) : RGB(34,43,57));
    kPanelHover = light ? RGB(232,237,248) : RGB(45,57,75);
    kBorder = light ? RGB(207,215,229) : RGB(64,78,101);
    kText = light ? RGB(31,42,61) : RGB(242,245,250);
    kMuted = light ? RGB(91,107,131) : RGB(153,164,182);
    kAccent = light ? RGB(83,100,221) : RGB(111,124,255);
    kAccentHover = light ? RGB(65,82,205) : RGB(134,146,255);
    kSuccess = light ? RGB(17,125,99) : RGB(72,211,160);
    kWarning = light ? RGB(159,102,14) : RGB(244,190,73);
    kError = light ? RGB(186,52,70) : RGB(242,100,116);
    if(g_statusColor==oldSuccess) g_statusColor=kSuccess;
    else if(g_statusColor==oldWarning) g_statusColor=kWarning;
    else if(g_statusColor==oldError) g_statusColor=kError;
    else if(g_statusColor==oldMuted) g_statusColor=kMuted;
    kControlSurface = light ? RGB(248,250,253) : RGB(28,36,49);
    kDrawerSurface = light ? RGB(247,249,253) : RGB(25,32,43);
    if(g_theme==3) {
        kControlSurface=blend(g_backgroundColor,light?RGB(255,255,255):RGB(100,115,140),10);
        kDrawerSurface=blend(g_backgroundColor,light?RGB(255,255,255):RGB(100,115,140),6);
    }
    if (g_controlBrush) DeleteObject(g_controlBrush);
    g_controlBrush = CreateSolidBrush(kPanel);
    BOOL dark = !light;
    for (HWND window : {owner, g_usageWindow}) if (window) {
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, 35, &kControlSurface, sizeof(kControlSurface));
        DwmSetWindowAttribute(window, 36, &kText, sizeof(kText));
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    RefreshExperienceTheme();
}

HBITMAP LoadShellThumbnail(const std::wstring& path) {
    IShellItemImageFactory* factory{};
    HBITMAP bitmap{};
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
        SIZE size{320, 180};
        const auto flags = static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK |
                                               SIIGBF_RESIZETOFIT);
        factory->GetImage(size, flags, &bitmap);
        factory->Release();
    }
    return bitmap;
}

void ReadVideoDimensions(const std::wstring& path, UINT& width, UINT& height) {
    if(IsWallpaperImage(path)) {Gdiplus::Bitmap image(path.c_str());
        if(image.GetLastStatus()==Gdiplus::Ok){width=image.GetWidth();height=image.GetHeight();}return;}
    // 这里只读取容器中的媒体类型，不读取视频帧，因此不会为每张封面启动完整解码。
    // 放在缩略图线程中执行，避免机械硬盘或格式解析较慢时阻塞界面。
    IMFSourceReader* reader{};
    HRESULT result = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    IMFMediaType* mediaType{};
    if (SUCCEEDED(result)) {
        result = reader->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &mediaType);
        if (SUCCEEDED(result)) {
            result = MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height);
        }
    }
    if (mediaType) mediaType->Release();
    if (reader) reader->Release();
    if (SUCCEEDED(result) && width && height) return;

    // 少数高规格 H.264/HEVC 文件在 Source Reader 创建阶段就会因为解码器能力而失败，
    // 但 Windows 文件属性仍能直接读取容器中的宽高。代理判断只需要元数据，因此用
    // Property System 兜底，避免“能播放却被误判为未知分辨率”。
    width = height = 0;
    IPropertyStore* properties{};
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr,
                                                  GPS_BESTEFFORT,
                                                  IID_PPV_ARGS(&properties)))) return;
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(properties->GetValue(PKEY_Video_FrameWidth, &value))) {
        if (value.vt == VT_UI4) width = value.ulVal;
        else if (value.vt == VT_UI8) width = static_cast<UINT>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    PropVariantInit(&value);
    if (SUCCEEDED(properties->GetValue(PKEY_Video_FrameHeight, &value))) {
        if (value.vt == VT_UI4) height = value.ulVal;
        else if (value.vt == VT_UI8) height = static_cast<UINT>(value.uhVal.QuadPart);
    }
    PropVariantClear(&value);
    properties->Release();
    if (!width || !height) width = height = 0;
}

uint64_t ThumbnailCacheKey(const std::wstring& path) {
    // FNV-1a 哈希输入不仅包含路径，也包含文件大小和最后修改时间。
    // 视频被替换后会自动得到新缓存名，无需遍历并删除旧封面。
    uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xff;
            hash *= 1099511628211ULL;
        }
    };
    for (wchar_t character : path) mix(static_cast<uint64_t>(character));
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error) mix(size);
    error.clear();
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error) mix(static_cast<uint64_t>(modified.time_since_epoch().count()));
    return hash;
}

std::wstring ThumbnailCachePath(const std::wstring& videoPath,
                                const std::wstring& cacheFolder) {
    if (cacheFolder.empty()) return {};
    wchar_t fileName[32]{};
    swprintf_s(fileName, L"%016llX.bmp",
               static_cast<unsigned long long>(ThumbnailCacheKey(videoPath)));
    return (std::filesystem::path(cacheFolder) / fileName).wstring();
}

std::wstring VideoMetadataCachePath(const std::wstring& videoPath,
                                    const std::wstring& cacheFolder) {
    if (cacheFolder.empty()) return {};
    wchar_t fileName[40]{};
    swprintf_s(fileName, L"%016llX.dwdmeta",
               static_cast<unsigned long long>(ThumbnailCacheKey(videoPath)));
    return (std::filesystem::path(cacheFolder) / fileName).wstring();
}

struct CachedVideoMetadata {
    uint32_t magic{0x31445744}; // "DWD1"：用于拒绝旧版本或损坏的缓存。
    uint32_t width{};
    uint32_t height{};
};

bool LoadCachedVideoDimensions(const std::wstring& path, UINT& width, UINT& height) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    CachedVideoMetadata metadata{};
    DWORD read{};
    const bool loaded = ReadFile(file, &metadata, sizeof(metadata), &read, nullptr) != FALSE &&
                        read == sizeof(metadata) && metadata.magic == 0x31445744 &&
                        metadata.width > 0 && metadata.height > 0 &&
                        metadata.width <= 32768 && metadata.height <= 32768;
    CloseHandle(file);
    if (!loaded) return false;
    width = metadata.width;
    height = metadata.height;
    return true;
}

void SaveCachedVideoDimensions(const std::wstring& path, UINT width, UINT height) {
    if (path.empty() || !width || !height) return;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const CachedVideoMetadata metadata{0x31445744, width, height};
    DWORD written{};
    WriteFile(file, &metadata, sizeof(metadata), &written, nullptr);
    CloseHandle(file);
}

void LoadVideoDimensions(const std::wstring& videoPath, const std::wstring& cacheFolder,
                         UINT& width, UINT& height) {
    // 每条缓存只有 12 字节。缓存名沿用包含路径、大小和修改时间的哈希，
    // 因此视频内容变化后会自然失效，不需要读取视频来确认缓存是否仍然正确。
    const std::wstring metadataPath = VideoMetadataCachePath(videoPath, cacheFolder);
    if (!metadataPath.empty() && LoadCachedVideoDimensions(metadataPath, width, height)) return;
    ReadVideoDimensions(videoPath, width, height);
    if (!metadataPath.empty()) SaveCachedVideoDimensions(metadataPath, width, height);
}

SIZE LargestMonitorSize() {
    SIZE largest{};uint64_t maximum{};
    for(const auto& screen:g_screens) {
        const SIZE size{screen.rect.right-screen.rect.left,screen.rect.bottom-screen.rect.top};
        if(!g_targetMonitor.empty() && screen.device==g_targetMonitor) return size;
        const uint64_t area=static_cast<uint64_t>(size.cx)*size.cy;
        if(area>maximum) {maximum=area;largest=size;}
    }
    return largest;
}

ProxySpec BuildProxySpec(const std::wstring& sourcePath, UINT sourceWidth, UINT sourceHeight,
                         int quality, const std::wstring& cacheFolder, SIZE monitor) {
    ProxySpec spec;
    if (quality == 1 || sourcePath.empty() || !sourceWidth || !sourceHeight || cacheFolder.empty()) return spec;
    if (quality == 2 && monitor.cx > 1920) {
        monitor.cy = MulDiv(monitor.cy, 1920, monitor.cx);
        monitor.cx = 1920;
    }
    if (monitor.cx <= 0 || monitor.cy <= 0) return spec;

    const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
    const double monitorAspect = static_cast<double>(monitor.cx) / monitor.cy;
    const double aspectDifference = (std::max)(sourceAspect, monitorAspect) /
                                    (std::min)(sourceAspect, monitorAspect);
    // 第一版只优化比例接近屏幕的视频。竖屏/超宽视频仍保留原文件，避免裁剪后放大失真。
    if (aspectDifference > 1.10) return spec;

    const double scale = (std::min)(1.0,(std::min)(static_cast<double>(monitor.cx) / sourceWidth,
                                    static_cast<double>(monitor.cy) / sourceHeight));
    const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) * sourceHeight;
    const uint64_t monitorPixels = static_cast<uint64_t>(monitor.cx) * monitor.cy;
    if (quality!=2 && (scale >= 0.90 || sourcePixels < monitorPixels * 5 / 4)) return spec;

    spec.width = static_cast<UINT>(sourceWidth * scale + 0.5) & ~1U;
    spec.height = static_cast<UINT>(sourceHeight * scale + 0.5) & ~1U;
    if (spec.width < 640 || spec.height < 360) return {};
    spec.fps = 30;
    const double bitsPerPixelFrame = 0.09;
    const uint64_t estimated = static_cast<uint64_t>(
        spec.width * static_cast<double>(spec.height) * spec.fps * bitsPerPixelFrame);
    spec.bitrate = static_cast<UINT>(std::clamp<uint64_t>(estimated, 6'000'000, 12'000'000));

    const std::filesystem::path folder = std::filesystem::path(cacheFolder) / L"VideoProxy";
    wchar_t name[96]{};
    swprintf_s(name, L"%016llX_%ux%u_%u_%u.mp4",
               static_cast<unsigned long long>(ThumbnailCacheKey(sourcePath)),
               spec.width, spec.height, spec.fps, spec.bitrate);
    spec.finalPath = (folder / name).wstring();
    std::filesystem::path temporary = spec.finalPath;
    temporary.replace_extension(L".part.mp4");
    spec.temporaryPath = temporary.wstring();
    spec.eligible = true;
    return spec;
}

ProxySpec BuildProxySpec(const std::wstring& sourcePath, UINT width, UINT height) {
    return BuildProxySpec(sourcePath, width, height, g_quality, g_cacheFolder, LargestMonitorSize());
}

bool ProxyFileReady(const ProxySpec& spec) {
    if (!spec.eligible || spec.finalPath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(spec.finalPath.c_str(), GetFileExInfoStandard, &data)) return false;
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return size >= 4096;
}

void TouchProxyFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(file, nullptr, nullptr, &now);
    CloseHandle(file);
}

void EnforceProxyCacheLimit(const std::wstring& keepPath = {}) {
    const std::filesystem::path folder = std::filesystem::path(g_cacheFolder) / L"VideoProxy";
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error) ||
        (GetFileAttributesW(folder.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return;
    struct Entry {
        std::filesystem::path path;
        uint64_t size{};
        std::filesystem::file_time_type modified{};
    };
    std::vector<Entry> entries;
    uint64_t total{};
    for (const auto& item : std::filesystem::directory_iterator(
             folder, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) break;
        if (item.is_symlink(error) || !item.is_regular_file(error) || !ManagedCacheFile(item.path(),true)) continue;
        if (item.path().filename().wstring().ends_with(L".part.mp4")) continue;
        const uint64_t size = item.file_size(error);
        if (error) { error.clear(); continue; }
        entries.push_back(Entry{item.path(), size, item.last_write_time(error)});
        total += size;
    }
    if (total <= g_cacheLimitBytes) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.modified < right.modified;
    });
    for (const Entry& entry : entries) {
        if (total <= g_cacheLimitBytes) break;
        if ((!keepPath.empty() && _wcsicmp(entry.path.c_str(), keepPath.c_str()) == 0) ||
            (!g_player.playbackPath.empty() &&
             _wcsicmp(entry.path.c_str(), g_player.playbackPath.c_str()) == 0)) continue;
        if (std::filesystem::remove(entry.path, error)) total -= entry.size;
        error.clear();
    }
}

void StopProxyJob() {
    ++g_proxyGeneration;
    g_pendingProxySource.clear();
    if (g_mainWindow) KillTimer(g_mainWindow, kProxyIdleTimer);
    if (g_proxyJob.process || g_proxyJob.waiter.joinable())
        g_processCleanup.Retire(g_proxyJob.process, g_proxyJob.stopEvent, nullptr, nullptr,
            std::move(g_proxyJob.waiter), g_proxyJob.temporaryPath, 5000);
    g_proxyJob = {};
}

bool StartProxyJob(HWND owner, const std::wstring& sourcePath, const ProxySpec& spec) {
    if (!spec.eligible || ProxyFileReady(spec)) return false;
    if (g_proxyJob.process && _wcsicmp(g_proxyJob.sourcePath.c_str(), sourcePath.c_str()) == 0 &&
        _wcsicmp(g_proxyJob.finalPath.c_str(), spec.finalPath.c_str()) == 0) {
        return true;
    }
    StopProxyJob();

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(spec.finalPath).parent_path(), error);
    if (error) return false;
    EnforceProxyCacheLimit();

    const unsigned generation = ++g_proxyGeneration;
    // 取消的旧任务仍可能短暂写入，用独立临时文件避免新旧任务互相覆盖/删除。
    const auto temporaryPath = spec.finalPath + L"." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(generation) + L".part.mp4";
    const std::wstring stopName = L"Local\\DWDNative_ProxyStop_" +
                                  std::to_wstring(GetCurrentProcessId()) + L"_" +
                                  std::to_wstring(generation);
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
    if (!stopEvent) return false;

    const std::wstring executable = ExeDirectory() + L"\\DWDNative.exe";
    std::wstring command = Quote(executable) + L" --proxy --source " + Quote(sourcePath) +
        L" --output " + Quote(temporaryPath) +
        L" --width " + std::to_wstring(spec.width) +
        L" --height " + std::to_wstring(spec.height) +
        L" --fps " + std::to_wstring(spec.fps) +
        L" --bitrate " + std::to_wstring(spec.bitrate) +
        L" --stop-event " + Quote(stopName) +
        L" --parent-pid " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        ExeDirectory().c_str(), &startup, &processInfo)) {
        CloseHandle(stopEvent);
        return false;
    }
    CloseHandle(processInfo.hThread);

    g_proxyJob.process = processInfo.hProcess;
    g_proxyJob.stopEvent = stopEvent;
    g_proxyJob.generation = generation;
    g_proxyJob.sourcePath = sourcePath;
    g_proxyJob.finalPath = spec.finalPath;
    g_proxyJob.temporaryPath = temporaryPath;
    g_proxyJob.waiter = std::thread([owner, generation, process = processInfo.hProcess,
                                     sourcePath, finalPath = spec.finalPath,
                                     temporaryPath] {
        WaitForSingleObject(process, INFINITE);
        DWORD exitCode = ERROR_GEN_FAILURE;
        GetExitCodeProcess(process, &exitCode);
        auto* result = new ProxyResult{generation, exitCode, sourcePath, finalPath,
                                       temporaryPath};
        if (!PostMessageW(owner, kProxyReady, 0, reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
    });
    return true;
}

HBITMAP NormalizeThumbnail(HBITMAP sourceBitmap) {
    // Shell 返回的封面尺寸可能很大。这里统一裁成 320x180，既控制磁盘占用，
    // 也避免 UI 同时持有几十张高分辨率 HBITMAP 导致 GDI/内存膨胀。
    if (!sourceBitmap) return nullptr;
    BITMAP source{};
    if (!GetObjectW(sourceBitmap, sizeof(source), &source) ||
        source.bmWidth <= 0 || source.bmHeight <= 0) return nullptr;
    constexpr int targetWidth = 320;
    constexpr int targetHeight = 180;
    BITMAPINFO targetInfo{};
    targetInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    targetInfo.bmiHeader.biWidth = targetWidth;
    targetInfo.bmiHeader.biHeight = -targetHeight;
    targetInfo.bmiHeader.biPlanes = 1;
    targetInfo.bmiHeader.biBitCount = 32;
    targetInfo.bmiHeader.biCompression = BI_RGB;
    void* bits{};
    HDC screen = GetDC(nullptr);
    HBITMAP targetBitmap = screen ? CreateDIBSection(screen, &targetInfo, DIB_RGB_COLORS,
                                                     &bits, nullptr, 0) : nullptr;
    if (!screen || !targetBitmap) {
        if (screen) ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HDC sourceDc = CreateCompatibleDC(screen);
    HDC targetDc = CreateCompatibleDC(screen);
    HGDIOBJ oldSource = SelectObject(sourceDc, sourceBitmap);
    HGDIOBJ oldTarget = SelectObject(targetDc, targetBitmap);
    SetStretchBltMode(targetDc, HALFTONE);
    int sourceX = 0;
    int sourceY = 0;
    int sourceWidth = source.bmWidth;
    int sourceHeight = source.bmHeight;
    const double sourceAspect = static_cast<double>(sourceWidth) / sourceHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
    if (sourceAspect > targetAspect) {
        sourceWidth = static_cast<int>(sourceHeight * targetAspect);
        sourceX = (source.bmWidth - sourceWidth) / 2;
    } else if (sourceAspect < targetAspect) {
        sourceHeight = static_cast<int>(sourceWidth / targetAspect);
        sourceY = (source.bmHeight - sourceHeight) / 2;
    }
    StretchBlt(targetDc, 0, 0, targetWidth, targetHeight,
               sourceDc, sourceX, sourceY, sourceWidth, sourceHeight, SRCCOPY);
    SelectObject(sourceDc, oldSource);
    SelectObject(targetDc, oldTarget);
    DeleteDC(sourceDc);
    DeleteDC(targetDc);
    ReleaseDC(nullptr, screen);
    return targetBitmap;
}

bool SaveBitmapFile(HBITMAP bitmap, const std::wstring& path) {
    if (!bitmap || path.empty()) return false;
    BITMAP source{};
    if (!GetObjectW(bitmap, sizeof(source), &source) || source.bmWidth <= 0 || source.bmHeight <= 0) {
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source.bmWidth;
    info.bmiHeader.biHeight = -source.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD pixelBytes = static_cast<DWORD>(source.bmWidth * source.bmHeight * 4ULL);
    std::vector<BYTE> pixels(pixelBytes);
    HDC screen = GetDC(nullptr);
    const int rows = screen ? GetDIBits(screen, bitmap, 0, static_cast<UINT>(source.bmHeight),
                                        pixels.data(), &info, DIB_RGB_COLORS) : 0;
    if (screen) ReleaseDC(nullptr, screen);
    if (rows != source.bmHeight) return false;

    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4D42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + pixelBytes;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    bool saved = WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr) != FALSE &&
                 written == sizeof(fileHeader);
    if (saved) {
        saved = WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) != FALSE &&
                written == sizeof(info.bmiHeader);
    }
    if (saved) {
        saved = WriteFile(file, pixels.data(), pixelBytes, &written, nullptr) != FALSE &&
                written == pixelBytes;
    }
    CloseHandle(file);
    return saved;
}

HBITMAP LoadThumbnail(const std::wstring& videoPath, const std::wstring& cacheFolder) {
    const std::wstring cachePath = ThumbnailCachePath(videoPath, cacheFolder);
    if (!cachePath.empty()) {
        // 优先从磁盘读 BMP。若旧版本缓存尺寸不正确，加载后顺便标准化并覆盖。
        if (auto bitmap = reinterpret_cast<HBITMAP>(LoadImageW(nullptr, cachePath.c_str(),
                                                               IMAGE_BITMAP, 0, 0,
                                                               LR_LOADFROMFILE | LR_CREATEDIBSECTION))) {
            BITMAP info{};
            GetObjectW(bitmap, sizeof(info), &info);
            if (info.bmWidth != 320 || info.bmHeight != 180) {
                HBITMAP normalized = NormalizeThumbnail(bitmap);
                DeleteObject(bitmap);
                if (normalized) {
                    SaveBitmapFile(normalized, cachePath);
                    return normalized;
                }
                return nullptr; // 转换失败时 bitmap 已被释放，不能把失效句柄交给 UI。
            }
            return bitmap;
        }
    }
    HBITMAP shellBitmap = LoadShellThumbnail(videoPath);
    HBITMAP bitmap = NormalizeThumbnail(shellBitmap);
    if (shellBitmap) DeleteObject(shellBitmap);
    if (bitmap && !cachePath.empty()) SaveBitmapFile(bitmap, cachePath);
    return bitmap;
}

void StopThumbnailWorker() {
    g_stopThumbnails = true;
    {
        std::scoped_lock lock(g_thumbnailTaskMutex);
        g_thumbnailTasks.clear();
    }
    g_thumbnailTaskReady.notify_all();
    if (g_thumbnailThread.joinable()) g_thumbnailThread.join();
    g_stopThumbnails = false;
}

void ClearVideoLibrary() {
    if(g_tooltip) {
        TOOLINFOW tool{sizeof(tool)};tool.hwnd=g_mainWindow;tool.uId=0xD0;
        SendMessageW(g_tooltip,TTM_POP,0,0);
        SendMessageW(g_tooltip,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    }
    StopThumbnailWorker();
    g_scaledBitmaps.Clear();
    g_residentThumbnails.clear(); g_wantedThumbnails.clear();
    for (auto& video : g_videos) {
        if (video.thumbnail) DeleteObject(video.thumbnail);
    }
    g_videos.clear();
}

void StartThumbnailWorker(HWND owner) {
    if (g_thumbnailThread.joinable()) return;
    // 持久工作线程只处理当前页及相邻页的任务；翻页时替换队列，不再扫描并常驻全部100张封面。
    g_thumbnailThread = std::thread([owner] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        while (!g_stopThumbnails) {
            ThumbnailTask task;
            {
                std::unique_lock lock(g_thumbnailTaskMutex);
                g_thumbnailTaskReady.wait(lock, [] {
                    return g_stopThumbnails || (!g_pauseThumbnailWork && !g_thumbnailTasks.empty());
                });
                if (g_stopThumbnails) break;
                task = std::move(g_thumbnailTasks.front());
                g_thumbnailTasks.pop_front();
            }
            if (g_stopThumbnails) break;
            if (task.generation != g_thumbnailGeneration) continue;

            std::error_code error;
            if (!task.cacheFolder.empty()) {
                std::filesystem::create_directories(task.cacheFolder, error);
            }
            UINT width{};
            UINT height{};
            LoadVideoDimensions(task.path, task.cacheFolder, width, height);
            if (g_stopThumbnails || task.generation != g_thumbnailGeneration) continue;
            HBITMAP bitmap = LoadThumbnail(task.path, task.cacheFolder);
            if (g_stopThumbnails) {
                if (bitmap) DeleteObject(bitmap);
                break;
            }
            if (task.generation != g_thumbnailGeneration.load()) {
                if (bitmap) DeleteObject(bitmap);
                continue;
            }
            auto* result = new ThumbnailResult{task.generation, task.index, bitmap, width, height};
            if (!PostMessageW(owner, kThumbnailReady, 0, reinterpret_cast<LPARAM>(result))) {
                if (bitmap) DeleteObject(bitmap);
                delete result;
                break;
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        CoUninitialize();
    });
}

void TrimThumbnailMemory() {
    // 原始封面最多 32 张，约 7 MiB；不是把整个图库留在内存。
    while (g_residentThumbnails.size() > 32) {
        size_t oldest = SIZE_MAX;
        for (size_t index : g_residentThumbnails) {
            if (g_wantedThumbnails.contains(index)) continue;
            if (oldest == SIZE_MAX || g_videos[index].thumbnailUse < g_videos[oldest].thumbnailUse) oldest = index;
        }
        if (oldest == SIZE_MAX) break;
        auto& video = g_videos[oldest];
        g_scaledBitmaps.Forget(video.thumbnail); DeleteObject(video.thumbnail); video.thumbnail = nullptr;
        g_residentThumbnails.erase(oldest);
    }
}

void QueueVisibleThumbnails(HWND owner) {
    if (g_videos.empty()) return;
    StartThumbnailWorker(owner);
    const unsigned generation = g_thumbnailGeneration.fetch_add(1) + 1;
    const size_t first = (g_galleryPage ? g_galleryPage - 1 : 0) * kGalleryPageSize;
    const size_t last = (std::min)(g_galleryIndices.size(), (g_galleryPage + 2) * kGalleryPageSize);
    g_wantedThumbnails.clear();
    std::deque<ThumbnailTask> tasks;
    const auto enqueue = [&](size_t index) {
        if (index >= g_videos.size() || !g_wantedThumbnails.insert(index).second) return;
        g_videos[index].thumbnailUse = ++g_thumbnailUse;
        if (!g_videos[index].thumbnail)
            tasks.push_back({generation, index, g_videos[index].path, g_cacheFolder});
    };
    enqueue(g_selectedVideo);
    for (size_t n = g_galleryPage * kGalleryPageSize; n < (std::min)(last, (g_galleryPage + 1) * kGalleryPageSize); ++n)
        enqueue(g_galleryIndices[n]);
    for (size_t n = first; n < last; ++n) enqueue(g_galleryIndices[n]);
    TrimThumbnailMemory();
    { std::scoped_lock lock(g_thumbnailTaskMutex); g_thumbnailTasks = std::move(tasks); }
    g_thumbnailTaskReady.notify_one();
}

void RebuildGallery(HWND owner, bool preservePage) {
    if(g_tooltip) {
        TOOLINFOW tool{sizeof(tool)};tool.hwnd=owner;tool.uId=0xD0;
        SendMessageW(g_tooltip,TTM_POP,0,0);
        SendMessageW(g_tooltip,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool));
    }
    g_galleryIndices.clear();
    std::wstring query = g_searchText;
    std::transform(query.begin(), query.end(), query.begin(), towlower);
    for (size_t index = 0; index < g_videos.size(); ++index) {
        const auto found=g_preferences.find(FocusKey(g_videos[index].path));
        const LibraryPreference pref=found==g_preferences.end()?LibraryPreference{}:found->second;
        if(g_libraryFilter==1 && !pref.favorite)continue;
        if(g_libraryFilter==2 && !pref.lastUsed)continue;
        if (g_videos[index].searchTitle.find(query) != std::wstring::npos) g_galleryIndices.push_back(index);
    }
    std::sort(g_galleryIndices.begin(), g_galleryIndices.end(), [](size_t a, size_t b) {
        if(g_libraryFilter==2){const auto usedA=g_preferences[FocusKey(g_videos[a].path)].lastUsed;
            const auto usedB=g_preferences[FocusKey(g_videos[b].path)].lastUsed;if(usedA!=usedB)return usedA>usedB;}
        if (g_sortOrder == 2 && g_videos[a].modified != g_videos[b].modified)
            return g_videos[a].modified > g_videos[b].modified;
        const int order = StrCmpLogicalW(g_videos[a].title.c_str(), g_videos[b].title.c_str());
        return g_sortOrder == 1 ? order > 0 : order < 0;
    });
    const size_t pages = (std::max<size_t>)(1, (g_galleryIndices.size()+kGalleryPageSize-1)/kGalleryPageSize);
    g_galleryPage = preservePage ? (std::min)(g_galleryPage, pages - 1) : 0;
    g_hoverThumbnail = -1;
    QueueVisibleThumbnails(owner);
    LayoutControls(owner);
    InvalidateRect(owner, nullptr, FALSE);
}

void BeginCurrentPreview(HWND owner) {
    KillTimer(owner, kPreviewSwitchTimer);
    if(g_inTray)return;
    if(CurrentEffects().Enabled() || IsWallpaperImage(g_videoPath)) {
        if(!StartEffectPreview(owner)){g_previewLoading=false;PauseThumbnailWork(false);
            SetStatus(owner,L"效果预览未能启动，请重试或关闭效果",kError);}return;
    }
    StopEffectPreview();
    if (!g_previewPlayer || g_videoPath.empty() || !g_previewWindow) {
        g_previewLoading = false;
        PauseThumbnailWork(false);
        return;
    }

    // 如果该原片已经生成过屏幕适配代理，前台预览也复用它。这样再次点击 4K 壁纸时
    // 不必重新打开 4K/60fps 解码器，预览首帧更快，前台临时内存也更低。
    UINT sourceWidth{};
    UINT sourceHeight{};
    if (g_selectedVideo < g_videos.size() &&
        _wcsicmp(g_videos[g_selectedVideo].path.c_str(), g_videoPath.c_str()) == 0) {
        sourceWidth = g_videos[g_selectedVideo].width;
        sourceHeight = g_videos[g_selectedVideo].height;
    }
    // 元数据/缓存选择与 MFPlay 都在媒体线程执行，捕获值快照，不读取 UI 全局变量。
    // 冷启动仍能命中已存在的优化视频，不能为了异步化而退回高内存原片。
    auto prepare = [path = g_videoPath, cache = g_cacheFolder, quality = g_quality,
                    monitor = LargestMonitorSize(), sourceWidth, sourceHeight]() mutable {
        if (!sourceWidth || !sourceHeight) LoadVideoDimensions(path, cache, sourceWidth, sourceHeight);
        const auto spec = BuildProxySpec(path, sourceWidth, sourceHeight, quality, cache, monitor);
        if (ProxyFileReady(spec)) { TouchProxyFile(spec.finalPath); return spec.finalPath; }
        return path;
    };
    const uint64_t request = g_previewPlayer->OpenAsync(g_videoPath, g_previewWindow, std::move(prepare));
    g_previewRequest = request;
    if (request) SetTimer(owner, kPreviewTimeoutTimer, 12000, nullptr);
    if (!request) {
        g_previewLoading = false;
        PauseThumbnailWork(false);
        SetStatus(owner, L"无法预览该视频，仍可尝试应用到桌面", kError);
    }
}

bool IsAppliedVideo(const std::wstring& path) {
    if (!g_player.process || g_player.videoPath.empty() ||
        _wcsicmp(g_player.videoPath.c_str(), path.c_str()) != 0) {
        return false;
    }
    DWORD exitCode{};
    return GetExitCodeProcess(g_player.process, &exitCode) && exitCode == STILL_ACTIVE;
}

bool RestoreAppliedSharedPreview(HWND owner) {
    if (!IsAppliedVideo(g_videoPath) || g_player.previewTextureName.empty()) return false;
    KillTimer(owner, kPreviewSwitchTimer);
    KillTimer(owner, kOverlayTimer);
    PauseThumbnailWork(false);
    if (g_previewPlayer) g_previewPlayer->Stop();
    if (!ActivateSharedPreview(owner, g_player.previewTextureName)) return false;
    SetStatus(owner, L"已应用到桌面 · 动态预览已连接", kSuccess);
    return true;
}

void ScheduleCurrentPreview(HWND owner, bool immediate) {
    StopEffectPreview();g_previewOnlyPaused=false;
    KillTimer(owner, kPreviewTimeoutTimer);
    KillTimer(owner, kPreviewSwitchTimer);
    KillTimer(owner, kOverlayTimer);
    StopSharedPreview(owner, false);
    if (g_previewPlayer) g_previewPlayer->CancelPending();

    g_previewRequest = 0;
    g_previewActive = false;
    g_previewLoading = !g_videoPath.empty();
    PauseThumbnailWork(g_previewLoading);
    ShowWindow(g_previewWindow, SW_HIDE);
    ShowWindow(g_cropOverlay, SW_HIDE);

    // 隐藏旧视频后，主窗口会立即绘制刚点击的磁盘缓存封面；真正的视频在异步就绪后接管。
    InvalidateRect(owner, nullptr, FALSE);
    if (!g_previewLoading) return;
    if (g_selectedVideo < g_videos.size()) {
        SetStatus(owner, L"正在载入预览 · " + g_videos[g_selectedVideo].title, kWarning);
    } else {
        SetStatus(owner, L"正在载入视频预览…", kWarning);
    }
    if (immediate) BeginCurrentPreview(owner);
    else SetTimer(owner, kPreviewSwitchTimer, kPreviewSwitchDelayMs, nullptr);
}

void SelectVideo(HWND owner, size_t index) {
    // 120 ms 合并连续点击；播放器本身不再重建，只异步替换 MediaItem。
    if (index >= g_videos.size()) return;
    SaveVideoFocus();
    const bool sameSelection = g_selectedVideo == index &&
        _wcsicmp(g_videoPath.c_str(), g_videos[index].path.c_str()) == 0;
    g_lastInteraction = GetTickCount64();
    g_selectedVideo = index;
    g_videoPath = g_videos[index].path;
    g_compareOriginal=false;
    if(!sameSelection) {FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER stamp{};
        stamp.LowPart=now.dwLowDateTime;stamp.HighPart=now.dwHighDateTime;
        g_preferences[FocusKey(g_videoPath)].lastUsed=stamp.QuadPart;SaveConfig();}
    UpdateCoverAccent(owner);
    if(g_sharedPreview)g_sharedPreview->SetEffects(CurrentEffects(),CurrentVideoSize());
    if(g_experienceWindow)PostMessageW(g_experienceWindow,WM_APP+1,0,0);
    LayoutControls(owner);
    QueueVisibleThumbnails(owner);
    EnableWindow(g_applyButton, TRUE);
    if (g_inTray) { PauseThumbnailWork(false); return; }

    // 已应用视频的动态画面已经由后台通过共享纹理提供，不要因为再次点击封面
    // 就重新启动一条 MFPlay 解码管线。这也是避免 UI 内存突然上涨的关键路径。
    if (IsAppliedVideo(g_videoPath)) {
        if (g_sharedPreviewActive || RestoreAppliedSharedPreview(owner)) {
            SetStatus(owner, L"已应用到桌面 · 动态预览已连接", kSuccess);
            InvalidateRect(owner, nullptr, FALSE);
            return;
        }
    }
    if (sameSelection && (g_previewActive || g_previewLoading)) return;
    ScheduleCurrentPreview(owner, false);
}

void ApplyLibrarySnapshot(HWND owner, LibrarySnapshot& snapshot) {
    if (g_applying) return;
    // 拖入的单个文件属于显式加入的素材；刷新文件夹不能把它从库里悄悄移除。
    for(const auto& path:g_externalFiles) {
        if(std::any_of(snapshot.entries.begin(),snapshot.entries.end(),[&](const auto& e){return _wcsicmp(e.path.c_str(),path.c_str())==0;}))continue;
        WIN32_FILE_ATTRIBUTE_DATA info{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info))continue;
        LibraryEntry entry;entry.path=path;entry.title=std::filesystem::path(path).stem().wstring();entry.searchTitle=entry.title;
        std::transform(entry.searchTitle.begin(),entry.searchTitle.end(),entry.searchTitle.begin(),towlower);
        entry.size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        entry.modified=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
        snapshot.entries.push_back(std::move(entry));
    }
    const auto preferredVideo = snapshot.preferred.empty() ? g_videoPath : snapshot.preferred;
    const bool folderChanged = _wcsicmp(snapshot.folder.c_str(), g_wallpaperFolder.c_str()) != 0;
    SaveVideoFocus();
    // 只换掉变化的文件；未变化的对象携带现有封面、分辨率和焦点直接迁移。
    ++g_thumbnailGeneration;
    std::map<std::wstring, size_t> previous;
    for (size_t i = 0; i < g_videos.size(); ++i) previous.emplace(g_videos[i].path, i);
    std::vector<VideoItem> next;
    bool selectedChanged = false;
    for (const auto& entry : snapshot.entries) {
        VideoItem item;
        const auto old = previous.find(entry.path);
        if (old != previous.end() && g_videos[old->second].modified == entry.modified &&
            g_videos[old->second].fileSize == entry.size) {
            item = std::move(g_videos[old->second]);
            g_videos[old->second].thumbnail = nullptr;
        } else {
            item.path = entry.path; item.title = entry.title; item.searchTitle = entry.searchTitle;
            item.modified = entry.modified; item.fileSize = entry.size;
            const auto saved = g_savedFocus.find(FocusKey(item.path));
            if (saved != g_savedFocus.end()) { item.focusX = saved->second.first; item.focusY = saved->second.second; }
            if (_wcsicmp(item.path.c_str(), g_videoPath.c_str()) == 0) selectedChanged = true;
        }
        next.push_back(std::move(item));
    }
    for (auto& item : g_videos) if (item.thumbnail) {
        g_scaledBitmaps.Forget(item.thumbnail); DeleteObject(item.thumbnail);
    }
    g_videos = std::move(next);
    g_residentThumbnails.clear(); g_wantedThumbnails.clear();
    for (size_t i = 0; i < g_videos.size(); ++i)
        if (g_videos[i].thumbnail) g_residentThumbnails.insert(i);
    g_wallpaperFolder = snapshot.folder;
    if (folderChanged) g_directoryWatcher.Start(snapshot.folder, owner, kDirectoryChanged);
    if (snapshot.saveSetting) SaveConfig();
    if (folderChanged || selectedChanged) {
        g_videoPath.clear(); g_previewActive = g_previewLoading = false;
    }
    g_selectedVideo = 0;
    EnableWindow(g_refreshButton, !snapshot.folder.empty());
    if (!preferredVideo.empty()) {
        const auto selected = std::find_if(g_videos.begin(), g_videos.end(),
            [&preferredVideo](const VideoItem& item) {
                return _wcsicmp(item.path.c_str(), preferredVideo.c_str()) == 0;
            });
        if (selected != g_videos.end()) {
            g_selectedVideo = static_cast<size_t>(std::distance(g_videos.begin(), selected));
            g_galleryPage = g_selectedVideo / kGalleryPageSize;
        }
    }

    RebuildGallery(owner, !folderChanged);
    if(preferredVideo.empty() && !g_galleryIndices.empty()) g_selectedVideo=g_galleryIndices.front();
    const auto position=std::find(g_galleryIndices.begin(),g_galleryIndices.end(),g_selectedVideo);
    if(position!=g_galleryIndices.end()) g_galleryPage=static_cast<size_t>(position-g_galleryIndices.begin())/kGalleryPageSize;
    if (g_videos.empty()) {
        g_videoPath.clear();
        StopSharedPreview(owner);
        if (g_previewPlayer) g_previewPlayer->Stop();
        g_previewActive = false;
        g_previewLoading = false;
        g_previewRequest = 0;
        PauseThumbnailWork(false);
        ShowWindow(g_previewWindow, SW_HIDE);
        ShowWindow(g_cropOverlay, SW_HIDE);
        EnableWindow(g_applyButton, FALSE);
        SetStatus(owner, L"该文件夹中没有支持的视频", kWarning);
    } else {
        SelectVideo(owner, g_selectedVideo);
        QueueVisibleThumbnails(owner);
        if (snapshot.saveSetting) SetStatus(owner, L"壁纸文件夹已配置，启动时会自动加载", kSuccess);
    }
    InvalidateRect(owner, nullptr, TRUE);
}

void ScanWallpaperFolder(HWND owner, const std::wstring& folder, bool saveSetting,
                         const std::wstring& preferredVideo = {}) {
    g_requestedFolder = folder;
    g_scanPending = true;
    g_deferredLibrary.reset(); KillTimer(owner, kLibraryApplyTimer);
    g_libraryGeneration = g_libraryScanner.Request(owner, kLibraryReady, folder, preferredVideo, saveSetting, false);
    SetStatus(owner, L"正在刷新视频库，当前预览保持播放…", kMuted);
}

void RefreshVideoLibrary(HWND owner) {
    if (g_wallpaperFolder.empty()) {
        SetStatus(owner, L"请先在设置中选择壁纸文件夹", kWarning);
        return;
    }
    const std::wstring selectedVideo = g_videoPath;
    ScanWallpaperFolder(owner, g_wallpaperFolder, false, selectedVideo);
}

bool ChooseWallpaperFolder(HWND owner) {
    IFileOpenDialog* dialog{};
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return false;
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择动态壁纸文件夹");
    if (!g_wallpaperFolder.empty()) {
        IShellItem* initial{};
        if (SUCCEEDED(SHCreateItemFromParsingName(g_wallpaperFolder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&initial)))) {
            dialog->SetFolder(initial);
            initial->Release();
        }
    }
    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* result{};
        PWSTR path{};
        hr = dialog->GetResult(&result);
        if (SUCCEEDED(hr)) hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (SUCCEEDED(hr) && path) ScanWallpaperFolder(owner, path, true);
        if (path) CoTaskMemFree(path);
        if (result) result->Release();
    }
    dialog->Release();
    return SUCCEEDED(hr);
}

bool ChooseCacheFolder(HWND owner) {
    IFileOpenDialog* dialog{};
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return false;
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择缓存文件夹（封面和优化视频）");
    if (!g_cacheFolder.empty()) {
        IShellItem* initial{};
        if (SUCCEEDED(SHCreateItemFromParsingName(g_cacheFolder.c_str(), nullptr,
                                                  IID_PPV_ARGS(&initial)))) {
            dialog->SetFolder(initial);
            initial->Release();
        }
    }
    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* result{};
        PWSTR path{};
        hr = dialog->GetResult(&result);
        if (SUCCEEDED(hr)) hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (SUCCEEDED(hr) && path) {
            StopProxyJob();
            g_cacheFolder = path;
            std::error_code error;
            std::filesystem::create_directories(g_cacheFolder, error);
            const bool saved = SaveConfig();
            if (!g_wallpaperFolder.empty()) RefreshVideoLibrary(owner);
            SetStatus(owner,
                      saved ? L"缓存文件夹已保存，封面和优化视频将写入新目录"
                            : L"缓存目录已使用，但配置保存失败",
                      saved ? kSuccess : kError);
        }
        if (path) CoTaskMemFree(path);
        if (result) result->Release();
    }
    dialog->Release();
    return SUCCEEDED(hr);
}

void ChooseBackgroundColor(HWND owner) {
    // Windows 的颜色对话框原生支持基础色、自定义色和 RGB 输入，不需要额外 UI 库。
    static COLORREF customColors[16]{};
    CHOOSECOLORW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.rgbResult = g_customBackground;
    dialog.lpCustColors = customColors;
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) return;

    g_customBackground = dialog.rgbResult;
    g_theme = 3;
    ApplyTheme(owner);
    const bool saved = SaveConfig();
    InvalidateRect(owner, nullptr, TRUE);
    if (g_chooseBackgroundButton) InvalidateRect(g_chooseBackgroundButton, nullptr, TRUE);
    SetStatus(owner,
              saved ? L"应用背景颜色已保存" : L"颜色已应用，但配置保存失败",
              saved ? kSuccess : kError);
}

void SetSettingsOpen(HWND owner, bool open) {
    g_settingsOpen = open;
    // 设置和画面编辑互斥，避免两侧同时遮住预览且无处操作。
    if(open && g_experienceWindow)ShowWindow(g_experienceWindow,SW_HIDE);
    if (g_previewActive) {
        if (open) {
            // 设置抽屉覆盖预览区域，暂停并隐藏预览可避免无意义地解码被遮住的画面。
            if (g_previewPlayer) g_previewPlayer->Pause();
            ShowWindow(g_previewWindow, SW_HIDE);
            ShowWindow(g_sharedPreviewWindow, SW_HIDE);
            ShowWindow(g_cropOverlay, SW_HIDE);
        } else if (!g_inTray) {
            ShowWindow(g_sharedPreviewActive ? g_sharedPreviewWindow : g_previewWindow, SW_SHOWNA);
            ShowWindow(g_cropOverlay, g_cropEditing ? SW_SHOWNA : SW_HIDE);
            SetWindowPos(g_cropOverlay, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (g_sharedPreviewActive) {
                if (g_sharedPreview) g_sharedPreview->Render();
            } else if (g_previewPlayer) {
                g_previewPlayer->Play();
                g_previewPlayer->UpdateVideo();
            }
        }
    }
    LayoutControls(owner);
    InvalidateRect(g_settingsButton, nullptr, FALSE);
    InvalidateRect(owner, nullptr, FALSE);
}

// 暂停由独立命名事件通知后台；不会暂停使用时长统计，也不用模拟鼠标操作。
void ToggleWallpaperPause(HWND owner) {
    if (!g_player.process || !g_player.pauseEvent || g_applying) return;
    g_wallpaperPaused = !g_wallpaperPaused;
    if (g_wallpaperPaused) SetEvent(g_player.pauseEvent);
    else ResetEvent(g_player.pauseEvent);
    SetWindowTextW(g_extraButtons[kPauseButton], g_wallpaperPaused ? L"继续播放" : L"暂停壁纸");
    SetStatus(owner, g_wallpaperPaused ? L"壁纸已暂停" : L"壁纸继续播放", kSuccess);
    InvalidateRect(g_extraButtons[kPauseButton], nullptr, FALSE);
}

void StopWallpaper(HWND owner) {
    if (g_applying) return;
    StopProxyJob();
    StopSharedPreview(owner);
    StopPlayer(g_player, true);
    g_wallpaperPaused = false;
    g_lastWallpaper.clear();
    g_recoveryAttempts = 0;
    SaveConfig();
    SetWindowTextW(g_extraButtons[kPauseButton], L"暂停壁纸");
    LayoutControls(owner);
    if (!g_inTray && !g_settingsOpen) ScheduleCurrentPreview(owner, true);
    SetStatus(owner, L"已恢复 Windows 静态壁纸", kSuccess);
}

// 只认自己的缓存命名，不递归删除用户指定目录，更不删除原始视频。
bool ManagedCacheFile(const std::filesystem::path& path, bool proxy) {
    const auto name = path.filename().wstring();
    if (name.size() < 20) return false;
    if (!std::all_of(name.begin(), name.begin()+16, [](wchar_t ch) { return iswxdigit(ch) != 0; }))
        return false;
    if (!proxy) return name.size() == 20 && name.ends_with(L".bmp");
    if (name[16] != L'_' || !name.ends_with(L".mp4") || name.ends_with(L".part.mp4")) return false;
    return std::all_of(name.begin()+17, name.end()-4, [](wchar_t ch) {
        return (ch >= L'0' && ch <= L'9') || ch == L'_' || ch == L'x';
    });
}

std::vector<std::filesystem::path> CacheFiles(bool proxies) {
    std::vector<std::filesystem::path> files;
    if (g_cacheFolder.empty()) return files;
    const auto folder = proxies ? std::filesystem::path(g_cacheFolder)/L"VideoProxy" :
                                  std::filesystem::path(g_cacheFolder);
    std::error_code error;
    if(proxies && (GetFileAttributesW(folder.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(folder,
             std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) break;
        if (!entry.is_symlink(error) && entry.is_regular_file(error) &&
            ManagedCacheFile(entry.path(), proxies)) files.push_back(entry.path());
        error.clear();
    }
    return files;
}

void RefreshCacheUsage() {
    g_cacheUsageBytes = 0;
    for (bool proxies : {false, true}) for (const auto& file : CacheFiles(proxies)) {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        if (!error) g_cacheUsageBytes += size;
    }
}

void ClearCache(HWND owner, bool proxies) {
    if (g_applying || g_proxyJob.process) {
        SetStatus(owner, L"正在准备壁纸或生成缓存，请完成后再清理", kWarning); return;
    }
    if (MessageBoxW(owner, proxies ?
        L"清理未在使用的优化视频缓存？\n原视频与当前壁纸不会被删除。" :
        L"清理 DWD 生成的封面缓存？\n需要时会重新生成，不影响原视频。",
        L"DWD · 清理缓存", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!proxies) StopThumbnailWorker();
    size_t removed{};
    for (const auto& file : CacheFiles(proxies)) {
        if (_wcsicmp(file.c_str(), g_player.playbackPath.c_str()) == 0 ||
            _wcsicmp(file.c_str(), g_videoPath.c_str()) == 0) continue;
        std::error_code error;
        if (std::filesystem::remove(file, error)) ++removed;
    }
    if (!proxies) {
        g_scaledBitmaps.Clear(); g_residentThumbnails.clear();
        for (auto& video : g_videos) {
            if (video.thumbnail) DeleteObject(video.thumbnail);
            video.thumbnail = nullptr;
        }
        QueueVisibleThumbnails(owner);
    }
    RefreshCacheUsage();
    SetStatus(owner, L"已清理 " + std::to_wstring(removed) + L" 个可重新生成的缓存文件", kSuccess);
}

bool WaitForPlayer(HWND owner, PlayerInstance& player, DWORD timeout) {
    // 等待新播放器首帧期间仍然泵送窗口消息，否则控制窗口会“未响应”，
    // 按钮重绘、系统关闭和 DPI 消息也会被堵住。
    HANDLE waits[2]{player.readyEvent, player.process};
    const ULONGLONG deadline = GetTickCount64() + timeout;
    while (IsWindow(owner)) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        const DWORD result = MsgWaitForMultipleObjects(2, waits, FALSE, remaining, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) return true;
        if (result == WAIT_OBJECT_0 + 1 || result == WAIT_TIMEOUT) return false;
        if (result == WAIT_OBJECT_0 + 2) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                    return false;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }
    return false;
}

void SetApplying(HWND owner, bool applying) {
    g_applying = applying;
    EnableWindow(g_settingsButton, !applying);
    EnableWindow(g_chooseFolderButton, !applying);
    EnableWindow(g_chooseCacheButton, !applying);
    EnableWindow(g_chooseBackgroundButton, !applying);
    EnableWindow(g_activeModeButton, !applying);
    EnableWindow(g_viewingModeButton, !applying);
    EnableWindow(g_usageButton, !applying);
    EnableWindow(g_refreshButton, !applying && !g_wallpaperFolder.empty());
    EnableWindow(g_applyButton, !applying && !g_videoPath.empty());
    for(const auto& [id,button]:g_extraButtons) EnableWindow(button,!applying);
    EnableWindow(g_searchEdit,!applying);EnableWindow(g_sortCombo,!applying);
    LayoutControls(owner);
    if (applying) SetStatus(owner, L"正在准备首帧，原壁纸会保持显示…", kWarning);
}

void ResumePreviewAfterApplyFailure() {
    if (g_previewPlayer && g_previewActive && !g_inTray) g_previewPlayer->Play();
}

bool LaunchPlayer(HWND owner) {
    if (g_applying) return false;
    if (g_videoPath.empty()) {
        SetStatus(owner, L"请先在设置中选择包含视频的壁纸文件夹", kWarning);
        return false;
    }
    if(GetFileAttributesW(g_videoPath.c_str())==INVALID_FILE_ATTRIBUTES){
        SetStatus(owner,L"原文件已移动或不可访问 · 在“更多”中重新定位",kError);return false;}
    const double focusX = CurrentFocusX();
    const double focusY = CurrentFocusY();
    UINT sourceWidth{};
    UINT sourceHeight{};
    if (g_selectedVideo < g_videos.size() &&
        _wcsicmp(g_videos[g_selectedVideo].path.c_str(), g_videoPath.c_str()) == 0) {
        auto& selected = g_videos[g_selectedVideo];
        sourceWidth = selected.width;
        sourceHeight = selected.height;
    }
    const std::wstring screenFocus=BuildScreenFocusMap(g_videoPath);
    const ProxySpec proxySpec = IsWallpaperImage(g_videoPath)?ProxySpec{}:BuildProxySpec(g_videoPath, sourceWidth, sourceHeight);
    const bool proxyReady = ProxyFileReady(proxySpec);
    const std::wstring playbackPath = proxyReady ? proxySpec.finalPath : g_videoPath;
    if (proxyReady) TouchProxyFile(playbackPath);

    // 同一视频、同一取景位置已经在桌面播放时，直接复用现有实例。
    // 只有用户移动过取景框，才需要安全地启动新实例并接替旧实例。
    constexpr double kFocusEpsilon = 0.0001;
    if (IsAppliedVideo(g_videoPath) &&
        _wcsicmp(g_player.playbackPath.c_str(), playbackPath.c_str()) == 0 &&
        std::abs(g_player.focusX - focusX) < kFocusEpsilon &&
        std::abs(g_player.focusY - focusY) < kFocusEpsilon &&
        g_player.quality==g_quality && g_player.fullscreenRule==g_fullscreenRule &&
        g_player.screenDevice==g_targetMonitor && g_player.screenFocus==screenFocus) {
        g_player.effects=g_preferences[FocusKey(g_videoPath)].effects;
        if(g_player.channel)g_player.channel->Write(g_player.effects);
        if (g_wallpaperPaused) ToggleWallpaperPause(owner);
        RestoreAppliedSharedPreview(owner);
        SetStatus(owner, L"当前视频已经应用 · 无需重复解码", kSuccess);
        InvalidateRect(owner, nullptr, FALSE);
        return true;
    }
    SetApplying(owner, true);
    StopProxyJob(); // 优先保障用户应用新壁纸的首帧，不让旧转码任务继续争用解码资源。
    // 启动第二条解码管线时先暂停预览，减少首帧初始化阶段的 GPU 解码竞争。
    if (g_previewPlayer) g_previewPlayer->Pause();
    StopEffectPreview();

    const DWORD pid = GetCurrentProcessId();
    const auto generation = ++g_generation;
    // 事件名包含控制器 PID 和递增代数，保证每次应用都有独立的通信对象，
    // 不会误收到上一次播放器遗留的 ready/stop 状态。
    const std::wstring stopName = L"Local\\DWDNative_Stop_" + std::to_wstring(pid) + L"_" + std::to_wstring(generation);
    const std::wstring readyName = L"Local\\DWDNative_Ready_" + std::to_wstring(pid) + L"_" + std::to_wstring(generation);
    const std::wstring previewTextureName = L"Local\\DWDNative_Preview_" +
                                            std::to_wstring(pid) + L"_" +
                                            std::to_wstring(generation);

    PlayerInstance next;
    next.previewTextureName = previewTextureName;
    next.videoPath = g_videoPath;
    next.playbackPath = playbackPath;
    next.focusX = focusX;
    next.focusY = focusY;
    next.quality=g_quality;next.fullscreenRule=g_fullscreenRule;
    next.screenDevice=g_targetMonitor;next.screenFocus=screenFocus;
    next.stopEvent = CreateEventW(nullptr, TRUE, FALSE, stopName.c_str());
    next.readyEvent = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    const std::wstring pauseName = stopName + L"_Pause";
    next.pauseEvent = CreateEventW(nullptr, TRUE, FALSE, pauseName.c_str());
    const std::wstring effectName=stopName+L"_Effects";
    next.channel=std::make_shared<EffectChannel>();
    next.effects=g_preferences[FocusKey(g_videoPath)].effects;
    if(!next.channel->Open(effectName,true)){CloseHandles(next);SetApplying(owner,false);return false;}
    next.channel->Write(next.effects);
    if (!next.stopEvent || !next.readyEvent || !next.pauseEvent) {
        CloseHandles(next);
        SetApplying(owner, false);
        SetStatus(owner, L"播放器通信初始化失败", kError);
        MessageBoxW(owner, L"无法创建播放器通信事件。", L"DWD", MB_ICONERROR);
        ResumePreviewAfterApplyFailure();
        return false;
    }

    const std::wstring playerPath = ExeDirectory() + L"\\DWDNative.exe";
    if (GetFileAttributesW(playerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CloseHandles(next);
        SetApplying(owner, false);
        SetStatus(owner, L"程序文件不完整", kError);
        MessageBoxW(owner, L"找不到 DWDNative.exe。",
                    L"DWD", MB_ICONERROR);
        ResumePreviewAfterApplyFailure();
        return false;
    }

    std::wstring command = Quote(playerPath) + L" --player --video " + Quote(playbackPath) +
        L" --stop-event " + Quote(stopName) + L" --ready-event " + Quote(readyName) +
        L" --pause-event " + Quote(pauseName) +
        L" --fullscreen-rule " + std::to_wstring(g_fullscreenRule) +
        L" --monitor-device " + Quote(g_targetMonitor) +
        L" --screen-focus " + Quote(screenFocus) +
        L" --parent-pid " + std::to_wstring(pid) +
        L" --focus-x " + std::to_wstring(focusX) +
        L" --focus-y " + std::to_wstring(focusY) +
        L" --preview-texture " + Quote(previewTextureName) +
        L" --effects-channel " + Quote(effectName) +
        L" --preview-owner " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(owner));
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(playerPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, ExeDirectory().c_str(), &startup, &processInfo)) {
        CloseHandles(next);
        SetApplying(owner, false);
        SetStatus(owner, L"播放器进程启动失败", kError);
        MessageBoxW(owner, L"播放器进程启动失败。", L"DWD", MB_ICONERROR);
        ResumePreviewAfterApplyFailure();
        return false;
    }
    CloseHandle(processInfo.hThread);
    next.process = processInfo.hProcess;

    if (!WaitForPlayer(owner, next, 8000)) {
        DWORD exitCode{};GetExitCodeProcess(next.process,&exitCode);
        RuntimeLog(L"Wallpaper startup failed; exit code="+std::to_wstring(exitCode));
        // 新播放器失败时只清理 next，当前 g_player 仍然存活，所以桌面不会黑屏。
        StopPlayer(next, false);
        if (IsWindow(owner)) {
            SetApplying(owner, false);
            SetStatus(owner, L"视频初始化失败，原壁纸未改变", kError);
            MessageBoxW(owner, L"视频初始化超时或格式不受支持。原桌面壁纸保持不变。",
                        L"DWD", MB_ICONERROR);
            ResumePreviewAfterApplyFailure();
        }
        return false;
    }

    // 到这里说明新播放器已经成功显示首帧。先停旧实例，再把 next 提升为当前实例。
    StopPlayer(g_player, false);
    g_player = next;
    g_wallpaperPaused = false;
    g_lastWallpaper = g_videoPath;
    g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;
    SaveVideoFocus();
    SaveConfig();
    LayoutControls(owner);
    SetWindowTextW(g_extraButtons[kPauseButton], L"暂停壁纸");
    // 后台已经持有同一视频的完整解码器：释放前台 MFPlay，改为显示后台共享的 720p GPU 纹理。
    // 这是第二阶段降低 4K 内存的核心，动态预览仍然保留，但只解码一次。
    if (g_previewPlayer) g_previewPlayer->Stop();
    const bool sharedPreviewReady = ActivateSharedPreview(owner, g_player.previewTextureName);
    if (!sharedPreviewReady) {
        // 极少数多显卡/驱动不支持命名共享纹理时，保留原 MFPlay 路径作为兼容回退。
        ScheduleCurrentPreview(owner, true);
    }
    SetApplying(owner, false);
    const bool generatingProxy = proxySpec.eligible && !proxyReady &&
                                 !g_forceExit;
    if (generatingProxy) {
        g_pendingProxySource = g_videoPath; g_pendingProxySpec = proxySpec;
        g_lastInteraction = GetTickCount64();
        SetTimer(owner, kProxyIdleTimer, 1000, nullptr);
    } else if (!g_proxyJob.process) {
        g_pendingProxySource.clear(); KillTimer(owner, kProxyIdleTimer);
    }
    SetStatus(owner,
              generatingProxy
                  ? L"壁纸运行中 · 空闲后生成屏幕适配缓存"
                  : (proxyReady
                      ? L"屏幕适配壁纸运行中"
                      : (sharedPreviewReady
                          ? L"动态壁纸运行中"
                          : L"动态壁纸运行中 · 当前驱动使用兼容预览")),
              sharedPreviewReady ? kSuccess : kWarning);
    SetWindowTextW(owner, L"DWD");
    return true;
}

void RecoverWallpaper(HWND owner) {
    KillTimer(owner,kRecoveryTimer);
    if(g_applying || !g_player.process) return;
    const auto found=std::find_if(g_videos.begin(),g_videos.end(),[](const auto& video) {
        return _wcsicmp(video.path.c_str(),g_player.videoPath.c_str())==0;
    });
    if(g_recoveryAttempts>=3 || found==g_videos.end()) {
        StopSharedPreview(owner);StopPlayer(g_player,true);
        LayoutControls(owner);
        SetStatus(owner,L"壁纸未能恢复 · 已还原静态壁纸，请重新选择视频",kError);
        return;
    }
    ++g_recoveryAttempts;
    const auto selected=g_selectedVideo;const auto selectedPath=g_videoPath;
    const bool paused=g_wallpaperPaused;
    g_selectedVideo=static_cast<size_t>(found-g_videos.begin());g_videoPath=found->path;
    SetStatus(owner,L"播放中断 · 正在重新初始化播放器（"+std::to_wstring(g_recoveryAttempts)+L"/3）",kWarning);
    const bool recovered=LaunchPlayer(owner);
    g_selectedVideo=selected;g_videoPath=selectedPath;
    if(recovered) {
        if(paused) ToggleWallpaperPause(owner);
        if(!g_inTray && _wcsicmp(g_player.videoPath.c_str(),selectedPath.c_str())!=0)
            ScheduleCurrentPreview(owner,true);
        SetStatus(owner,L"壁纸播放已恢复",kSuccess);
    } else SetTimer(owner,kRecoveryTimer,3000,nullptr);
}

RECT PreviewRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int margin = Scale(24);
    const int top = Scale(84);
    const int gap = Scale(18);
    const int contentWidth = client.right - margin * 2 - gap;
    // 61.8% / 38.2% 的黄金分割用于主预览与图库；同时限制图库宽度，避免极端窗口尺寸失衡。
    int sidebar = std::clamp(static_cast<int>(contentWidth * (1-g_previewRatio)),
                                   Scale(300), (std::max)(Scale(300),contentWidth-Scale(430)));
    if(g_experienceWindow&&IsWindowVisible(g_experienceWindow))sidebar=(std::max)(sidebar,Scale(366));
    const int width = contentWidth - sidebar;
    const int maxHeight = client.bottom - top - Scale(150);
    const int height = (std::min)(maxHeight, width * 9 / 16 + Scale(58));
    return RECT{margin, top, margin + width, top + height};
}

RECT ThumbnailRect(HWND hwnd, int slot) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const RECT preview = PreviewRect(hwnd);
    const int margin = Scale(24);
    const int gap = Scale(12);
    const int left = preview.right + Scale(18);
    const int areaWidth = client.right - margin - left;
    const int cardWidth = (areaWidth - gap) / 2;
    const int cardHeight = (std::min)(cardWidth * 9 / 16,int((client.bottom-preview.top-Scale(160))/3));
    const int column = slot % 2;
    const int row = slot / 2;
    const int x = left + column * (cardWidth + gap);
    const int y = preview.top + Scale(34) + row * (cardHeight + gap);
    return RECT{x, y, x + cardWidth, y + cardHeight};
}

int HitTestThumbnail(HWND hwnd, POINT point) {
    if (g_settingsOpen && point.x < Scale(430)) return -1;
    for (int slot = 0; slot < kGalleryPageSize; ++slot) {
        const size_t position = g_galleryPage*kGalleryPageSize + slot;
        if (position >= g_galleryIndices.size()) break;
        RECT rect = ThumbnailRect(hwnd, slot);
        if (PtInRect(&rect, point)) return static_cast<int>(g_galleryIndices[position]);
    }
    return -1;
}

void InvalidateThumbnail(HWND hwnd, int index) {
    if (index < 0) return;
    for (int slot = 0; slot < kGalleryPageSize; ++slot) {
        const size_t position = g_galleryPage*kGalleryPageSize + slot;
        if (position >= g_galleryIndices.size()) break;
        if (g_galleryIndices[position] == static_cast<size_t>(index)) {
            RECT rect = ThumbnailRect(hwnd, slot);
            InflateRect(&rect, Scale(2), Scale(2));
            InvalidateRect(hwnd, &rect, FALSE);
            return;
        }
    }
}

double CurrentFocusX() {
    if(g_selectedVideo>=g_videos.size() || _wcsicmp(g_videos[g_selectedVideo].path.c_str(),g_videoPath.c_str())!=0) return 0.5;
    const auto& video=g_videos[g_selectedVideo];
    return ScreenFocus(video.path,CurrentCropDevice(),video.focusX,video.focusY).first;
}

double CurrentFocusY() {
    if(g_selectedVideo>=g_videos.size() || _wcsicmp(g_videos[g_selectedVideo].path.c_str(),g_videoPath.c_str())!=0) return 0.5;
    const auto& video=g_videos[g_selectedVideo];
    return ScreenFocus(video.path,CurrentCropDevice(),video.focusX,video.focusY).second;
}

SIZE CurrentVideoSize() {
    SIZE size{16, 9};
    if (g_selectedVideo < g_videos.size() &&
        g_videos[g_selectedVideo].width && g_videos[g_selectedVideo].height) {
        size = {static_cast<LONG>(g_videos[g_selectedVideo].width),
                static_cast<LONG>(g_videos[g_selectedVideo].height)};
    }
    SIZE native{};
    if (!g_sharedPreviewActive && g_previewPlayer &&
        g_previewPlayer->GetNativeVideoSize(native)) size = native;
    return size;
}

double CurrentScreenAspect() {
    const auto device=CurrentCropDevice();
    for(const auto& screen:g_screens) if(screen.device==device) {
        const int w=screen.rect.right-screen.rect.left,h=screen.rect.bottom-screen.rect.top;
        if(w>0 && h>0) return static_cast<double>(w)/h;
    }
    return 16.0/9.0;
}

RECT DisplayedVideoRect(HWND overlay, const SIZE& source) {
    // MFPlay 默认等比例完整显示（contain），竖视频两侧或横视频上下会出现黑边。
    // 取景框必须映射到真正的视频画面，而不是把黑边也当作源视频坐标。
    RECT client{};
    GetClientRect(overlay, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || source.cx <= 0 || source.cy <= 0) return client;
    const double sourceAspect = static_cast<double>(source.cx) / source.cy;
    const double clientAspect = static_cast<double>(width) / height;
    if (sourceAspect > clientAspect) {
        const int shownHeight = static_cast<int>(width / sourceAspect + 0.5);
        client.top = (height - shownHeight) / 2;
        client.bottom = client.top + shownHeight;
    } else {
        const int shownWidth = static_cast<int>(height * sourceAspect + 0.5);
        client.left = (width - shownWidth) / 2;
        client.right = client.left + shownWidth;
    }
    return client;
}

RECT CropFrameRect(HWND overlay) {
    // 先在“源视频像素坐标”中求出与当前屏幕同宽高比的裁剪框，
    // 再投影到预览窗口中实际显示视频的矩形。
    const SIZE source = CurrentVideoSize();
    const RECT shown = DisplayedVideoRect(overlay, source);
    const double targetAspect = CurrentScreenAspect();
    double cropWidth = source.cx;
    double cropHeight = source.cy;
    if (static_cast<double>(source.cx) / source.cy > targetAspect) {
        cropWidth = source.cy * targetAspect;
    } else {
        cropHeight = source.cx / targetAspect;
    }
    const double left = std::clamp(CurrentFocusX() * source.cx - cropWidth / 2.0,
                                   0.0, source.cx - cropWidth);
    const double top = std::clamp(CurrentFocusY() * source.cy - cropHeight / 2.0,
                                  0.0, source.cy - cropHeight);
    const double scaleX = static_cast<double>(shown.right - shown.left) / source.cx;
    const double scaleY = static_cast<double>(shown.bottom - shown.top) / source.cy;
    return RECT{
        shown.left + static_cast<LONG>(left * scaleX + 0.5),
        shown.top + static_cast<LONG>(top * scaleY + 0.5),
        shown.left + static_cast<LONG>((left + cropWidth) * scaleX + 0.5),
        shown.top + static_cast<LONG>((top + cropHeight) * scaleY + 0.5)
    };
}

RECT CropLabelRect(const RECT& frame) {
    return RECT{frame.left + Scale(8), frame.top + Scale(8),
                (std::min)(frame.right - Scale(8), frame.left + Scale(190)),
                frame.top + Scale(34)};
}

void UpdateCropOverlayRegion(HWND overlay) {
    if (!overlay) return;
    // MFPlay 会持续重绘自己的视频 HWND，普通透明子窗口容易被它覆盖。
    // 因此覆盖层使用一个异形窗口区域：窗口只真实占用边框、拖动点和标签，
    // 中间区域根本不属于覆盖窗口，视频既不会被遮挡，也不用复制画面来合成透明层。
    const RECT frame = CropFrameRect(overlay);
    const int border = Scale(4);
    HRGN region = CreateRectRgn(frame.left, frame.top, frame.right, frame.bottom);
    HRGN inner = CreateRectRgn(frame.left + border, frame.top + border,
                               frame.right - border, frame.bottom - border);
    CombineRgn(region, region, inner, RGN_DIFF);
    DeleteObject(inner);

    const int handle = Scale(12);
    const POINT corners[] = {{frame.left, frame.top}, {frame.right, frame.top},
                             {frame.left, frame.bottom}, {frame.right, frame.bottom}};
    for (const POINT& corner : corners) {
        HRGN handleRegion = CreateRectRgn(corner.x - handle / 2, corner.y - handle / 2,
                                          corner.x + handle / 2, corner.y + handle / 2);
        CombineRgn(region, region, handleRegion, RGN_OR);
        DeleteObject(handleRegion);
    }
    const RECT label = CropLabelRect(frame);
    HRGN labelRegion = CreateRectRgn(label.left, label.top, label.right, label.bottom);
    CombineRgn(region, region, labelRegion, RGN_OR);
    DeleteObject(labelRegion);
    SetWindowRgn(overlay, region, TRUE);
}

void SetCropFocusFromPoint(HWND overlay, POINT point) {
    if (g_selectedVideo >= g_videos.size()) return;
    const SIZE source = CurrentVideoSize();
    const RECT shown = DisplayedVideoRect(overlay, source);
    const int width = shown.right - shown.left;
    const int height = shown.bottom - shown.top;
    if (width <= 0 || height <= 0) return;
    auto& video = g_videos[g_selectedVideo];
    // 鼠标坐标转成 0~1 后与分辨率无关，可直接通过命令行传给后台播放器。
    // 各屏焦点分别存储；同一解码帧可以在不同输出上采用不同裁剪。
    g_savedFocus[FocusKey(video.path)+L"_"+FocusKey(CurrentCropDevice())]={
        std::clamp(static_cast<double>(point.x-shown.left)/width,0.0,1.0),
        std::clamp(static_cast<double>(point.y-shown.top)/height,0.0,1.0)};
    if (!g_sharedPreviewActive && g_previewPlayer) g_previewPlayer->UpdateVideo();
    UpdateCropOverlayRegion(overlay);
    InvalidateRect(overlay, nullptr, FALSE);
}

LRESULT CALLBACK CropOverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return TRUE;
    case WM_LBUTTONDOWN: {
        g_cropDragging = true;
        // 捕获鼠标后，即使拖动过程中指针离开那几像素宽的异形边框，仍会继续收到移动消息。
        SetCapture(hwnd);
        SetCropFocusFromPoint(hwnd, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_cropDragging && (wParam & MK_LBUTTON)) {
            SetCropFocusFromPoint(hwnd, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_cropDragging) {
            SetCropFocusFromPoint(hwnd, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            g_cropDragging = false;
            ReleaseCapture();
            SaveVideoFocus();
            SetStatus(g_mainWindow, L"取景位置已保存 · 点击应用后更新桌面", kSuccess);
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_cropDragging = false;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT frame = CropFrameRect(hwnd);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH accentBrush = CreateSolidBrush(kAccent);
        FillRect(dc, &client, accentBrush);
        DeleteObject(accentBrush);

        const int handle = Scale(8);
        HBRUSH handleBrush = CreateSolidBrush(kAccent);
        RECT handles[] = {
            {frame.left - handle / 2, frame.top - handle / 2, frame.left + handle / 2, frame.top + handle / 2},
            {frame.right - handle / 2, frame.top - handle / 2, frame.right + handle / 2, frame.top + handle / 2},
            {frame.left - handle / 2, frame.bottom - handle / 2, frame.left + handle / 2, frame.bottom + handle / 2},
            {frame.right - handle / 2, frame.bottom - handle / 2, frame.right + handle / 2, frame.bottom + handle / 2}
        };
        for (const RECT& handleRect : handles) FillRect(dc, &handleRect, handleBrush);
        DeleteObject(handleBrush);

        RECT label = CropLabelRect(frame);
        FillRounded(dc, label, Scale(8), kControlSurface);
        DrawLabel(dc, L"拖动取景框选择主体位置", label, g_smallFont, kText,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

#include "SettingsControls.inl"

std::wstring SettingActionLabel(int id) {
    switch(id) {
    case kAutoRestore:return g_autoRestore?L"启动时恢复上次壁纸：开":L"启动时恢复上次壁纸：关";
    case kBallToggle:return g_showMemoryBall?L"内存悬浮球：显示":L"内存悬浮球：隐藏";
    case kUsageToggle:return g_usageEnabled?L"使用时长统计：开":L"使用时长统计：关";
    case kPauseButton:return g_wallpaperPaused?L"继续播放":L"暂停壁纸";
    case kCropButton:return g_cropEditing?L"完成取景":L"调整取景";
    default:return {};
    }
}

void LayoutControls(HWND hwnd) {
    if (!g_settingsButton || !g_previewWindow || !g_cropOverlay) return;
    for(const auto& [id,button]:g_extraButtons) {
        if(IsSettingCombo(id))continue;
        const auto label=SettingActionLabel(id);
        if(!label.empty()) {wchar_t current[160]{};GetWindowTextW(button,current,160);
            if(label!=current) SetWindowTextW(button,label.c_str());}
    }
    SyncSettingCombos();
    RECT client{}; GetClientRect(hwnd, &client);
    const RECT preview = PreviewRect(hwnd);
    const int availableWidth=preview.right-preview.left-2,availableHeight=preview.bottom-preview.top-Scale(58)-1;
    const int videoHeight=(std::min)(availableHeight,availableWidth*9/16),videoWidth=videoHeight*16/9;
    const int videoX=preview.left+1+(availableWidth-videoWidth)/2,videoY=preview.top+1+(availableHeight-videoHeight)/2;
    for(HWND surface:{g_previewWindow,g_sharedPreviewWindow,g_effectPreviewWindow})if(surface)
        PlaceStable(surface,videoX,videoY,videoWidth,videoHeight,(GetWindowLongPtrW(surface,GWL_STYLE)&WS_VISIBLE)!=0);
    POINT origin{videoX,videoY}; ClientToScreen(hwnd, &origin);
    SetWindowPos(g_cropOverlay, HWND_TOP, origin.x, origin.y,videoWidth,videoHeight,SWP_NOACTIVATE);
    UpdateCropOverlayRegion(g_cropOverlay);
    const bool showPreview = g_previewActive && !g_settingsOpen && !g_inTray && !IsIconic(hwnd);
    ShowWindow(g_previewWindow, showPreview && !g_sharedPreviewActive && !g_effectPreview.process ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_sharedPreviewWindow, showPreview && g_sharedPreviewActive ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_effectPreviewWindow,showPreview && g_effectPreview.process?SW_SHOWNA:SW_HIDE);
    ShowWindow(g_cropOverlay, showPreview && g_cropEditing ? SW_SHOWNA : SW_HIDE);
    const auto place = [](HWND control, int x, int y, int w, int h, bool show) {
        if (!control) return;
        PlaceStable(control,Scale(x),Scale(y),Scale(w),Scale(h),show);
    };
    const int width = MulDiv(client.right,96,g_dpi), height = MulDiv(client.bottom,96,g_dpi);
    const auto extra = [&](int id,int x,int y,int w,int h,bool show) {
        const auto found=g_extraButtons.find(id);
        if(found!=g_extraButtons.end()) place(found->second,x,y,w,h,show);
    };
    SetWindowPos(g_applyButton, HWND_TOP, preview.right-Scale(178), preview.bottom-Scale(50),
        Scale(164),Scale(40), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    place(g_settingsButton,24,height-57,42,40,true);
    place(g_refreshButton,width-140,20,116,30,true);
    place(g_searchEdit,234,21,width-555,30,!g_settingsOpen);
    place(g_sortCombo,width-306,21,154,240,!g_settingsOpen);
    const int px=MulDiv(preview.left,96,g_dpi),py=MulDiv(preview.bottom,96,g_dpi)+38;
    extra(kPauseButton,px,py,90,34,!g_settingsOpen);
    extra(kStopButton,0,0,0,0,false);
    extra(kCropButton,px+98,py,88,34,!g_settingsOpen);
    extra(kEffectsButton,px+194,py,88,34,!g_settingsOpen);
    extra(kFavorite,px+290,py,70,34,!g_settingsOpen);
    extra(kMoreButton,px+368,py,60,34,!g_settingsOpen);
    place(g_usageButton,78,height-57,100,40,!g_settingsOpen || g_settingsTab==4);
    extra(kLibraryFilter,MulDiv(preview.right,96,g_dpi)+100,84,100,26,!g_settingsOpen);
    place(g_seekBar,px+2,MulDiv(preview.bottom,96,g_dpi)+6,
        MulDiv(preview.right-preview.left,96,g_dpi)-4,24,!g_settingsOpen && !IsWallpaperImage(g_videoPath));
    SetWindowTextW(g_extraButtons[kLibraryFilter],g_libraryFilter==1?L"收藏 ▾":g_libraryFilter==2?L"最近 ▾":L"全部 ▾");
    SetWindowTextW(g_extraButtons[kFavorite],g_preferences[FocusKey(g_videoPath)].favorite?L"★ 已收藏":L"☆ 收藏");
    const bool library=g_settingsOpen && g_settingsTab==0;
    const bool playback=g_settingsOpen && g_settingsTab==1;
    const bool appearance=g_settingsOpen && g_settingsTab==2;
    const bool cache=g_settingsOpen && g_settingsTab==3;
    const bool usage=g_settingsOpen && g_settingsTab==4;
    for(int n=0;n<5;++n) extra(kTabFirst+n,24+n*78,78,70,34,g_settingsOpen);
    place(g_chooseFolderButton,24,240,382,38,library);
    extra(kAutoRestore,24,320,382,38,library);
    extra(kCropMonitor,24,386,382,240,library);
    extra(kResetCropButton,24,438,382,38,library);
    extra(kQuality,24,164,382,240,playback);
    extra(kFullscreenRule,24,300,382,240,playback);
    extra(kScreenTarget,24,434,382,240,playback);
    extra(kThemeFirst,24,174,382,240,appearance);
    place(g_chooseBackgroundButton,24,304,382,38,appearance && g_theme==3);
    extra(kBallToggle,24,438,382,38,appearance);
    place(g_chooseCacheButton,24,238,382,38,cache);
    extra(kCacheLimit,24,312,382,240,cache);
    extra(kClearThumbnails,24,386,186,38,cache);
    extra(kClearProxies,220,386,186,38,cache);
    place(g_activeModeButton,24,174,186,40,usage);
    place(g_viewingModeButton,220,174,186,40,usage);
    extra(kUsageToggle,24,320,382,40,usage);
    if(usage) place(g_usageButton,24,438,382,40,true);
    const auto last=ThumbnailRect(hwnd,kGalleryPageSize-1);
    const int gx=MulDiv(preview.right,96,g_dpi)+18, gy=MulDiv(last.bottom,96,g_dpi)+8;
    extra(kPreviousPage,gx,gy,58,30,true);
    extra(kNextPage,width-82,gy,58,30,true);
    const size_t pages=(std::max<size_t>)(1,(g_galleryIndices.size()+kGalleryPageSize-1)/kGalleryPageSize);
    EnableWindow(g_extraButtons[kPreviousPage],!g_applying && g_galleryPage>0);
    EnableWindow(g_extraButtons[kNextPage],!g_applying && g_galleryPage+1<pages);
    EnableWindow(g_extraButtons[kPauseButton],g_player.process && !g_applying);
    EnableWindow(g_extraButtons[kStopButton],g_player.process && !g_applying);
    EnableWindow(g_extraButtons[kCropButton],!g_applying && !g_videoPath.empty());
    if(g_experienceWindow && IsWindowVisible(g_experienceWindow)){
        for(HWND control:{g_sortCombo,g_refreshButton,g_extraButtons[kLibraryFilter],g_extraButtons[kPreviousPage],g_extraButtons[kNextPage]})ShowWindow(control,SW_HIDE);
        const int panelLeft=preview.right+Scale(18);
        MoveWindow(g_searchEdit,Scale(234),Scale(21),(std::max)(Scale(100),panelLeft-Scale(258)),Scale(30),TRUE);
        PlaceStable(g_experienceWindow,panelLeft,0,client.right-panelLeft,client.bottom,true);
    }
}

LRESULT CALLBACK ButtonSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
    if (message == WM_MOUSEMOVE && g_hoverButton != hwnd) {
        g_hoverButton = hwnd;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (message == WM_MOUSELEAVE) {
        if (g_hoverButton == hwnd) g_hoverButton = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ButtonSubclass, 1);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    const RECT rect=item->rcItem;
    const int id=static_cast<int>(item->CtlID);
    const bool primary=id==kApplyButton||id==2032;
    const bool settings=id==kSettingsButton;
    const bool selected=(id>=kTabFirst && id<kTabFirst+5 && id-kTabFirst==g_settingsTab) ||
        (id>=kThemeFirst && id<kThemeFirst+4 && id-kThemeFirst==g_theme) ||
        (id==kActiveModeButton && g_usageMode==UsageMode::Active) ||
        (id==kViewingModeButton && g_usageMode==UsageMode::Viewing) ||
        (id==kCropButton && g_cropEditing);
    const bool disabled=(item->itemState&ODS_DISABLED)!=0;
    const bool pressed=(item->itemState&ODS_SELECTED)!=0;
    const bool hovered=g_hoverButton==item->hwndItem;
    const bool quiet=id==kPauseButton || id==kCropButton || id==kEffectsButton || id==kFavorite || id==kMoreButton || id==kUsageButton;
    COLORREF fill=primary?kAccent:(quiet?kControlSurface:kPanel), border=selected?kAccent:(quiet?kControlSurface:kBorder);
    COLORREF text=primary?RGB(255,255,255):kText;
    if(disabled) {fill=kControlSurface; text=kMuted;}
    else if(pressed || hovered) fill=primary?kAccentHover:kPanelHover;
    HBRUSH background=CreateSolidBrush(g_settingsOpen?kDrawerSurface:kControlSurface);
    FillRect(item->hDC,&rect,background); DeleteObject(background);
    RECT button{rect.left+1,rect.top+1,rect.right-1,rect.bottom-1};
    FillRounded(item->hDC,button,Scale(12),fill); StrokeRounded(item->hDC,button,Scale(12),border);
    if(settings) DrawCogIcon(item->hDC,button,text);
    else {
        wchar_t label[160]{}; GetWindowTextW(item->hwndItem,label,160);
        DrawLabel(item->hDC,label,button,primary ? g_buttonFont : g_bodyFont,text,
                  DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    }
    if((item->itemState&ODS_FOCUS) && !disabled) {RECT focus=button;InflateRect(&focus,-Scale(3),-Scale(3));StrokeRounded(item->hDC,focus,Scale(6),kAccent);}
}

HWND MakeActionButton(HWND owner,int id,const wchar_t* label) {
    HWND button=CreateWindowW(L"BUTTON",label,WS_CHILD|WS_TABSTOP|BS_OWNERDRAW,
        0,0,0,0,owner,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    SetWindowSubclass(button,ButtonSubclass,1,0);
    SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(g_buttonFont),TRUE);
    g_extraButtons[id]=button;
    return button;
}

void DrawBitmapCover(HDC dc, HBITMAP bitmap, const RECT& rect) {
    if (!bitmap || !RectVisible(dc, &rect)) return;
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    if (!info.bmWidth || !info.bmHeight) return;
    const int targetWidth = rect.right - rect.left;
    const int targetHeight = rect.bottom - rect.top;
    if (targetWidth <= 0 || targetHeight <= 0) return;
    const double sourceAspect = static_cast<double>(info.bmWidth) / info.bmHeight;
    const double targetAspect = static_cast<double>(targetWidth) / targetHeight;
    int sourceX = 0, sourceY = 0, sourceWidth = info.bmWidth, sourceHeight = info.bmHeight;
    if (sourceAspect > targetAspect) {
        sourceWidth = static_cast<int>(info.bmHeight * targetAspect);
        sourceX = (info.bmWidth - sourceWidth) / 2;
    } else {
        sourceHeight = static_cast<int>(info.bmWidth / targetAspect);
        sourceY = (info.bmHeight - sourceHeight) / 2;
    }
    HDC memory = CreateCompatibleDC(dc);
    if (!memory) return;
    const RECT crop{sourceX, sourceY, sourceX + sourceWidth, sourceY + sourceHeight};
    HBITMAP scaled = g_scaledBitmaps.Get(dc, bitmap, targetWidth, targetHeight, crop);
    HGDIOBJ oldBitmap = SelectObject(memory, scaled ? scaled : bitmap);
    if (scaled) BitBlt(dc, rect.left, rect.top, targetWidth, targetHeight, memory, 0, 0, SRCCOPY);
    else {
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, rect.left, rect.top, targetWidth, targetHeight,
                   memory, sourceX, sourceY, sourceWidth, sourceHeight, SRCCOPY);
    }
    SelectObject(memory, oldBitmap);
    DeleteDC(memory);
}

std::wstring ThumbnailResolutionText(const VideoItem& video) {
    if (!video.width || !video.height) return {};
    return std::to_wstring(video.width) + L" × " + std::to_wstring(video.height);
}

void DrawThumbnail(HDC dc, HWND hwnd, int slot, size_t index) {
    RECT rect = ThumbnailRect(hwnd, slot);
    if (!RectVisible(dc, &rect)) return;
    const int savedDc = SaveDC(dc);
    const bool selected = index == g_selectedVideo;
    const bool hovered = static_cast<int>(index) == g_hoverThumbnail;
    const bool applied = IsAppliedVideo(g_videos[index].path);
    HRGN clip = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                                   Scale(12), Scale(12));
    ExtSelectClipRgn(dc, clip, RGN_AND);
    FillRounded(dc, rect, Scale(12), hovered ? kPanelHover : kPanel);
    const int labelHeight = Scale(30);
    RECT imageRect{rect.left, rect.top, rect.right, rect.bottom};
    if (g_videos[index].thumbnail) {
        DrawBitmapCover(dc, g_videos[index].thumbnail, imageRect);
    } else {
        RECT loading = rect;
        DrawLabel(dc, L"正在生成封面…", loading, g_smallFont, kMuted,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    // 默认保持封面干净；鼠标悬停时才显示缓存的真实分辨率。
    const std::wstring resolutionText = ThumbnailResolutionText(g_videos[index]);
    if (hovered && !resolutionText.empty()) {
        RECT resolution{rect.right - Scale(112), rect.top + Scale(8),
                        rect.right - Scale(8), rect.top + Scale(32)};
        FillRounded(dc, resolution, Scale(7), kControlSurface);
        StrokeRounded(dc, resolution, Scale(7), RGB(82, 96, 120));
        DrawLabel(dc, resolutionText, resolution, g_smallFont, kText,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    if (applied && hovered) {
        RECT appliedBadge{rect.left + Scale(8), rect.top + Scale(8),
                          rect.left + Scale(68), rect.top + Scale(32)};
        FillRounded(dc, appliedBadge, Scale(7), RGB(25, 72, 59));
        StrokeRounded(dc, appliedBadge, Scale(7), RGB(55, 154, 119));
        DrawLabel(dc, L"已应用", appliedBadge, g_smallFont, kSuccess,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    RECT label{rect.left, rect.bottom - labelHeight, rect.right, rect.bottom};
    HBRUSH labelBrush = CreateSolidBrush(kControlSurface);
    FillRect(dc, &label, labelBrush);
    DeleteObject(labelBrush);
    RECT labelText{label.left + Scale(9), label.top, label.right - Scale(8), label.bottom};
    if(g_preferences[FocusKey(g_videos[index].path)].favorite){
        RECT star{label.right-Scale(26),label.top,label.right-Scale(6),label.bottom};
        DrawLabel(dc,L"★",star,g_smallFont,kAccent);labelText.right=star.left;}
    DrawLabel(dc, g_videos[index].title, labelText, g_smallFont, kText,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RestoreDC(dc, savedDc);
    DeleteObject(clip);
    StrokeRounded(dc, rect, Scale(12), selected ? kAccent : (hovered ? RGB(91, 105, 130) : kBorder));
    if (selected) {
        RECT inner = rect;
        InflateRect(&inner, -1, -1);
        StrokeRounded(dc, inner, Scale(11), kAccent);
    }
}

void PaintWindow(HWND hwnd, HDC dc) {
    RECT client{};
    GetClientRect(hwnd, &client);
    HBRUSH background = CreateSolidBrush(g_backgroundColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    // 顶部与底部用轻微不同的表面色形成层次，但不引入渐变和透明合成开销。
    RECT headerSurface{0, 0, client.right, Scale(66)};
    HBRUSH headerBrush = CreateSolidBrush(kControlSurface);
    FillRect(dc, &headerSurface, headerBrush);
    DeleteObject(headerBrush);
    HPEN headerDivider = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldHeaderPen = SelectObject(dc, headerDivider);
    MoveToEx(dc, 0, headerSurface.bottom, nullptr);
    LineTo(dc, client.right, headerSurface.bottom);
    SelectObject(dc, oldHeaderPen);
    DeleteObject(headerDivider);

    RECT footerSurface{0, client.bottom - Scale(68), client.right, client.bottom};
    HBRUSH footerBrush = CreateSolidBrush(kControlSurface);
    FillRect(dc, &footerSurface, footerBrush);
    DeleteObject(footerBrush);
    HPEN footerDivider = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldFooterPen = SelectObject(dc, footerDivider);
    MoveToEx(dc, 0, footerSurface.top, nullptr);
    LineTo(dc, client.right, footerSurface.top);
    SelectObject(dc, oldFooterPen);
    DeleteObject(footerDivider);

    RECT title{Scale(24), Scale(12), client.right - Scale(24), Scale(56)};
    DrawLabel(dc, L"动态壁纸库", title, g_titleFont, kText);
    std::wstring subtitle;
    RECT subtitleRect{Scale(214), Scale(15), client.right - Scale(164), Scale(53)};
    DrawLabel(dc, subtitle, subtitleRect, g_subtitleFont, kMuted,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT preview = PreviewRect(hwnd);
    FillRounded(dc, preview, Scale(16), kControlSurface);
    StrokeRounded(dc, preview, Scale(16), kBorder);
    const int controlBarHeight = Scale(58);
    RECT videoArea{preview.left + 1, preview.top + 1, preview.right - 1,
                   preview.bottom - controlBarHeight};
    if (g_videos.empty()) {
        RECT placeholder = videoArea;
        DrawLabel(dc, L"选择壁纸文件夹后，将在这里预览第一个视频", placeholder,
                  g_bodyFont, kMuted, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    } else if ((!g_previewActive || g_settingsOpen) && g_selectedVideo < g_videos.size() &&
               g_videos[g_selectedVideo].thumbnail) {
        DrawBitmapCover(dc, g_videos[g_selectedVideo].thumbnail, videoArea);
    }
    if (!g_videos.empty() && g_selectedVideo < g_videos.size()) {
        RECT currentTitle{preview.left + Scale(18), preview.bottom - controlBarHeight,
                          preview.right - Scale(200), preview.bottom};
        const auto& video=g_videos[g_selectedVideo];
        const auto state=IsAppliedVideo(video.path)?L"桌面正在使用 · ":L"仅预览 · ";
        DrawLabel(dc, state+video.title, currentTitle, g_bodyFont, kText,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    RECT galleryHeading{preview.right + Scale(18), preview.top,
                        client.right - Scale(24), preview.top + Scale(26)};
    DrawLabel(dc, L"选择壁纸", galleryHeading, g_bodyFont, kText,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    for (int slot = 0; slot < kGalleryPageSize; ++slot) {
        const size_t position = g_galleryPage*kGalleryPageSize + slot;
        if(position>=g_galleryIndices.size()) break;
        DrawThumbnail(dc,hwnd,slot,g_galleryIndices[position]);
    }
    const size_t pages=(std::max<size_t>)(1,(g_galleryIndices.size()+kGalleryPageSize-1)/kGalleryPageSize);
    const RECT lastCard=ThumbnailRect(hwnd,kGalleryPageSize-1);
    RECT page{preview.right+Scale(88),lastCard.bottom+Scale(8),
              client.right-Scale(90),lastCard.bottom+Scale(38)};
    const std::wstring footerText=std::to_wstring(g_galleryPage+1)+L" / "+
        std::to_wstring(pages)+L"  ·  "+std::to_wstring(g_galleryIndices.size())+L" 个素材";
    DrawLabel(dc,footerText,page,g_smallFont,kMuted,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    RECT status{Scale(200),client.bottom-Scale(57),client.right-Scale(24),client.bottom-Scale(17)};
    DrawLabel(dc,g_statusText,status,g_smallFont,g_statusColor,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    if(g_galleryIndices.empty() && !g_videos.empty()) {
        RECT empty{preview.right+Scale(18),preview.top+Scale(40),client.right-Scale(24),preview.top+Scale(110)};
        DrawLabel(dc,L"没有匹配的视频",empty,g_bodyFont,kMuted,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    }

    if (g_settingsOpen) {
        RECT drawer{0,0,Scale(430),client.bottom};
        HBRUSH brush=CreateSolidBrush(kDrawerSurface);FillRect(dc,&drawer,brush);DeleteObject(brush);
        RECT line{drawer.right-1,0,drawer.right,drawer.bottom};
        brush=CreateSolidBrush(kBorder);FillRect(dc,&line,brush);DeleteObject(brush);
        RECT heading{Scale(24),Scale(22),Scale(400),Scale(62)};
        DrawLabel(dc,L"设置",heading,g_titleFont,kText);
        const auto info=[&](int top,int bottom,const std::wstring& text,bool card=false) {
            RECT area{Scale(24),Scale(top),Scale(406),Scale(bottom)};
            if(card) {FillRounded(dc,area,Scale(12),kPanel);InflateRect(&area,-Scale(16),-Scale(12));}
            DrawLabel(dc,text,area,g_smallFont,kMuted,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
        };
        if(g_settingsTab==0) {
            info(132,224,L"壁纸文件夹\n\n"+(g_wallpaperFolder.empty()?L"尚未选择":g_wallpaperFolder),true);
            info(292,314,L"自动恢复");
            info(362,382,L"取景位置按视频、屏幕分别记忆。");
            info(494,560,L"取景框只在点击“调整取景”后出现。更改主体位置后，再点“应用到桌面”。");
        } else if(g_settingsTab==1) {
            info(132,166,L"播放画质");
            info(210,266,QualityDescription());
            info(274,298,L"全屏时的播放行为");
            info(346,402,FullscreenDescription());
            info(408,432,L"应用屏幕");
            info(480,548,L"修改后点击“应用到桌面”生效。优化视频会占用磁盘缓存，仅转换比例合适的素材，不拉伸原片。");
        } else if(g_settingsTab==2) {
            info(132,160,L"应用主题");
            info(224,292,ThemeDescription());
            info(492,546,L"悬浮球是可选诊断入口；隐藏后停止内存采样。拖动改变位置，双击打开 DWD。");
        } else if(g_settingsTab==3) {
            info(132,222,L"缓存文件夹\n\n"+g_cacheFolder,true);
            info(284,306,L"已用约 "+FormatMemoryValue(static_cast<SIZE_T>(g_cacheUsageBytes)));
            info(452,532,L"只清理 DWD 生成的封面和优化视频，不删原文件。当前壁纸使用中的缓存会保留；很小的分辨率元数据也保留。");
        } else {
            info(132,160,L"两种口径同时记录，切换只改变排行展示");
            info(234,294,L"活跃：60 秒没有输入后暂停。\n观看：前台持续累计，锁屏、睡眠时暂停。");
            info(378,420,L"只保存应用名称、路径和累计时长，不记录键鼠内容，也不截屏。");
            info(502,548,L"可以查看 DWD 已记录的历史日期；程序未运行期间和安装前的时长不会伪造。");
        }
    }
}

#include "ControllerExperience.inl"
#include "SettingsTooltips.inl"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Explorer 崩溃/重启会清空通知区。TaskbarCreated 是系统约定的广播消息，
    // 收到后需要在窗口仍隐藏的情况下重新注册托盘图标。
    if (g_taskbarCreated && message == g_taskbarCreated) {
        g_trayIconVisible = false;
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd,TRUE);
        SetTimer(hwnd,kExperienceTimer,500,nullptr);
        // Win32 控件在这里创建，具体位置统一交给 LayoutControls；这样 WM_SIZE 和
        // WM_DPICHANGED 可以复用同一套布局逻辑。
        g_mainWindow = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
        CreateFonts();
        g_previewWindow = CreateWindowW(L"STATIC", nullptr, WS_CHILD | SS_BLACKRECT,
                                        0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        g_sharedPreviewWindow = CreateWindowW(L"DWDSharedPreviewSurface", nullptr, WS_CHILD,
                                        0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        // 取景框是主窗口拥有的工具型弹出窗口：保持在 MFPlay 视频上方，
        // 但不抢键盘焦点，也不会单独出现在任务栏。
        g_cropOverlay = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                        kCropOverlayClass, nullptr, WS_POPUP,
                                        0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
        g_settingsButton = CreateWindowW(L"BUTTON", L"设置",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSettingsButton)),
                                         nullptr, nullptr);
        g_applyButton = CreateWindowW(L"BUTTON", L"应用到桌面",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kApplyButton)),
                                      nullptr, nullptr);
        g_chooseFolderButton = CreateWindowW(L"BUTTON", L"选择壁纸文件夹…",
                                             WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                             0, 0, 0, 0, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChooseFolderButton)),
                                             nullptr, nullptr);
        g_chooseCacheButton = CreateWindowW(L"BUTTON", L"选择缓存文件夹…",
                                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                            0, 0, 0, 0, hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChooseCacheButton)),
                                            nullptr, nullptr);
        g_chooseBackgroundButton = CreateWindowW(L"BUTTON", L"修改应用背景颜色…",
                                                 WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                                 0, 0, 0, 0, hwnd,
                                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChooseBackgroundButton)),
                                                 nullptr, nullptr);
        g_usageButton = CreateWindowW(L"BUTTON", L"使用时长",
                                      WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUsageButton)),
                                      nullptr, nullptr);
        g_activeModeButton = CreateWindowW(L"BUTTON", L"活跃模式",
                                           WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kActiveModeButton)),
                                           nullptr, nullptr);
        g_viewingModeButton = CreateWindowW(L"BUTTON", L"观看模式",
                                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                            0, 0, 0, 0, hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kViewingModeButton)),
                                            nullptr, nullptr);
        g_refreshButton = CreateWindowW(L"BUTTON", L"刷新素材",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        0, 0, 0, 0, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)),
                                        nullptr, nullptr);
        for(const auto& [id,label] : std::initializer_list<std::pair<int,const wchar_t*>>{
            {kPauseButton,L"暂停壁纸"},{kStopButton,L"恢复静态壁纸"},{kCropButton,L"调整取景"},
            {kResetCropButton,L"当前视频取景恢复居中"},{kPreviousPage,L"上一页"},{kNextPage,L"下一页"},
            {kAutoRestore,L""},{kBallToggle,L""},{kUsageToggle,L""},{kClearThumbnails,L"清理封面"},
            {kClearProxies,L"清理优化视频"},
            {kFavorite,L"☆ 收藏"},{kLibraryFilter,L"全部 ▾"},{kEffectsButton,L"画面效果"},{kMoreButton,L"更多"},
            {kTabFirst,L"壁纸"},{kTabFirst+1,L"播放"},{kTabFirst+2,L"外观"},
            {kTabFirst+3,L"缓存"},{kTabFirst+4,L"统计"}
        }) MakeActionButton(hwnd,id,label);
        for(int id:{kQuality,kFullscreenRule,kScreenTarget,kCropMonitor,kCacheLimit,kThemeFirst})
            g_extraButtons[id]=MakeThemeCombo(hwnd,id,g_bodyFont);
        const auto choices=[&](int id,std::initializer_list<const wchar_t*> labels){
            for(const auto* label:labels)SendMessageW(g_extraButtons[id],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));};
        choices(kQuality,{L"均衡（推荐）",L"原视频",L"节能（最高 1080p / 30 帧）"});
        choices(kFullscreenRule,{L"任一屏幕有全屏应用时暂停",L"全部目标屏幕被全屏覆盖时暂停",L"不因全屏应用暂停"});
        choices(kThemeFirst,{L"深色",L"浅色",L"跟随系统",L"自定义背景颜色…"});
        choices(kCacheLimit,{L"优化视频上限：512 MB",L"优化视频上限：1024 MB",L"优化视频上限：2048 MB",L"优化视频上限：4096 MB"});
        g_seekBar=CreateWindowW(TRACKBAR_CLASSW,L"预览进度",WS_CHILD|WS_TABSTOP|TBS_NOTICKS,
            0,0,0,0,hwnd,reinterpret_cast<HMENU>(INT_PTR(kSeekBar)),nullptr,nullptr);
        SetWindowSubclass(g_seekBar,SliderPaintProc,1,0);
        SendMessageW(g_seekBar,TBM_SETRANGE,TRUE,MAKELPARAM(0,1000));
        g_searchEdit=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
            0,0,0,0,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEdit)),nullptr,nullptr);
        SendMessageW(g_searchEdit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"搜索壁纸名称"));
        SendMessageW(g_searchEdit,WM_SETFONT,reinterpret_cast<WPARAM>(g_bodyFont),TRUE);
        g_sortCombo=MakeThemeCombo(hwnd,kSortCombo,g_smallFont);
        for(const wchar_t* label : {L"名称 A → Z",L"名称 Z → A",L"最近修改"})
            SendMessageW(g_sortCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(g_sortCombo,WM_SETFONT,reinterpret_cast<WPARAM>(g_smallFont),TRUE);
        SendMessageW(g_sortCombo,CB_SETCURSEL,g_sortOrder,0);
        HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                       WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                       CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                       hwnd, nullptr, nullptr, nullptr);
        g_tooltip=tooltip;
        if (tooltip) {
            SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,Scale(450));
            SendMessageW(tooltip,TTM_SETDELAYTIME,TTDT_INITIAL,650);
            SettingTooltips::Register();
        }
        SetWindowSubclass(g_settingsButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_applyButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_chooseFolderButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_chooseCacheButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_chooseBackgroundButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_activeModeButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_viewingModeButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_usageButton, ButtonSubclass, 1, 0);
        SetWindowSubclass(g_refreshButton, ButtonSubclass, 1, 0);
        SendMessageW(g_settingsButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_applyButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_chooseFolderButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_chooseCacheButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_chooseBackgroundButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_activeModeButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_viewingModeButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_usageButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        SendMessageW(g_refreshButton, WM_SETFONT, reinterpret_cast<WPARAM>(g_buttonFont), TRUE);
        EnableWindow(g_applyButton, FALSE);
        EnableWindow(g_refreshButton, FALSE);
        g_previewPlayer = std::make_unique<PreviewPlayer>(
            hwnd, kPreviewReady, kPreviewEnded, kPreviewError);
        g_sharedPreview = std::make_unique<SharedPreviewRenderer>();
        EnumChildWindows(hwnd,[](HWND child,LPARAM)->BOOL {
            SetWindowLongPtrW(child,GWL_STYLE,GetWindowLongPtrW(child,GWL_STYLE)|WS_CLIPSIBLINGS);return TRUE;
        },0);
        // 实时使用时长统计从程序启动即开始，不需要管理员权限。数据每 30 秒批量落盘，
        // 前台切换由系统事件驱动，定时器只负责判定“用户是否仍在操作”。
        g_usageTracker.Start(hwnd, kForegroundChanged, ExeDirectory() + L"\\UsageData");
        g_usageTracker.SetEnabled(g_usageEnabled);
        WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
        SetTimer(hwnd, kUsageTimer, 2000, nullptr);
        RefreshScreenOptions();
        ApplyTheme(hwnd);
        LayoutControls(hwnd);
        ShowWindow(g_previewWindow, SW_HIDE);
        ShowWindow(g_cropOverlay, SW_HIDE);
        return 0;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            MinimizeToTray(hwnd);
            return 0;
        }
        KeepMemoryBallTopmost(g_memoryBall);
        LayoutControls(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        LayoutControls(hwnd);
        return 0;
    case WM_DPICHANGED: {
        // Per-Monitor V2 下，窗口跨屏时系统给出建议矩形。接受它并重建字体，
        // 才能保证 100%/150% 缩放显示器之间移动时尺寸和文字都清晰。
        g_dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        CreateFonts();
        LayoutControls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(hwnd, message, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &point);
            if (point.y < Scale(60)) return HTCAPTION;
        }
        return hit;
    }
    case WM_COMMAND:
        g_lastInteraction = GetTickCount64();
        if(IsSettingCombo(LOWORD(wParam))) {
            // 展开、收起、获得焦点都不是修改；只在确实选中另一项时处理。
            if(HIWORD(wParam)!=CBN_SELCHANGE)return 0;
            const int id=LOWORD(wParam),selected=int(SendMessageW(reinterpret_cast<HWND>(lParam),CB_GETCURSEL,0,0));
            if(selected<0)return 0;
            if(id==kThemeFirst){
                if(selected==3)ChooseBackgroundColor(hwnd);
                else {g_theme=selected;ApplyTheme(hwnd);SaveConfig();}
            } else if(id==kQuality)g_quality=selected;
            else if(id==kFullscreenRule)g_fullscreenRule=selected;
            else if(id==kCacheLimit){g_cacheLimitBytes=(512ULL<<selected)*1024*1024;EnforceProxyCacheLimit();RefreshCacheUsage();}
            else {
                const bool all=id==kScreenTarget;const int index=selected-(all?1:0);
                (all?g_targetMonitor:g_cropMonitor)=index>=0&&index<int(g_screens.size())?g_screens[index].device:L"";
                UpdateCropOverlayRegion(g_cropOverlay);
            }
            SaveConfig();LayoutControls(hwnd);RepaintSettings(hwnd);return 0;
        }
        if(lParam && HIWORD(wParam)!=BN_CLICKED &&
            !(LOWORD(wParam)==kSearchEdit && HIWORD(wParam)==EN_CHANGE) &&
            !(LOWORD(wParam)==kSortCombo && HIWORD(wParam)==CBN_SELCHANGE))return 0;
        if(g_applying && LOWORD(wParam)>=1400)return 0;
        if(ExperienceCommand(hwnd,LOWORD(wParam))){LayoutControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        if (LOWORD(wParam) == kSearchEdit && HIWORD(wParam) == EN_CHANGE) {
            wchar_t text[1024]{};GetWindowTextW(g_searchEdit,text,1024);
            g_searchText=text;SetTimer(hwnd,kSearchTimer,100,nullptr);return 0;
        } else if(LOWORD(wParam)==kSortCombo && HIWORD(wParam)==CBN_SELCHANGE) {
            g_sortOrder=static_cast<int>(SendMessageW(g_sortCombo,CB_GETCURSEL,0,0));
            RebuildGallery(hwnd);SaveConfig();return 0;
        } else if(LOWORD(wParam)>=kTabFirst && LOWORD(wParam)<kTabFirst+5) {
            g_settingsTab=LOWORD(wParam)-kTabFirst;
            if(g_settingsTab==3) RefreshCacheUsage();
            LayoutControls(hwnd);RepaintSettings(hwnd);
            for(int n=0;n<5;++n)InvalidateRect(g_extraButtons[kTabFirst+n],nullptr,FALSE);return 0;
        } else if(LOWORD(wParam)==kPauseButton) ToggleWallpaperPause(hwnd);
        else if(LOWORD(wParam)==kStopButton) StopWallpaper(hwnd);
        else if(LOWORD(wParam)==kCropButton) {
            g_cropEditing=!g_cropEditing;
            SetWindowTextW(g_extraButtons[kCropButton],g_cropEditing?L"完成取景":L"调整取景");
            SaveVideoFocus();LayoutControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);
        } else if(LOWORD(wParam)==kResetCropButton) {
            if(g_selectedVideo<g_videos.size()) {
                g_savedFocus[FocusKey(g_videos[g_selectedVideo].path)+L"_"+FocusKey(CurrentCropDevice())]={0.5,0.5};
                SaveVideoFocus();UpdateCropOverlayRegion(g_cropOverlay);InvalidateRect(g_cropOverlay,nullptr,FALSE);
                SetStatus(hwnd,L"当前视频已恢复居中取景 · 应用后更新桌面",kSuccess);
            }
        } else if(LOWORD(wParam)==kAutoRestore) {g_autoRestore=!g_autoRestore;SaveConfig();InvalidateRect(hwnd,nullptr,FALSE);}
        else if(LOWORD(wParam)==kBallToggle) {
            g_showMemoryBall=!g_showMemoryBall;
            if(g_memoryBall) {
                ShowWindow(g_memoryBall,g_showMemoryBall?SW_SHOWNA:SW_HIDE);
                if(g_showMemoryBall) {SetTimer(g_memoryBall,kMemoryTimer,1000,nullptr);RefreshMemoryBall(g_memoryBall);}
                else KillTimer(g_memoryBall,kMemoryTimer);
            }
            SaveConfig();InvalidateRect(hwnd,nullptr,FALSE);
        } else if(LOWORD(wParam)==kUsageToggle) {
            g_usageEnabled=!g_usageEnabled;g_usageTracker.SetEnabled(g_usageEnabled);SaveConfig();InvalidateRect(hwnd,nullptr,FALSE);
        } else if(LOWORD(wParam)==kClearThumbnails || LOWORD(wParam)==kClearProxies)
            ClearCache(hwnd,LOWORD(wParam)==kClearProxies);
        else if(LOWORD(wParam)==kPreviousPage || LOWORD(wParam)==kNextPage) {
            const size_t pages=(std::max<size_t>)(1,(g_galleryIndices.size()+kGalleryPageSize-1)/kGalleryPageSize);
            if(LOWORD(wParam)==kPreviousPage && g_galleryPage) --g_galleryPage;
            else if(LOWORD(wParam)==kNextPage && g_galleryPage+1<pages) ++g_galleryPage;
            QueueVisibleThumbnails(hwnd);LayoutControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);
        } else if (LOWORD(wParam) == kSettingsButton) SetSettingsOpen(hwnd, !g_settingsOpen);
        else if (LOWORD(wParam) == kChooseFolderButton) ChooseWallpaperFolder(hwnd);
        else if (LOWORD(wParam) == kChooseCacheButton) ChooseCacheFolder(hwnd);
        else if (LOWORD(wParam) == kChooseBackgroundButton) ChooseBackgroundColor(hwnd);
        else if (LOWORD(wParam) == kActiveModeButton ||
                 LOWORD(wParam) == kViewingModeButton) {
            g_usageTracker.Tick();
            g_usageMode = LOWORD(wParam) == kViewingModeButton
                ? UsageMode::Viewing : UsageMode::Active;
            const bool saved = SaveConfig();
            InvalidateRect(g_activeModeButton, nullptr, FALSE);
            InvalidateRect(g_viewingModeButton, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, FALSE);
            g_historyDate.clear();
            if (g_usageWindow) InvalidateRect(g_usageWindow, nullptr, TRUE);
            SetStatus(hwnd,
                      saved
                          ? (g_usageMode == UsageMode::Active
                                 ? L"已切换到活跃模式" : L"已切换到观看模式")
                          : L"统计模式已切换，但配置保存失败",
                      saved ? kSuccess : kError);
        }
        else if (LOWORD(wParam) == kUsageButton) ShowUsageWindow();
        else if (LOWORD(wParam) == kRefreshButton) RefreshVideoLibrary(hwnd);
        else if (LOWORD(wParam) == kApplyButton) {g_recoveryAttempts=0;LaunchPlayer(hwnd);}
        LayoutControls(hwnd);
        RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
        return 0;
    case kLibraryReady: {
        auto snapshot = g_libraryScanner.Take();
        if (snapshot && snapshot->generation == g_libraryGeneration) {
            g_deferredLibrary = std::move(snapshot);
            SetTimer(hwnd, kLibraryApplyTimer, 100, nullptr);
        }
        return 0;
    }
    case kThumbnailReady: {
        // 该指针由缩略图线程 new，UI 线程用 unique_ptr 接管，确保所有分支都释放。
        std::unique_ptr<ThumbnailResult> result(reinterpret_cast<ThumbnailResult*>(lParam));
        if (result && result->generation == g_thumbnailGeneration.load() &&
            result->index < g_videos.size()) {
            if (g_videos[result->index].thumbnail) {
                g_scaledBitmaps.Forget(g_videos[result->index].thumbnail);
                DeleteObject(g_videos[result->index].thumbnail);
            }
            g_videos[result->index].thumbnail = result->bitmap;
            g_videos[result->index].width = result->width;
            g_videos[result->index].height = result->height;
            result->bitmap = nullptr;
            if (g_videos[result->index].thumbnail) {
                g_videos[result->index].thumbnailUse = ++g_thumbnailUse;
                g_residentThumbnails.insert(result->index); TrimThumbnailMemory();
            }
            InvalidateThumbnail(hwnd, static_cast<int>(result->index));
            const auto& video = g_videos[result->index];
            if(result->index==g_selectedVideo)UpdateCoverAccent(hwnd);
            if (IsAppliedVideo(video.path) && !IsWallpaperImage(video.path) && !g_proxyJob.process && !g_applying) {
                const auto spec = BuildProxySpec(video.path, video.width, video.height);
                if (spec.eligible && !ProxyFileReady(spec)) {
                    g_pendingProxySource = video.path; g_pendingProxySpec = spec;
                    SetTimer(hwnd, kProxyIdleTimer, 1000, nullptr);
                }
            }
        }
        if (result && result->bitmap) DeleteObject(result->bitmap);
        return 0;
    }
    case kProxyReady: {
        std::unique_ptr<ProxyResult> result(reinterpret_cast<ProxyResult*>(lParam));
        if (!result || result->generation != g_proxyJob.generation) return 0;

        if (g_proxyJob.waiter.joinable()) g_proxyJob.waiter.join();
        if (g_proxyJob.process) CloseHandle(g_proxyJob.process);
        if (g_proxyJob.stopEvent) CloseHandle(g_proxyJob.stopEvent);
        g_proxyJob.process = nullptr;
        g_proxyJob.stopEvent = nullptr;

        bool completed = result->exitCode == 0;
        if (completed) {
            completed = MoveFileExW(result->temporaryPath.c_str(), result->finalPath.c_str(),
                                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }
        if (!completed) DeleteFileW(result->temporaryPath.c_str());
        g_proxyJob = {};

        if (completed) {
            TouchProxyFile(result->finalPath);
            EnforceProxyCacheLimit(result->finalPath);
            // 用户仍停留在同一张已应用壁纸时，自动走已有的首帧安全交接切换到代理。
            // 若用户正在浏览别的视频，只缓存不打断当前操作，下次应用时自然命中。
            if (IsAppliedVideo(result->sourcePath) &&
                _wcsicmp(g_videoPath.c_str(), result->sourcePath.c_str()) == 0) {
                SetStatus(hwnd, L"2K优化缓存已完成 · 正在无黑屏切换", kSuccess);
                LaunchPlayer(hwnd);
            } else {
                SetStatus(hwnd, L"2K优化缓存已完成 · 下次应用该视频时自动使用", kSuccess);
            }
        } else if (!g_forceExit) {
            SetStatus(hwnd, L"2K优化缓存生成失败 · 当前继续使用原视频", kWarning);
        }
        return 0;
    }
    case kPreviewReady:
        if (static_cast<uint64_t>(wParam) != g_previewRequest) return 0;
        KillTimer(hwnd, kPreviewTimeoutTimer);
        g_previewLoading = false;
        g_previewActive = true;
        PauseThumbnailWork(false);
        if (g_settingsOpen || g_inTray) {
            if (g_previewPlayer) g_previewPlayer->Pause();
        } else {
            ShowWindow(g_previewWindow, SW_SHOWNA);
            ShowWindow(g_cropOverlay, g_cropEditing ? SW_SHOWNA : SW_HIDE);
            SetWindowPos(g_cropOverlay, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetTimer(hwnd, kOverlayTimer, 250, nullptr);
            LayoutControls(hwnd);
            if (g_previewPlayer) g_previewPlayer->UpdateVideo();
        }
        if (g_selectedVideo < g_videos.size()) {
            SetStatus(hwnd, L"正在预览 · " + g_videos[g_selectedVideo].title, kSuccess);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kPreviewEnded:
        if (static_cast<uint64_t>(wParam) == g_previewRequest &&
            g_previewPlayer && g_previewActive && !g_inTray) {
            g_previewPlayer->Restart();
        }
        return 0;
    case kPreviewError:
        // 旧请求即使晚到也不能影响用户刚刚点选的新视频。
        if (static_cast<uint64_t>(wParam) != g_previewRequest) return 0;
        KillTimer(hwnd, kPreviewTimeoutTimer);
        if (g_previewPlayer) g_previewPlayer->CancelPending();
        g_previewActive = false;
        g_previewLoading = false;
        g_previewRequest = 0;
        PauseThumbnailWork(false);
        ShowWindow(g_previewWindow, SW_HIDE);
        ShowWindow(g_cropOverlay, SW_HIDE);
        SetStatus(hwnd,GetFileAttributesW(g_videoPath.c_str())==INVALID_FILE_ATTRIBUTES?
            L"原文件已移动 · 更多 → 重新定位当前素材":L"无法解码此素材 · 可尝试 H.264 MP4 或 JPG/PNG 图片",kError);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case kSharedPreviewFrame:
        if (g_sharedPreviewActive && g_sharedPreview && !g_settingsOpen && !g_inTray &&
            !g_sharedPreview->Render()) {
            // 设备丢失时恢复到兼容预览，避免预览区域永久停在最后一帧。
            StopSharedPreview(hwnd);
            ScheduleCurrentPreview(hwnd, true);
        }
        return 0;
    case kForegroundChanged:
        g_usageTracker.HandleForegroundChanged();
        if (g_usageWindow) InvalidateRect(g_usageWindow, nullptr, FALSE);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT ||
            wParam == WTS_REMOTE_DISCONNECT) {
            g_sessionSuspended=true;g_usageTracker.SetSessionLocked(true);
        } else if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_CONSOLE_CONNECT ||
                   wParam == WTS_REMOTE_CONNECT) {
            g_sessionSuspended=false;g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;g_usageTracker.SetSessionLocked(false);
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {g_sessionSuspended=true;g_usageTracker.SetSuspended(true);}
        else if (wParam == PBT_APMRESUMEAUTOMATIC ||
                 wParam == PBT_APMRESUMESUSPEND) {g_sessionSuspended=false;g_nextPlaylist=GetTickCount64()+ULONGLONG(g_playlistMinutes)*60000;g_usageTracker.SetSuspended(false);}
        return TRUE;
    case kWallpaperError:
        if(g_player.process && static_cast<DWORD>(lParam)==GetProcessId(g_player.process))
            SetTimer(hwnd,kRecoveryTimer,1500,nullptr);
        return 0;
    case kDirectoryChanged:
        g_directorySignature=0;SetTimer(hwnd,kDirectoryTimer,2000,nullptr);return 0;
    case WM_DISPLAYCHANGE:
        RefreshScreenOptions();LayoutControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
    case WM_SETTINGCHANGE:
        if(g_theme==2) ApplyTheme(hwnd);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wParam),kText);
        SetBkColor(reinterpret_cast<HDC>(wParam),kPanel);
        return reinterpret_cast<LRESULT>(g_controlBrush);
    case WM_CTLCOLORSTATIC:
        if(IsCombo(reinterpret_cast<HWND>(lParam)) || reinterpret_cast<HWND>(lParam)==g_seekBar) {
            SetTextColor(reinterpret_cast<HDC>(wParam),kText);
            SetBkColor(reinterpret_cast<HDC>(wParam),kPanel);
            return reinterpret_cast<LRESULT>(g_controlBrush);
        }
        break;
    case WM_TIMER:
        if(wParam==kExperienceTimer){TickExperience(hwnd);return 0;}
        if(wParam==kEffectPreviewTimer){
            if(!g_effectPreview.process){KillTimer(hwnd,kEffectPreviewTimer);return 0;}
            if(WaitForSingleObject(g_effectPreview.readyEvent,0)==WAIT_OBJECT_0){
                KillTimer(hwnd,kEffectPreviewTimer);g_previewLoading=false;g_previewActive=true;PauseThumbnailWork(false);
                LayoutControls(hwnd);SetStatus(hwnd,L"正在预览 · 效果尚未应用到桌面",kSuccess);return 0;
            }
            if(WaitForSingleObject(g_effectPreview.process,0)==WAIT_OBJECT_0||GetTickCount64()>=g_effectPreviewDeadline){
                DWORD code{};GetExitCodeProcess(g_effectPreview.process,&code);RuntimeLog(L"Effect preview failed code="+std::to_wstring(code));
                StopEffectPreview();g_previewLoading=false;g_previewActive=false;PauseThumbnailWork(false);
                SetStatus(hwnd,L"效果预览失败 · 可关闭效果重试，或选择其他格式",kError);LayoutControls(hwnd);
            }return 0;
        }
        if (wParam == kSharedIdleTimer) {
            KillTimer(hwnd, kSharedIdleTimer);
            if (!g_sharedPreviewActive && g_sharedPreview) g_sharedPreview->Stop();
        } else if (wParam == kSearchTimer) {
            KillTimer(hwnd, kSearchTimer); RebuildGallery(hwnd);
        } else if (wParam == kProxyIdleTimer) {
            if (g_pendingProxySource.empty()) { KillTimer(hwnd, kProxyIdleTimer); return 0; }
            // 新任务在停止浏览至少 5 秒、且没有另一条预览解码争用时启动。
            if (!g_applying && !g_previewLoading && (g_inTray || g_sharedPreviewActive) &&
                GetTickCount64() - g_lastInteraction >= 5000) {
                const auto source = g_pendingProxySource; const auto spec = g_pendingProxySpec;
                g_pendingProxySource.clear(); KillTimer(hwnd, kProxyIdleTimer);
                if (IsAppliedVideo(source) && StartProxyJob(hwnd, source, spec))
                    SetStatus(hwnd, L"壁纸运行中 · 正在生成屏幕适配缓存", kMuted);
            }
        } else if (wParam == kLibraryApplyTimer) {
            if (g_applying) return 0;
            KillTimer(hwnd, kLibraryApplyTimer);
            auto snapshot = std::move(g_deferredLibrary);
            if (!snapshot) return 0;
            g_scanPending = false;
            if (snapshot->folder != g_requestedFolder) return 0;
            if (snapshot->error) {
                SetStatus(hwnd, L"无法读取壁纸文件夹，现有视频库保留", kWarning); return 0;
            }
            if (snapshot->automatic && (snapshot->writable || snapshot->signature != g_directorySignature)) {
                g_directorySignature = snapshot->signature;
                SetTimer(hwnd, kDirectoryTimer, 2000, nullptr); return 0;
            }
            KillTimer(hwnd, kDirectoryTimer);
            ApplyLibrarySnapshot(hwnd, *snapshot);
            if (!g_videos.empty() && !snapshot->saveSetting)
                SetStatus(hwnd, L"视频库已刷新 · " + std::to_wstring(g_videos.size()) + L" 个视频", kSuccess);
            if (g_autoApplyPending) {
                g_autoApplyPending = false;
                if (!g_videoPath.empty() && _wcsicmp(g_videoPath.c_str(), g_lastWallpaper.c_str()) == 0)
                    LaunchPlayer(hwnd);
            }
        } else if (wParam == kPreviewTimeoutTimer) {
            KillTimer(hwnd, kPreviewTimeoutTimer);
            if (g_previewLoading) {
                RuntimeLog(L"preview: loading timed out; request=" + std::to_wstring(g_previewRequest));
                if (g_previewPlayer) g_previewPlayer->Stop();
                g_previewRequest = 0;
                g_previewLoading = false;
                g_previewActive = false;
                PauseThumbnailWork(false);
                ShowWindow(g_previewWindow, SW_HIDE);
                SetStatus(hwnd, L"预览加载超时 · 请重新选择视频重试", kError);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if(wParam==kRecoveryTimer) RecoverWallpaper(hwnd);
        else if(wParam==kDirectoryTimer) {
            if (g_applying || g_scanPending || g_wallpaperFolder.empty()) return 0;
            KillTimer(hwnd, kDirectoryTimer);
            g_scanPending = true; g_requestedFolder = g_wallpaperFolder;
            g_libraryGeneration = g_libraryScanner.Request(hwnd, kLibraryReady, g_wallpaperFolder, g_videoPath, false, true);
        } else if (wParam == kUsageTimer) {
            g_processCleanup.Reap();
            if (g_configStore.TakeFailure()) {
                g_lastConfigSnapshot.clear();
                SetStatus(hwnd, L"配置写入失败，请检查程序目录的写入权限", kError);
            }
            if(g_developmentExit && WaitForSingleObject(g_developmentExit,0)==WAIT_OBJECT_0) {
                g_forceExit=true;DestroyWindow(hwnd);return 0;
            }
            g_usageTracker.Tick();
            if(g_player.process && WaitForSingleObject(g_player.process,0)==WAIT_OBJECT_0 &&
               g_recoveryAttempts<3 && !g_applying) SetTimer(hwnd,kRecoveryTimer,1500,nullptr);
            if (g_usageWindow) InvalidateRect(g_usageWindow, nullptr, FALSE);
        } else if (wParam == kPreviewSwitchTimer) {
            BeginCurrentPreview(hwnd);
        } else if (wParam == kOverlayTimer) {
            // 视频刚打开时原始尺寸可能尚未可用，定时重算取景框；拿到尺寸后停止轮询。
            SIZE native{};
            if (g_previewPlayer && g_previewPlayer->GetNativeVideoSize(native)) {
                KillTimer(hwnd, kOverlayTimer);
            }
            UpdateCropOverlayRegion(g_cropOverlay);
            InvalidateRect(g_cropOverlay, nullptr, FALSE);
            SetWindowPos(g_applyButton, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return 0;
    case kRestoreMessage:
        RestoreFromTray(hwnd);
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK || LOWORD(lParam) == NIN_SELECT ||
            LOWORD(lParam) == NIN_KEYSELECT) {
            RestoreFromTray(hwnd);
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        return 0;
    case WM_MEASUREITEM:
        if(auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);item->CtlType==ODT_COMBOBOX){item->itemHeight=Scale(30);return TRUE;}
        break;
    case WM_NOTIFY:
        if(auto* header=reinterpret_cast<NMHDR*>(lParam)) {
            if(SettingTooltips::Notify(header))return 0;
            if(header->hwndFrom==g_seekBar && header->code==NM_CUSTOMDRAW)
                return DrawThemeSlider(header,g_backgroundColor);
        }
        break;
    case WM_DRAWITEM:
        if(auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lParam);item->CtlType==ODT_COMBOBOX){DrawCombo(item);return TRUE;}
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_MOUSEMOVE: {
        if(g_tooltip) {MSG relay{hwnd,message,wParam,lParam};SendMessageW(g_tooltip,TTM_RELAYEVENT,0,reinterpret_cast<LPARAM>(&relay));}
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if(g_splitDragging){RECT client{};GetClientRect(hwnd,&client);
            g_previewRatio=std::clamp(double(point.x-Scale(24))/(std::max)(1L,client.right-Scale(66)),.45,.75);
            LayoutControls(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;}
        const int hover = HitTestThumbnail(hwnd, point);
        if (hover != g_hoverThumbnail) {
            const int previous = g_hoverThumbnail;
            g_hoverThumbnail = hover;
            InvalidateThumbnail(hwnd, previous);
            InvalidateThumbnail(hwnd, hover);
            if(g_tooltip) {
                TOOLINFOW tool{sizeof(tool)};tool.hwnd=hwnd;tool.uId=0xD0;
                SendMessageW(g_tooltip,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool));
                if(hover>=0) {
                    const auto position=std::find(g_galleryIndices.begin(),g_galleryIndices.end(),static_cast<size_t>(hover));
                    if(position!=g_galleryIndices.end()) {
                        const int slot=static_cast<int>(position-g_galleryIndices.begin()-g_galleryPage*kGalleryPageSize);
                        tool.rect=ThumbnailRect(hwnd,slot);tool.lpszText=g_videos[hover].title.data();
                        SendMessageW(g_tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
                    }
                }
            }
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE: {
        const int previous = g_hoverThumbnail;
        g_hoverThumbnail = -1;
        InvalidateThumbnail(hwnd, previous);
        return 0;
    }
    case WM_LBUTTONUP: {
        if(g_splitDragging){g_splitDragging=false;ReleaseCapture();SaveConfig();return 0;}
        if(g_applying) return 0;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int index = HitTestThumbnail(hwnd, point);
        if (index >= 0) SelectVideo(hwnd, static_cast<size_t>(index));
        return 0;
    }
    case WM_LBUTTONDOWN:{POINT p{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};RECT splitter=SplitterRect(hwnd);
        if(!g_settingsOpen&&!g_applying&&PtInRect(&splitter,p)){g_splitDragging=true;SetCapture(hwnd);}return 0;}
    case WM_CAPTURECHANGED:g_splitDragging=false;return 0;
    case WM_SETCURSOR:{POINT p{};GetCursorPos(&p);ScreenToClient(hwnd,&p);RECT splitter=SplitterRect(hwnd);
        if(!g_settingsOpen&&PtInRect(&splitter,p)){SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;}break;}
    case WM_DROPFILES:{HDROP drop=reinterpret_cast<HDROP>(wParam);std::vector<std::wstring> paths;
        const UINT count=(std::min)(1000U,DragQueryFileW(drop,0xffffffff,nullptr,0));
        for(UINT n=0;n<count;++n){wchar_t path[32768]{};if(DragQueryFileW(drop,n,path,32768))paths.push_back(path);}
        DragFinish(drop);AcceptWallpaperFiles(hwnd,paths);return 0;}
    case WM_HSCROLL:{
        if(reinterpret_cast<HWND>(lParam)!=g_seekBar||g_applying)return 0;
        const int code=LOWORD(wParam);g_seekDragging=code==TB_THUMBTRACK;
        if(code==TB_ENDTRACK||code==TB_THUMBPOSITION||code==TB_LINEUP||code==TB_LINEDOWN||code==TB_PAGEUP||code==TB_PAGEDOWN){
            g_pendingSeek=int(SendMessageW(g_seekBar,TBM_GETPOS,0,0));
            // 共享预览拖动进度时切换成独立预览，不跳转桌面上的壁纸。
            if(g_sharedPreviewActive){StopSharedPreview(hwnd);ScheduleCurrentPreview(hwnd,true);}
            TickExperience(hwnd);
        }return 0;
    }
    case WM_MOUSEWHEEL:
        g_lastInteraction = GetTickCount64();
        if(g_applying) return 0;
        if (g_galleryIndices.size() > kGalleryPageSize) {
            const size_t pages = (g_galleryIndices.size() + kGalleryPageSize - 1) / kGalleryPageSize;
            if (GET_WHEEL_DELTA_WPARAM(wParam) < 0 && g_galleryPage + 1 < pages) ++g_galleryPage;
            else if (GET_WHEEL_DELTA_WPARAM(wParam) > 0 && g_galleryPage > 0) --g_galleryPage;
            g_hoverThumbnail = -1;
            QueueVisibleThumbnails(hwnd);
            LayoutControls(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT: {
        // 整个界面先画到内存位图，再一次 BitBlt 到窗口，避免悬停和刷新时闪烁。
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (EnsureBackBuffer(dc, width, height)) {
            const int savedDc = SaveDC(g_backBufferDc);
            IntersectClipRect(g_backBufferDc, paint.rcPaint.left, paint.rcPaint.top,
                paint.rcPaint.right, paint.rcPaint.bottom);
            PaintWindow(hwnd, g_backBufferDc);
            RestoreDC(g_backBufferDc, savedDc);
            BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top,
                   paint.rcPaint.right - paint.rcPaint.left,
                   paint.rcPaint.bottom - paint.rcPaint.top,
                   g_backBufferDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
        } else {
            PaintWindow(hwnd, dc);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        // 背景已经由上面的双缓冲完整绘制；阻止系统再次擦背景可进一步减少闪烁。
        return 1;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = Scale(940);
        limits->ptMinTrackSize.y = Scale(670);
        return 0;
    }
    case WM_CLOSE:
        if (g_forceExit) DestroyWindow(hwnd);
        // 标题栏叉号只负责隐藏控制界面。退出入口统一放在托盘右键菜单中，
        // 避免误关后连同动态壁纸一起停止。
        else MinimizeToTray(hwnd);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            g_forceExit = true;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_forceExit = true;
        KillTimer(hwnd,kExperienceTimer);StopEffectPreview();
        if(g_experienceWindow)DestroyWindow(g_experienceWindow);
        // 销毁顺序很重要：先停止预览/缩略图线程，再通知后台播放器退出并恢复静态壁纸。
        KillTimer(hwnd, kUsageTimer);
        KillTimer(hwnd, kDirectoryTimer);
        KillTimer(hwnd, kRecoveryTimer);
        g_directoryWatcher.Stop();
        g_libraryScanner.Stop();
        SaveVideoFocus();
        WTSUnRegisterSessionNotification(hwnd);
        g_usageTracker.Stop();
        if (g_usageWindow) DestroyWindow(g_usageWindow);
        RemoveTrayIcon(hwnd);
        if (g_memoryBall) DestroyWindow(g_memoryBall);
        StopSharedPreview(hwnd);
        g_sharedPreview.reset();
        g_previewPlayer.reset();
        StopProxyJob();
        ClearVideoLibrary();
        StopPlayer(g_player, true);
        g_processCleanup.Drain();
        RestoreStaticWallpaper();
        if (!g_configStore.Flush()) RuntimeLog(L"Configuration save failed during exit");
        // 清理后台已投递而窗口来不及接收的结果，避免泄漏 GDI 位图。
        {
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, kThumbnailReady, kThumbnailReady, PM_REMOVE)) {
                auto* result = reinterpret_cast<ThumbnailResult*>(pending.lParam);
                if (result) { if (result->bitmap) DeleteObject(result->bitmap); delete result; }
            }
            while (PeekMessageW(&pending, hwnd, kProxyReady, kProxyReady, PM_REMOVE))
                delete reinterpret_cast<ProxyResult*>(pending.lParam);
        }
        DeleteBackBuffer();
        DeleteFonts();
        if(g_controlBrush) {DeleteObject(g_controlBrush);g_controlBrush=nullptr;}
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    // 单 EXE 双模式：--player 直接进入 Player.cpp，不创建控制界面。
    int modeArgc{};
    wchar_t** modeArgv = CommandLineToArgvW(GetCommandLineW(), &modeArgc);
    bool playerMode = false;
    bool proxyMode = false;
    bool developmentExit=false,diagnose=false;
    for (int i = 1; modeArgv && i < modeArgc; ++i) {
        if (_wcsicmp(modeArgv[i],L"--exit")==0) developmentExit=true;
        if (_wcsicmp(modeArgv[i],L"--diagnose")==0) diagnose=true;
        if (_wcsicmp(modeArgv[i], L"--player") == 0) {
            playerMode = true;
            break;
        }
        if (_wcsicmp(modeArgv[i], L"--proxy") == 0) {
            proxyMode = true;
            break;
        }
    }
    if (modeArgv) LocalFree(modeArgv);
    if(developmentExit) {
        HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,kDevelopmentExitEvent);
        if(!event) return 1;
        const bool ok=SetEvent(event)!=FALSE;CloseHandle(event);return ok?0:1;
    }
    if(diagnose) {LogDesktopTree();return 0;}
    if (proxyMode) return RunProxyTranscoder();
    if (playerMode) return RunPlayer(instance);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ImmDisableIME(static_cast<DWORD>(-1));
    // 控制器只允许一个实例。再次双击 EXE 时不启动第二套 UI，而是唤醒托盘中的窗口。
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\DWDNative_Controller");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (singleInstance) CloseHandle(singleInstance);
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing) {
            PostMessageW(existing, kRestoreMessage, 0, 0);
        }
        return 0;
    }

    // 文件选择器、Shell 缩略图和 MFPlay 都依赖 COM；Media Foundation 也必须全局 Startup。
    g_developmentExit=CreateEventW(nullptr,TRUE,FALSE,kDevelopmentExitEvent);
    RuntimeLog(L"Controller started");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        CoUninitialize();
        CloseHandle(singleInstance);
        MessageBoxW(nullptr, L"Windows 媒体组件初始化失败。", L"DWD", MB_ICONERROR);
        return 2;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES|ICC_BAR_CLASSES|ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput gdiplusInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        g_gdiplusToken = 0;
    }

    WNDCLASSEXW cropClass{sizeof(cropClass)};
    cropClass.lpfnWndProc = CropOverlayProc;
    cropClass.hInstance = instance;
    cropClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    cropClass.hbrBackground = nullptr;
    cropClass.lpszClassName = kCropOverlayClass;
    RegisterClassExW(&cropClass);

    WNDCLASSEXW memoryClass{sizeof(memoryClass)};
    memoryClass.style = CS_DBLCLKS;
    memoryClass.lpfnWndProc = MemoryBallProc;
    memoryClass.hInstance = instance;
    memoryClass.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    memoryClass.hbrBackground = nullptr;
    memoryClass.lpszClassName = kMemoryBallClass;
    RegisterClassExW(&memoryClass);

    WNDCLASSEXW usageClass{sizeof(usageClass)};
    usageClass.lpfnWndProc = UsageWindowProc;
    usageClass.hInstance = instance;
    usageClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    usageClass.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    usageClass.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    usageClass.hbrBackground = nullptr;
    usageClass.lpszClassName = kUsageWindowClass;
    RegisterClassExW(&usageClass);

    WNDCLASSEXW sharedClass{sizeof(sharedClass)};
    sharedClass.hInstance = instance;
    sharedClass.lpszClassName = L"DWDSharedPreviewSurface";
    sharedClass.lpfnWndProc = [](HWND target, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        // 此窗口只由 D3D 绘制；禁止 GDI 擦除，以免覆盖视频或引起闪烁。
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; BeginPaint(target, &paint); EndPaint(target, &paint); return 0;
        }
        return DefWindowProcW(target, message, wp, lp);
    };
    RegisterClassExW(&sharedClass);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    windowClass.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!windowClass.hIconSm) windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass);

    g_cacheFolder = LoadConfigValue(L"Cache", L"Directory");
    if (g_cacheFolder.empty()) g_cacheFolder = DefaultCacheFolder();
    else if(std::filesystem::path(g_cacheFolder).is_relative())g_cacheFolder=ExeDirectory()+L"\\"+g_cacheFolder;
    LoadExperience();
    ParseConfigColor(LoadConfigValue(L"Appearance",L"BackgroundColor"),g_customBackground);
    const auto number=[](const wchar_t* section,const wchar_t* key,int fallback,int maximum) {
        const auto text=LoadConfigValue(section,key);
        return text.empty()?fallback:std::clamp(_wtoi(text.c_str()),0,maximum);
    };
    g_theme=number(L"Appearance",L"Theme",0,3);
    if(LoadConfigValue(L"Appearance",L"Theme").empty() && g_customBackground!=kBackground)
        g_theme=3;
    g_sortOrder=number(L"Library",L"Sort",0,2);
    g_quality=number(L"Playback",L"Quality",0,2);
    g_fullscreenRule=number(L"Playback",L"FullscreenRule",0,2);
    g_targetMonitor=LoadConfigValue(L"Playback",L"TargetMonitor");
    g_cropMonitor=LoadConfigValue(L"Playback",L"CropMonitor");
    g_showMemoryBall=LoadConfigValue(L"Appearance",L"MemoryBall")!=L"0";
    g_autoRestore=LoadConfigValue(L"Library",L"AutoRestore")==L"1";
    g_usageEnabled=LoadConfigValue(L"Usage",L"Enabled")!=L"0";
    g_lastWallpaper=LoadConfigValue(L"Library",L"LastWallpaper");
    const auto limit=LoadConfigValue(L"Cache",L"LimitMB");
    if(!limit.empty()) g_cacheLimitBytes=static_cast<uint64_t>(
        std::clamp(_wtoi(limit.c_str()),512,4096))*1024*1024;
    std::vector<wchar_t> focusSection(65536);
    const auto configPath=ConfigPath();
    GetPrivateProfileSectionW(L"Focus",focusSection.data(),static_cast<DWORD>(focusSection.size()),configPath.c_str());
    for(const wchar_t* line=focusSection.data();*line;line+=wcslen(line)+1) {
        const std::wstring field=line;const auto eq=field.find(L'='),comma=field.find(L',');
        if((eq==16 || (eq==33 && field[16]==L'_')) && comma!=std::wstring::npos && comma>eq) {
            const auto xText=field.substr(eq+1,comma-eq-1),yText=field.substr(comma+1);
            wchar_t* endX{};wchar_t* endY{};
            const double x=wcstod(xText.c_str(),&endX),y=wcstod(yText.c_str(),&endY);
            if(endX && !*endX && endY && !*endY && x>=0 && x<=1 && y>=0 && y<=1)
                g_savedFocus[field.substr(0,eq)]={x,y};
        }
    }
    const std::wstring configuredUsageMode = LoadConfigValue(L"Usage", L"Mode");
    g_usageMode = _wcsicmp(configuredUsageMode.c_str(), L"Viewing") == 0
        ? UsageMode::Viewing : UsageMode::Active;

    const UINT dpi = GetDpiForSystem();
    const int clientWidth = MulDiv(1180, static_cast<int>(dpi), 96);
    const int clientHeight = MulDiv(650, static_cast<int>(dpi), 96);
    RECT windowRect{0, 0, clientWidth, clientHeight};
    constexpr DWORD controllerStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                                      WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&windowRect, controllerStyle,
                             FALSE, 0, dpi);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND window = CreateWindowExW(0, kWindowClass, L"DWD",
                                  controllerStyle,
                                  work.left + (work.right - work.left - width) / 2,
                                  work.top + (work.bottom - work.top - height) / 2,
                                  width, height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        const DWORD error = GetLastError();
        wchar_t message[160]{};
        swprintf_s(message, L"控制窗口创建失败，Windows 错误码：%lu", error);
        MessageBoxW(nullptr, message, L"DWD", MB_ICONERROR);
        if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
        MFShutdown();
        CoUninitialize();
        CloseHandle(singleInstance);
        return static_cast<int>(error ? error : 1);
    }
    // 程序一启动就注册托盘图标；控制窗口打开时图标也保持可见。
    AddTrayIcon(window);
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    // 主窗口完成显示后再创建工具型悬浮球，避免系统把悬浮球识别成进程主窗口。
    g_memoryBall = CreateMemoryBall(instance);
    if(!g_showMemoryBall && g_memoryBall) {ShowWindow(g_memoryBall,SW_HIDE);KillTimer(g_memoryBall,kMemoryTimer);}

    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--apply") == 0 && i + 1 < argc) g_videoPath = argv[++i];
        else if (_wcsicmp(argv[i], L"--minimized") == 0) g_minimizeAfterApply = true;
    }
    if (argv) LocalFree(argv);
    if (!g_videoPath.empty()) {
        ShowWindow(g_previewWindow, SW_SHOW);
        EnableWindow(g_applyButton, TRUE);
        ScheduleCurrentPreview(window, true);
        SetStatus(window, L"视频已选择，可以应用到桌面", kWarning);
        if (LaunchPlayer(window) && g_minimizeAfterApply) MinimizeToTray(window);
    } else {
        const std::wstring savedFolder = LoadConfigValue(L"Library", L"WallpaperFolder");
        const std::wstring lastWallpaper=g_lastWallpaper;
        g_autoApplyPending = g_autoRestore && !lastWallpaper.empty();
        if (!savedFolder.empty()) ScanWallpaperFolder(window,savedFolder,false,
            g_autoRestore?lastWallpaper:std::wstring{});
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    MFShutdown();
    CoUninitialize();
    if(g_developmentExit) CloseHandle(g_developmentExit);
    CloseHandle(singleInstance);
    RuntimeLog(L"Controller exited; wallpaper restored");
    return static_cast<int>(message.wParam);
}
