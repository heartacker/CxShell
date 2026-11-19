using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Threading;
using CxShell.ViewModels;

namespace CxShell.Views;

/// <summary>
/// Hosts a single terminal tab in its own top-level window. The tab view model
/// is moved here from the main window, so its connections, buffers, and runtime
/// state survive the move; closing this window closes that session.
/// </summary>
public partial class DetachedTabWindow : AtomUI.Desktop.Controls.Window
{
    private static readonly List<DetachedTabWindow> OpenWindows = [];
    private TerminalTabViewModel? _tab;
    private Action<TerminalTabViewModel>? _closeTab;
    private WindowState _windowStateBeforeFullScreen = WindowState.Normal;
    private readonly DispatcherTimer _floatingBarHideTimer;
    private bool _isPointerOverFloatingBar;
    private bool _isFloatingBarPinned;
    private bool _disposed;

    public DetachedTabWindow()
    {
        InitializeComponent();
        _floatingBarHideTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(750)
        };
        _floatingBarHideTimer.Tick += (_, _) => HideFloatingBarIfNeeded();

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

    /// <summary>
    /// Closes every detached window, which also releases the sessions they host.
    /// The main window calls this so detaching a tab never keeps a connection
    /// alive after the application window is gone.
    /// </summary>
    public static void CloseAll()
    {
        foreach (var window in OpenWindows.ToArray())
            window.Close();

        OpenWindows.Clear();
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        _floatingBarHideTimer.Stop();
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

    /// <summary>
    /// Remembers the last non-fullscreen window state so leaving fullscreen can
    /// restore whether the window was normal or maximized. The AtomUI caption
    /// button toggles the state itself, so evaluating the current state here also
    /// covers that path.
    /// </summary>
    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property != WindowStateProperty)
            return;

        var state = WindowState;
        if (state != WindowState.FullScreen)
            _windowStateBeforeFullScreen = state;

        UpdateImmersiveChrome(state);
    }

    private void UpdateImmersiveChrome(WindowState state)
    {
        var isImmersive = state is WindowState.Maximized or WindowState.FullScreen;
        IsTitleBarVisible = !isImmersive;

        if (FloatingTitleBarHost == null || FloatingTitleBarPanel == null)
            return;

        FloatingTitleBarHost.IsVisible = isImmersive;
        if (!isImmersive)
        {
            _floatingBarHideTimer.Stop();
            _isPointerOverFloatingBar = false;
            FloatingTitleBarPanel.IsVisible = false;
            return;
        }

        // Briefly reveal the floating title bar when entering Maximized/FullScreen
        // so the user can discover it immediately, then auto-hide unless pinned.
        FloatingTitleBarPanel.IsVisible = true;
        if (!_isFloatingBarPinned)
        {
            _floatingBarHideTimer.Interval = TimeSpan.FromMilliseconds(1500);
            _floatingBarHideTimer.Stop();
            _floatingBarHideTimer.Start();
        }
    }

    private void FloatingTitleBar_OnPointerEntered(object? sender, PointerEventArgs e)
    {
        _isPointerOverFloatingBar = true;
        _floatingBarHideTimer.Stop();

        if (FloatingTitleBarPanel != null && WindowState is WindowState.Maximized or WindowState.FullScreen)
            FloatingTitleBarPanel.IsVisible = true;
    }

    private void FloatingTitleBar_OnPointerExited(object? sender, PointerEventArgs e)
    {
        _isPointerOverFloatingBar = false;
        if (_isFloatingBarPinned)
            return;

        _floatingBarHideTimer.Interval = TimeSpan.FromMilliseconds(750);
        _floatingBarHideTimer.Stop();
        _floatingBarHideTimer.Start();
    }

    private void HideFloatingBarIfNeeded()
    {
        _floatingBarHideTimer.Stop();
        if (_isPointerOverFloatingBar || _isFloatingBarPinned)
            return;

        if (FloatingTitleBarPanel != null)
            FloatingTitleBarPanel.IsVisible = false;
    }

    private void FloatingTitleBar_OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            return;

        // Allow dragging the floating bar to restore to Normal window state.
        if (e.Source is Control source && IsInsideButton(source))
            return;

        if (WindowState is WindowState.Maximized or WindowState.FullScreen)
        {
            WindowState = WindowState.Normal;
            BeginMoveDrag(e);
            e.Handled = true;
        }
    }

    private static bool IsInsideButton(Control? control)
    {
        while (control != null)
        {
            if (control is Button)
                return true;
            control = control.Parent as Control;
        }

        return false;
    }

    private void PinFloatingBarButton_OnClick(object? sender, RoutedEventArgs e)
    {
        _isFloatingBarPinned = !_isFloatingBarPinned;
        if (PinFloatingBarButton != null)
        {
            PinFloatingBarButton.Background = _isFloatingBarPinned
                ? new SolidColorBrush(Color.Parse("#4464D8FF"))
                : Brushes.Transparent;
        }

        if (_isFloatingBarPinned)
        {
            _floatingBarHideTimer.Stop();
            if (FloatingTitleBarPanel != null)
                FloatingTitleBarPanel.IsVisible = true;
        }
        else if (!_isPointerOverFloatingBar)
        {
            _floatingBarHideTimer.Interval = TimeSpan.FromMilliseconds(500);
            _floatingBarHideTimer.Start();
        }

        FocusActiveContent();
    }

    private void MinimizeButton_OnClick(object? sender, RoutedEventArgs e)
    {
        WindowState = WindowState.Minimized;
    }

    private void RestoreButton_OnClick(object? sender, RoutedEventArgs e)
    {
        WindowState = WindowState.Normal;
        FocusActiveContent();
    }

    private void FullScreenButton_OnClick(object? sender, RoutedEventArgs e)
    {
        ToggleFullScreen();
        FocusActiveContent();
    }

    private void CloseButton_OnClick(object? sender, RoutedEventArgs e)
    {
        Close();
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
