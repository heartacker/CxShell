using System;
using System.Collections.Generic;
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
            _tab = value;
            DataContext = value;
            if (FloatingBar != null)
                FloatingBar.SingleTitle = value?.Title ?? string.Empty;
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
}
