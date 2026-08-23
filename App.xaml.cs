namespace DWD;

public partial class App : System.Windows.Application
{
    private const string SingleInstanceName = @"Local\DWD.SingleInstance";
    private const string ShowWindowEventName = @"Local\DWD.ShowWindow";
    private Mutex? _singleInstanceMutex;
    private EventWaitHandle? _showWindowEvent;
    private ManualResetEvent? _listenerStopEvent;
    private Task? _activationListener;
    private bool _ownsSingleInstance;

    protected override void OnStartup(System.Windows.StartupEventArgs e)
    {
        base.OnStartup(e);

        _showWindowEvent = new EventWaitHandle(false, EventResetMode.AutoReset, ShowWindowEventName);
        _singleInstanceMutex = new Mutex(false, SingleInstanceName);
        try
        {
            _ownsSingleInstance = _singleInstanceMutex.WaitOne(0);
        }
        catch (AbandonedMutexException)
        {
            _ownsSingleInstance = true;
        }

        if (!_ownsSingleInstance)
        {
            _showWindowEvent.Set();
            Shutdown();
            return;
        }

        var window = new MainWindow();
        MainWindow = window;
        window.Show();

        _listenerStopEvent = new ManualResetEvent(false);
        _activationListener = Task.Run(() => ListenForActivation(_showWindowEvent, _listenerStopEvent));
    }

    private void ListenForActivation(EventWaitHandle showWindowEvent, ManualResetEvent stopEvent)
    {
        var handles = new WaitHandle[] { showWindowEvent, stopEvent };
        while (WaitHandle.WaitAny(handles) == 0)
        {
            try
            {
                Dispatcher.BeginInvoke(() =>
                {
                    if (MainWindow is DWD.MainWindow window) window.ShowFromTray();
                });
            }
            catch (InvalidOperationException)
            {
                return;
            }
        }
    }

    protected override void OnExit(System.Windows.ExitEventArgs e)
    {
        _listenerStopEvent?.Set();
        try { _activationListener?.Wait(TimeSpan.FromSeconds(1)); }
        catch (AggregateException) { }

        if (_ownsSingleInstance)
        {
            try { _singleInstanceMutex?.ReleaseMutex(); }
            catch (ApplicationException) { }
        }

        _listenerStopEvent?.Dispose();
        _showWindowEvent?.Dispose();
        _singleInstanceMutex?.Dispose();
        base.OnExit(e);
    }
}
