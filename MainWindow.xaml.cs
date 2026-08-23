using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Forms;
using System.Windows.Controls.Primitives;
using Microsoft.Win32;

namespace DWD;

internal enum WallpaperLayoutMode
{
    PerMonitor16By9,
    SpanDesktop
}

public partial class MainWindow : Window
{
    private string? _videoPath;
    private WallpaperWindow? _wallpaperWindow;
    private int _activeDisplayCount;
    private string? _activeWallpaperPath;
    private bool _activeLoop;
    private bool _activeMuted;
    private bool _isApplyingWallpaper;
    private IntPtr _wallpaperParent;
    private readonly NotifyIcon _trayIcon;
    private bool _isExiting;
    private int _videoPixelWidth;
    private int _videoPixelHeight;
    private double _cropCenterX = 0.5;
    private double _cropCenterY = 0.5;
    private bool _isDraggingCrop;
    private System.Windows.Point _cropDragStart;
    private double _cropDragLeft;
    private double _cropDragTop;
    private Rect _displayedVideoRect;
    private CropRegion? _activeCrop;
    private WallpaperLayoutMode _layoutMode = WallpaperLayoutMode.PerMonitor16By9;
    private WallpaperLayoutMode _activeLayoutMode = WallpaperLayoutMode.PerMonitor16By9;

    public MainWindow()
    {
        InitializeComponent();

        var trayMenu = new ContextMenuStrip();
        trayMenu.Items.Add("显示 DWD", null, (_, _) => Dispatcher.Invoke(ShowFromTray));
        trayMenu.Items.Add("退出", null, (_, _) => Dispatcher.Invoke(ExitApplication));
        _trayIcon = new NotifyIcon
        {
            Text = "DWD 动态壁纸",
            Icon = System.Drawing.Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? System.Drawing.SystemIcons.Application,
            ContextMenuStrip = trayMenu,
            Visible = true
        };
        _trayIcon.DoubleClick += (_, _) => Dispatcher.Invoke(ShowFromTray);
    }

    private void ChooseVideo_Click(object sender, RoutedEventArgs e)
    {
        ChooseVideoFile();
    }

    private bool ChooseVideoFile()
    {
        var dialog = new Microsoft.Win32.OpenFileDialog { Filter = "视频文件|*.mp4;*.webm;*.avi;*.mov;*.mkv|所有文件|*.*" };
        if (dialog.ShowDialog() != true) return false;
        _videoPath = dialog.FileName;
        _videoPixelWidth = 0;
        _videoPixelHeight = 0;
        _cropCenterX = 0.5;
        _cropCenterY = 0.5;
        CropCanvas.Visibility = Visibility.Collapsed;
        FileNameText.Text = Path.GetFileName(_videoPath);
        CurrentVideoText.Text = Path.GetFileName(_videoPath);
        EmptyState.Visibility = Visibility.Collapsed;
        PreviewBar.Visibility = Visibility.Visible;
        WallpaperPreview.Source = new Uri(_videoPath);
        WallpaperPreview.Volume = MuteCheck.IsChecked == true ? 0 : 1;
        WallpaperPreview.Play();
        return true;
    }

    private void TogglePlayback_Click(object sender, RoutedEventArgs e)
    {
        if (WallpaperPreview.Position == TimeSpan.Zero || WallpaperPreview.Source is null) return;
        if (WallpaperPreview.IsLoaded) WallpaperPreview.Pause();
    }

    private void WallpaperPreview_MediaEnded(object sender, RoutedEventArgs e)
    {
        if (LoopCheck.IsChecked == true) { WallpaperPreview.Position = TimeSpan.Zero; WallpaperPreview.Play(); }
    }

    private void WallpaperPreview_MediaOpened(object sender, RoutedEventArgs e)
    {
        _videoPixelWidth = WallpaperPreview.NaturalVideoWidth;
        _videoPixelHeight = WallpaperPreview.NaturalVideoHeight;
        UpdateCropFrame();
    }

    private void PreviewSurface_SizeChanged(object sender, SizeChangedEventArgs e) => UpdateCropFrame();

    private void UpdateCropFrame()
    {
        if (_videoPixelWidth <= 0 || _videoPixelHeight <= 0 ||
            PreviewSurface.ActualWidth <= 0 || PreviewSurface.ActualHeight <= 0) return;

        var scale = Math.Min(PreviewSurface.ActualWidth / _videoPixelWidth,
            PreviewSurface.ActualHeight / _videoPixelHeight);
        var displayedWidth = _videoPixelWidth * scale;
        var displayedHeight = _videoPixelHeight * scale;
        _displayedVideoRect = new Rect(
            (PreviewSurface.ActualWidth - displayedWidth) / 2,
            (PreviewSurface.ActualHeight - displayedHeight) / 2,
            displayedWidth,
            displayedHeight);

        var desktop = GetVirtualScreenBounds(Screen.AllScreens);
        var targetAspect = GetTargetAspect(desktop);
        var sourceAspect = _videoPixelWidth / (double)_videoPixelHeight;
        var cropPixelWidth = sourceAspect > targetAspect ? _videoPixelHeight * targetAspect : _videoPixelWidth;
        var cropPixelHeight = sourceAspect > targetAspect ? _videoPixelHeight : _videoPixelWidth / targetAspect;
        var frameWidth = cropPixelWidth * scale;
        var frameHeight = cropPixelHeight * scale;

        var minCenterX = cropPixelWidth / (2 * _videoPixelWidth);
        var minCenterY = cropPixelHeight / (2 * _videoPixelHeight);
        _cropCenterX = Math.Clamp(_cropCenterX, minCenterX, 1 - minCenterX);
        _cropCenterY = Math.Clamp(_cropCenterY, minCenterY, 1 - minCenterY);

        var left = _displayedVideoRect.Left + (_cropCenterX * _videoPixelWidth - cropPixelWidth / 2) * scale;
        var top = _displayedVideoRect.Top + (_cropCenterY * _videoPixelHeight - cropPixelHeight / 2) * scale;
        CropFrame.Width = frameWidth;
        CropFrame.Height = frameHeight;
        System.Windows.Controls.Canvas.SetLeft(CropFrame, left);
        System.Windows.Controls.Canvas.SetTop(CropFrame, top);
        UpdateCropShades(left, top, frameWidth, frameHeight);
        CropCanvas.Visibility = Visibility.Visible;
    }

    private void UpdateCropShades(double left, double top, double width, double height)
    {
        SetCanvasRect(CropShadeTop, _displayedVideoRect.Left, _displayedVideoRect.Top,
            _displayedVideoRect.Width, Math.Max(0, top - _displayedVideoRect.Top));
        SetCanvasRect(CropShadeBottom, _displayedVideoRect.Left, top + height,
            _displayedVideoRect.Width, Math.Max(0, _displayedVideoRect.Bottom - top - height));
        SetCanvasRect(CropShadeLeft, _displayedVideoRect.Left, top,
            Math.Max(0, left - _displayedVideoRect.Left), height);
        SetCanvasRect(CropShadeRight, left + width, top,
            Math.Max(0, _displayedVideoRect.Right - left - width), height);
    }

    private static void SetCanvasRect(FrameworkElement element, double left, double top, double width, double height)
    {
        System.Windows.Controls.Canvas.SetLeft(element, left);
        System.Windows.Controls.Canvas.SetTop(element, top);
        element.Width = width;
        element.Height = height;
    }

    private void CropFrame_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        _isDraggingCrop = true;
        _cropDragStart = e.GetPosition(CropCanvas);
        _cropDragLeft = System.Windows.Controls.Canvas.GetLeft(CropFrame);
        _cropDragTop = System.Windows.Controls.Canvas.GetTop(CropFrame);
        CropFrame.CaptureMouse();
        e.Handled = true;
    }

    private void CropFrame_MouseMove(object sender, System.Windows.Input.MouseEventArgs e)
    {
        if (!_isDraggingCrop || e.LeftButton != MouseButtonState.Pressed) return;
        var point = e.GetPosition(CropCanvas);
        var left = Math.Clamp(_cropDragLeft + point.X - _cropDragStart.X,
            _displayedVideoRect.Left, _displayedVideoRect.Right - CropFrame.Width);
        var top = Math.Clamp(_cropDragTop + point.Y - _cropDragStart.Y,
            _displayedVideoRect.Top, _displayedVideoRect.Bottom - CropFrame.Height);
        System.Windows.Controls.Canvas.SetLeft(CropFrame, left);
        System.Windows.Controls.Canvas.SetTop(CropFrame, top);
        UpdateCropShades(left, top, CropFrame.Width, CropFrame.Height);
        _cropCenterX = (left - _displayedVideoRect.Left + CropFrame.Width / 2) / _displayedVideoRect.Width;
        _cropCenterY = (top - _displayedVideoRect.Top + CropFrame.Height / 2) / _displayedVideoRect.Height;
    }

    private void CropFrame_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        _isDraggingCrop = false;
        CropFrame.ReleaseMouseCapture();
        e.Handled = true;
    }

    private void MuteCheck_Changed(object sender, RoutedEventArgs e) => WallpaperPreview.Volume = MuteCheck.IsChecked == true ? 0 : 1;

    private void AspectModeButton_Click(object sender, RoutedEventArgs e)
    {
        if (AspectModeButton.ContextMenu is not { } menu) return;
        menu.PlacementTarget = AspectModeButton;
        menu.Placement = PlacementMode.Bottom;
        menu.IsOpen = true;
    }

    private void SelectPerMonitorMode_Click(object sender, RoutedEventArgs e) =>
        SetLayoutMode(WallpaperLayoutMode.PerMonitor16By9);

    private void SelectSpanMode_Click(object sender, RoutedEventArgs e) =>
        SetLayoutMode(WallpaperLayoutMode.SpanDesktop);

    private void SetLayoutMode(WallpaperLayoutMode mode)
    {
        _layoutMode = mode;
        var perMonitor = mode == WallpaperLayoutMode.PerMonitor16By9;
        AspectModeButton.Content = perMonitor
            ? "当前比例：16:9 · 每屏相同  ▾"
            : "当前比例：跨屏拼接  ▾";
        PerMonitorModeMenuItem.IsChecked = perMonitor;
        SpanModeMenuItem.IsChecked = !perMonitor;
        CropInstructionText.Text = perMonitor
            ? "16:9 · 两个屏幕显示同一选区"
            : "跨屏拼接 · 选区将横跨两个屏幕";
        UpdateCropFrame();
    }

    private void SetWallpaper_Click(object sender, RoutedEventArgs e)
    {
        if (_videoPath is null && !ChooseVideoFile()) return;
        ApplyWallpaper();
    }

    private async void ApplyWallpaper()
    {
        if (_videoPath is null) return;
        var loop = LoopCheck.IsChecked == true;
        var muted = MuteCheck.IsChecked == true;
        var screens = Screen.AllScreens;
        var virtualBounds = GetVirtualScreenBounds(screens);
        var crop = BuildCropRegion(GetTargetAspect(virtualBounds));
        var screenTiles = _layoutMode == WallpaperLayoutMode.PerMonitor16By9
            ? screens.Select(screen => screen.Bounds).ToArray()
            : null;

        // Clicking the button repeatedly while the same wallpaper is playing is a no-op.
        if (_isApplyingWallpaper ||
            (_wallpaperWindow is not null && _activeDisplayCount == screens.Length &&
             string.Equals(_activeWallpaperPath, _videoPath, StringComparison.OrdinalIgnoreCase) &&
             _activeLoop == loop && _activeMuted == muted && _activeCrop == crop &&
             _activeLayoutMode == _layoutMode)) return;

        // The desktop player becomes the only active decoder after applying.
        // Release the preview MediaElement instead of leaving a second decoder
        // and its frame buffers alive behind the control UI.
        WallpaperPreview.Stop();
        WallpaperPreview.Source = null;
        CropCanvas.Visibility = Visibility.Collapsed;
        _isApplyingWallpaper = true;
        WallpaperWindow? nextWindow = null;
        try
        {
            // Always prepare a fresh player off-screen. Reusing the existing
            // window would expose mpv's black initialization frames.
            nextWindow = new WallpaperWindow(_videoPath, virtualBounds, loop, muted, crop, screenTiles);
            nextWindow.Show();
            nextWindow.StartPlayback();
            var startupTimeout = _layoutMode == WallpaperLayoutMode.PerMonitor16By9
                ? TimeSpan.FromSeconds(10)
                : TimeSpan.FromSeconds(4);
            if (!await nextWindow.WaitUntilReadyAsync(startupTimeout))
                throw new TimeoutException("视频输出启动超时，桌面壁纸未更改");

            var nextParent = NativeMethods.SetWallpaperParent(new WindowInteropHelper(nextWindow).Handle, virtualBounds);
            var previousWindow = _wallpaperWindow;
            _wallpaperWindow = nextWindow;
            _wallpaperParent = nextParent;
            _activeDisplayCount = screens.Length;
            _activeWallpaperPath = _videoPath;
            _activeLoop = loop;
            _activeMuted = muted;
            _activeCrop = crop;
            _activeLayoutMode = _layoutMode;

            previousWindow?.Hide();
            previousWindow?.Close();
        }
        catch (Exception ex)
        {
            nextWindow?.Close();
            System.Windows.MessageBox.Show($"设置桌面壁纸失败：{ex.Message}", "DWD", MessageBoxButton.OK, MessageBoxImage.Error);
        }
        finally
        {
            _isApplyingWallpaper = false;
        }
    }

    private double GetTargetAspect(System.Drawing.Rectangle desktopBounds) =>
        _layoutMode == WallpaperLayoutMode.PerMonitor16By9
            ? 16d / 9d
            : desktopBounds.Width / (double)desktopBounds.Height;

    private CropRegion? BuildCropRegion(double targetAspect)
    {
        if (_videoPixelWidth <= 0 || _videoPixelHeight <= 0) return null;
        var sourceAspect = _videoPixelWidth / (double)_videoPixelHeight;
        var cropWidth = sourceAspect > targetAspect
            ? (int)Math.Round(_videoPixelHeight * targetAspect)
            : _videoPixelWidth;
        var cropHeight = sourceAspect > targetAspect
            ? _videoPixelHeight
            : (int)Math.Round(_videoPixelWidth / targetAspect);

        cropWidth = Math.Clamp(cropWidth, 2, _videoPixelWidth);
        cropHeight = Math.Clamp(cropHeight, 2, _videoPixelHeight);
        cropWidth -= cropWidth % 2;
        cropHeight -= cropHeight % 2;
        var x = (int)Math.Round(_cropCenterX * _videoPixelWidth - cropWidth / 2d);
        var y = (int)Math.Round(_cropCenterY * _videoPixelHeight - cropHeight / 2d);
        x = Math.Clamp(x, 0, _videoPixelWidth - cropWidth);
        y = Math.Clamp(y, 0, _videoPixelHeight - cropHeight);
        return new CropRegion(x, y, cropWidth, cropHeight);
    }

    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Close_Click(object sender, RoutedEventArgs e) => HideToTray();

    private void HideToTray()
    {
        Hide();
        ShowInTaskbar = false;
    }

    internal void ShowFromTray()
    {
        ShowInTaskbar = true;
        Show();
        WindowState = WindowState.Normal;
        Activate();
    }

    private void ExitApplication()
    {
        _isExiting = true;
        _trayIcon.Visible = false;
        Close();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!_isExiting)
        {
            e.Cancel = true;
            HideToTray();
            return;
        }

        base.OnClosing(e);
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        // Keep the window controls clickable while allowing the custom title bar to drag.
        if (e.OriginalSource is System.Windows.Controls.Button) return;
        if (e.LeftButton == MouseButtonState.Pressed) DragMove();
    }

    protected override void OnClosed(EventArgs e)
    {
        WallpaperPreview.Stop();
        var wallpaper = _wallpaperWindow;
        _wallpaperWindow = null;
        if (wallpaper is not null)
        {
            var wallpaperHandle = new WindowInteropHelper(wallpaper).Handle;
            wallpaper.Hide();
            NativeMethods.DetachWallpaper(wallpaperHandle);
            wallpaper.StopPlayback();
            wallpaper.Close();
        }
        NativeMethods.RestoreDesktopHost(_wallpaperParent);
        _wallpaperParent = IntPtr.Zero;
        _trayIcon.Visible = false;
        _trayIcon.Dispose();
        base.OnClosed(e);
    }

    private static System.Drawing.Rectangle GetVirtualScreenBounds(Screen[] screens)
    {
        var bounds = screens[0].Bounds;
        for (var i = 1; i < screens.Length; i++) bounds = System.Drawing.Rectangle.Union(bounds, screens[i].Bounds);
        return bounds;
    }
}

internal static class NativeMethods
{
    private const int WorkerMessage = 0x052C;
    private const int GwlStyle = -16;
    private const long WsChild = 0x40000000L;
    private const long WsCaption = 0x00C00000L;
    private const long WsThickFrame = 0x00040000L;
    private const long WsMinimize = 0x20000000L;
    private const uint SwpNoActivate = 0x0010;
    private const uint SwpShowWindow = 0x0040;
    private const uint SwpNoZOrder = 0x0004;
    private const int SwHide = 0;
    private const uint WmClose = 0x0010;
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr FindWindow(string? lpClassName, string? lpWindowName);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr extraData);
    [DllImport("user32.dll")] private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr childAfter, string? className, string? windowTitle);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SetParent(IntPtr child, IntPtr newParent);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW", SetLastError = true)] private static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int index, IntPtr value);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetWindowPos(IntPtr hWnd, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr hWnd, int command);
    [DllImport("user32.dll")] private static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr hWnd, StringBuilder className, int maxCount);
    [DllImport("user32.dll")] private static extern bool InvalidateRect(IntPtr hWnd, IntPtr rect, bool erase);
    [DllImport("user32.dll")] private static extern bool UpdateWindow(IntPtr hWnd);
    private struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr SetWallpaperParent(IntPtr window, System.Drawing.Rectangle bounds)
    {
        var progman = FindWindow("Progman", null);
        if (progman == IntPtr.Zero) throw new InvalidOperationException("找不到 Windows 桌面宿主 Progman");

        var lastError = 0;
        for (var attempt = 0; attempt < 3; attempt++)
        {
            var parent = FindOrCreateWallpaperHost(progman);
            if (TryAttachWallpaper(window, parent, bounds, out lastError)) return parent;
            Thread.Sleep(100);
        }

        throw new InvalidOperationException($"无法挂载到 Windows 桌面宿主（Win32 错误 {lastError}）");
    }

    private static IntPtr FindOrCreateWallpaperHost(IntPtr progman)
    {
        SendMessageTimeout(progman, WorkerMessage, IntPtr.Zero, IntPtr.Zero, 0, 1000, out _);
        for (var attempt = 0; attempt < 20; attempt++)
        {
            var worker = FindWallpaperWorker();
            if (worker != IntPtr.Zero && IsWindow(worker)) return worker;
            Thread.Sleep(50);
        }
        return progman;
    }

    private static IntPtr FindWallpaperWorker()
    {
        IntPtr worker = IntPtr.Zero;
        EnumWindows((top, _) =>
        {
            var shell = FindWindowEx(top, IntPtr.Zero, "SHELLDLL_DefView", null);
            if (shell != IntPtr.Zero) { worker = FindWindowEx(IntPtr.Zero, top, "WorkerW", null); return false; }
            return true;
        }, IntPtr.Zero);
        return worker;
    }

    private static bool TryAttachWallpaper(IntPtr window, IntPtr parent,
        System.Drawing.Rectangle bounds, out int lastError)
    {
        lastError = 0;
        SetParent(window, parent);
        if (GetParent(window) != parent)
        {
            lastError = Marshal.GetLastWin32Error();
            return false;
        }

        // SetParent alone leaves a top-level WPF window behaving like a normal window.
        // Convert it into a borderless child and size it to the complete virtual desktop.
        var style = GetWindowLongPtr(window, GwlStyle).ToInt64();
        style &= ~(WsCaption | WsThickFrame | WsMinimize);
        style |= WsChild;
        SetWindowLongPtr(window, GwlStyle, new IntPtr(style));
        if ((GetWindowLongPtr(window, GwlStyle).ToInt64() & WsChild) == 0)
        {
            lastError = Marshal.GetLastWin32Error();
            return false;
        }
        if (!GetWindowRect(parent, out var parentRect))
        {
            lastError = Marshal.GetLastWin32Error();
            return false;
        }
        if (!SetWindowPos(window, IntPtr.Zero, bounds.Left - parentRect.Left, bounds.Top - parentRect.Top,
                bounds.Width, bounds.Height, SwpNoZOrder | SwpNoActivate | SwpShowWindow))
        {
            lastError = Marshal.GetLastWin32Error();
            return false;
        }
        if (!GetWindowRect(window, out var actualRect) ||
            actualRect.Right - actualRect.Left != bounds.Width ||
            actualRect.Bottom - actualRect.Top != bounds.Height)
        {
            lastError = Marshal.GetLastWin32Error();
            return false;
        }
        return true;
    }

    public static void DetachWallpaper(IntPtr window)
    {
        if (window == IntPtr.Zero || !IsWindow(window)) return;
        ShowWindow(window, SwHide);
        SetParent(window, IntPtr.Zero);
    }

    public static void RestoreDesktopHost(IntPtr host)
    {
        var progman = FindWindow("Progman", null);
        if (host != IntPtr.Zero && host != progman && IsWindow(host))
        {
            var className = new StringBuilder(64);
            GetClassName(host, className, className.Capacity);
            var containsDesktopIcons = FindWindowEx(host, IntPtr.Zero, "SHELLDLL_DefView", null) != IntPtr.Zero;
            if (string.Equals(className.ToString(), "WorkerW", StringComparison.Ordinal) && !containsDesktopIcons)
            {
                SendMessageTimeout(host, WmClose, IntPtr.Zero, IntPtr.Zero, 0, 1000, out _);
                for (var attempt = 0; attempt < 30 && IsWindow(host); attempt++) Thread.Sleep(50);
            }
        }

        if (progman != IntPtr.Zero)
        {
            InvalidateRect(progman, IntPtr.Zero, true);
            UpdateWindow(progman);
        }
    }
}
