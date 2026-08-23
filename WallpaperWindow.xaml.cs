using System.Drawing;
using System.Diagnostics;
using System.ComponentModel;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Windows.Forms;
using System.Windows;
using System.Windows.Interop;

namespace DWD;

public readonly record struct CropRegion(int X, int Y, int Width, int Height);

public partial class WallpaperWindow : Window
{
    private string _path;
    private bool _loop;
    private bool _muted;
    private Process? _player;
    private readonly Panel _renderPanel;
    private readonly string _pipeName = $"dwd-mpv-{Environment.ProcessId}-{Guid.NewGuid():N}";
    private readonly CropRegion? _crop;
    private readonly Rectangle _desktopBounds;
    private readonly Rectangle[] _screenTiles;

    public WallpaperWindow(string path, Rectangle bounds, bool loop, bool muted, CropRegion? crop,
        Rectangle[]? screenTiles)
    {
        InitializeComponent();
        _path = path;
        _loop = loop;
        _muted = muted;
        _crop = crop;
        _desktopBounds = bounds;
        _screenTiles = screenTiles ?? [];
        _renderPanel = new Panel { Dock = DockStyle.Fill, BackColor = Color.Black };
        VideoHost.Child = _renderPanel;
        // Decode off-screen first. The window is moved into WorkerW only after
        // mpv confirms that its video output is ready.
        Left = -32000;
        Top = -32000;
        Width = bounds.Width;
        Height = bounds.Height;
    }

    public void StartPlayback()
    {
        if (_player is not null && !_player.HasExited) return;
        _renderPanel.CreateControl();
        var playerPath = Path.Combine(AppContext.BaseDirectory, "player", "mpv.exe");
        if (!File.Exists(playerPath)) throw new FileNotFoundException("找不到内置 mpv 播放器", playerPath);

        var startInfo = new ProcessStartInfo
        {
            FileName = playerPath,
            WorkingDirectory = Path.GetDirectoryName(playerPath)!,
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden
        };
        AddArgument(startInfo, $"--wid={_renderPanel.Handle.ToInt64()}");
        AddArgument(startInfo, $@"--input-ipc-server=\\.\pipe\{_pipeName}");
        AddArgument(startInfo, "--force-window=yes");
        AddArgument(startInfo, "--no-border");
        AddArgument(startInfo, "--no-osc");
        AddArgument(startInfo, "--osd-level=0");
        AddArgument(startInfo, "--no-input-default-bindings");
        AddArgument(startInfo, "--really-quiet");
        AddArgument(startInfo, $"--loop-file={(_loop ? "inf" : "no")}");
        AddArgument(startInfo, $"--mute={(_muted ? "yes" : "no")}");
        AddArgument(startInfo, "--keepaspect=yes");
        var filters = new List<string>();
        if (_crop is { } crop)
            filters.Add($"crop={crop.Width}:{crop.Height}:{crop.X}:{crop.Y}");
        if (_screenTiles.Length > 1)
        {
            var sources = string.Concat(Enumerable.Range(0, _screenTiles.Length).Select(index => $"[src{index}]"));
            var graph = new StringBuilder($"split={_screenTiles.Length}{sources};");
            for (var index = 0; index < _screenTiles.Length; index++)
            {
                var tile = _screenTiles[index];
                graph.Append($"[src{index}]scale={tile.Width}:{tile.Height}[tile{index}];");
            }

            var inputs = string.Concat(Enumerable.Range(0, _screenTiles.Length).Select(index => $"[tile{index}]"));
            var layout = string.Join('|', _screenTiles.Select(tile =>
                $"{tile.Left - _desktopBounds.Left}_{tile.Top - _desktopBounds.Top}"));
            graph.Append($"{inputs}xstack=inputs={_screenTiles.Length}:layout={layout}:fill=black,setsar=1");
            filters.Add($"lavfi=[{graph}]");
        }
        if (filters.Count > 0)
            AddArgument(startInfo, $"--vf={string.Join(',', filters)}");
        AddArgument(startInfo, "--video-sync=display-resample");
        // FFmpeg's split/scale/xstack filters cannot consume native D3D11
        // surfaces directly. Copy mode keeps hardware decoding enabled while
        // exposing frames to the tiling filter. Span mode stays zero-copy.
        AddArgument(startInfo, _screenTiles.Length > 1 ? "--hwdec=d3d11va-copy" : "--hwdec=d3d11va");
        AddArgument(startInfo, "--vo=gpu");
        AddArgument(startInfo, "--gpu-api=d3d11");
        AddArgument(startInfo, "--cache=no");
        AddArgument(startInfo, "--demuxer-max-bytes=16MiB");
        AddArgument(startInfo, "--demuxer-max-back-bytes=2MiB");
        AddArgument(startInfo, "--demuxer-readahead-secs=1");
        AddArgument(startInfo, _path);
        _player = Process.Start(startInfo) ?? throw new InvalidOperationException("mpv 播放器启动失败");
    }

    public async Task<bool> WaitUntilReadyAsync(TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (_player is null) return false;
            if (_player.HasExited)
                throw new InvalidOperationException($"mpv 播放器已退出，代码 {_player.ExitCode}");

            try
            {
                using var attempt = new CancellationTokenSource(TimeSpan.FromMilliseconds(300));
                await using var pipe = new NamedPipeClientStream(".", _pipeName, PipeDirection.InOut,
                    PipeOptions.Asynchronous);
                await pipe.ConnectAsync(attempt.Token);
                await using var writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, leaveOpen: true)
                    { AutoFlush = true };
                using var reader = new StreamReader(pipe, Encoding.UTF8, false, 1024, leaveOpen: true);
                await writer.WriteLineAsync("{\"command\":[\"get_property\",\"vo-configured\"]}");
                var response = await reader.ReadLineAsync(attempt.Token);
                if (!string.IsNullOrWhiteSpace(response))
                {
                    using var json = JsonDocument.Parse(response);
                    if (json.RootElement.TryGetProperty("data", out var data) && data.ValueKind == JsonValueKind.True)
                    {
                        if (_screenTiles.Length <= 1) return true;

                        await writer.WriteLineAsync("{\"command\":[\"get_property\",\"video-out-params\"]}");
                        var videoParamsResponse = await reader.ReadLineAsync(attempt.Token);
                        if (HasExpectedTiledOutput(videoParamsResponse)) return true;
                    }
                }
            }
            catch (OperationCanceledException) { }
            catch (IOException) { }

            await Task.Delay(50);
        }

        return false;
    }

    private bool HasExpectedTiledOutput(string? response)
    {
        if (string.IsNullOrWhiteSpace(response)) return false;
        using var json = JsonDocument.Parse(response);
        if (!json.RootElement.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object)
            return false;
        return data.TryGetProperty("w", out var width) && width.GetInt32() == _desktopBounds.Width &&
               data.TryGetProperty("h", out var height) && height.GetInt32() == _desktopBounds.Height;
    }

    public void UpdateSource(string path, bool loop, bool muted)
    {
        _path = path;
        _loop = loop;
        _muted = muted;
        _player?.Kill(entireProcessTree: true);
        _player?.Dispose();
        _player = null;
        StartPlayback();
    }

    public void StopPlayback()
    {
        var player = _player;
        _player = null;
        try
        {
            if (player is { HasExited: false })
            {
                player.Kill(entireProcessTree: true);
                player.WaitForExit(3000);
            }
        }
        catch (InvalidOperationException)
        {
        }
        catch (Win32Exception)
        {
        }
        finally { player?.Dispose(); }
    }

    private static void AddArgument(ProcessStartInfo info, string value) => info.ArgumentList.Add(value);

    protected override void OnClosed(EventArgs e)
    {
        try { StopPlayback(); }
        finally { base.OnClosed(e); }
    }
}
