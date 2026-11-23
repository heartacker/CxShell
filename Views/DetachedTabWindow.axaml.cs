using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using CxShell.ViewModels;

namespace CxShell.Views;

/// <summary>
/// Hosts a single terminal/RDP tab in its own top-level window.
/// Uses the shared ImmersiveFloatingBar (single-session mode) when maximized or full screen.
/// </summary>
public partial class DetachedTabWindow : AtomUI.Desktop.Controls.Window
{
    private static readonly List<DetachedTabWindow> OpenWindows = [];
    private TerminalTabViewModel? _tab;
    private RdpViewModel? _subscribedRdpVm;
    private Action<TerminalTabViewModel>? _closeTab;
    private WindowState _windowStateBeforeFullScreen = WindowState.Normal;
    private bool _disposed;

    public DetachedTabWindow()
    {
        InitializeComponent();

        FloatingBar.MinimizeRequested += () => WindowState = WindowState.Minimized;
        FloatingBar.RestoreRequested += () =>
        {
            WindowState = WindowState.Normal;
            FocusActiveContent();
        };
        FloatingBar.ToggleFullScreenRequested += () =>
        {
            ToggleFullScreen();
            FocusActiveContent();
        };
        FloatingBar.MultiMonitorRequested += anchor =>
            RdpContentView?.ToggleMultiMonitorPopup(anchor, PlacementMode.Bottom, FloatingBar.SetExternalPopupOpen);
        FloatingBar.CloseWindowRequested += Close;
        FloatingBar.InteractionCompleted += FocusActiveContent;
        FloatingBar.DragRestoreRequested += e =>
        {
            if (WindowState is WindowState.Maximized or WindowState.FullScreen)
            {
                WindowState = WindowState.Normal;
                BeginMoveDrag(e);
                e.Handled = true;
            }
        };

        Closed += OnClosed;
        PropertyChanged += OnWindowPropertyChanged;
        AddHandler(KeyDownEvent, OnWindowKeyDown, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    public DetachedTabWindow(TerminalTabViewModel tab, Action<TerminalTabViewModel>? closeTab = null)
        : this()
    {
        _closeTab = closeTab;
        Tab = tab;
    }

    public TerminalTabViewModel? Tab
    {
        get => _tab;
        set
        {
            if (_subscribedRdpVm != null)
            {
                _subscribedRdpVm.PropertyChanged -= OnRdpViewModelPropertyChanged;
                _subscribedRdpVm = null;
            }

            _tab = value;
            DataContext = value;
            if (value?.Rdp is { } rdp)
            {
                _subscribedRdpVm = rdp;
                rdp.PropertyChanged += OnRdpViewModelPropertyChanged;
            }

            if (FloatingBar != null)
            {
                FloatingBar.SingleTitle = value?.Title ?? string.Empty;
                FloatingBar.ShowMultiMonitorButton = value?.IsRdpSession == true;
            }

            SyncRdpMultiMonitorState();
        }
    }

    public static int OpenWindowCount => OpenWindows.Count;

    public static DetachedTabWindow ShowFor(
        TerminalTabViewModel tab,
        Action<TerminalTabViewModel>? closeTab = null,
        PixelPoint? position = null)
    {
        var window = new DetachedTabWindow(tab, closeTab);
        OpenWindows.Add(window);

        if (position != null)
        {
            window.WindowStartupLocation = WindowStartupLocation.Manual;
            window.Position = position.Value;
        }

        window.Show();
        return window;
    }

    public static void CloseAll()
    {
        foreach (var window in OpenWindows.ToArray())
            window.Close();

        OpenWindows.Clear();
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        FloatingBar?.StopTimer();
        OpenWindows.Remove(this);
        DisposeTab();
    }

    private void DisposeTab()
    {
        if (_disposed)
            return;

        _disposed = true;
        if (_subscribedRdpVm != null)
        {
            _subscribedRdpVm.PropertyChanged -= OnRdpViewModelPropertyChanged;
            _subscribedRdpVm = null;
        }
        var tab = _tab;
        _tab = null;
        DataContext = null;

        if (tab == null)
            return;

        try
        {
            if (_closeTab != null)
                _closeTab(tab);
            else
                tab.Dispose();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Detached tab cleanup failed: {ex.Message}");
        }
        finally
        {
            _closeTab = null;
        }
    }

    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property != WindowStateProperty)
            return;

        var state = WindowState;
        if (state != WindowState.FullScreen)
            _windowStateBeforeFullScreen = state;

        var isImmersive = state is WindowState.Maximized or WindowState.FullScreen;
        IsTitleBarVisible = !isImmersive;
        WindowDecorations = state == WindowState.FullScreen
            ? WindowDecorations.None
            : WindowDecorations.Full;
        FloatingBar?.SetImmersiveActive(isImmersive);
    }

    private void FocusActiveContent()
    {
        if (RdpContentView?.IsVisible == true)
            RdpContentView.Focus();
        else if (TerminalContentView?.IsVisible == true)
            TerminalContentView.Focus();
        else if (VncContentView?.IsVisible == true)
            VncContentView.Focus();
    }

    private void OnWindowKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && WindowState == WindowState.FullScreen)
        {
            ExitFullScreen();
            e.Handled = true;
            return;
        }

        if (e.Key != Key.F11)
            return;

        ToggleFullScreen();
        e.Handled = true;
    }

    private void ToggleFullScreen()
    {
        if (WindowState == WindowState.FullScreen)
        {
            ExitFullScreen();
            return;
        }

        WindowState = WindowState.FullScreen;
    }

    private void ExitFullScreen()
    {
        if (WindowState != WindowState.FullScreen)
            return;

        WindowState = _windowStateBeforeFullScreen == WindowState.FullScreen
            ? WindowState.Normal
            : _windowStateBeforeFullScreen;
    }

    private void OnRdpViewModelPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(RdpViewModel.IsConnected) or nameof(RdpViewModel.IsMultiMonitorActive))
            SyncRdpMultiMonitorState();
    }

    private void SyncRdpMultiMonitorState()
    {
        var connected = _subscribedRdpVm?.IsConnected == true;
        var active = _subscribedRdpVm?.IsMultiMonitorActive == true;
        if (FloatingBar != null)
        {
            FloatingBar.IsMultiMonitorEnabled = connected;
            FloatingBar.IsMultiMonitorActive = active;
        }
        if (TitleBarMultiMonitorButton != null)
            TitleBarMultiMonitorButton.IsEnabled = connected;
    }

    private void OnTitleBarMultiMonitorClick(object? sender, RoutedEventArgs e)
    {
        if (sender is Control anchor)
            RdpContentView?.ToggleMultiMonitorPopup(anchor, PlacementMode.Bottom);
    }
}
