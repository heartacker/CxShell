using System;
using System.Collections;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Threading;

namespace CxShell.Controls;

/// <summary>
/// Shared immersive floating title bar for standalone RDP windows (both multi-tab
/// RdpWorkspaceWindow and single-tab DetachedTabWindow).
/// </summary>
public partial class ImmersiveFloatingBar : UserControl
{
    private static readonly IBrush ActionNormalBrush = new SolidColorBrush(Color.Parse("#1FFFFFFF"));
    private static readonly IBrush ActionActiveBrush = new SolidColorBrush(Color.Parse("#4464D8FF"));
    private readonly DispatcherTimer _hideTimer;
    private bool _isImmersiveActive;
    private bool _isPointerOver;
    private bool _isPinned;
    private bool _isExternalPopupOpen;
    private bool _showTabSwitcher;
    private bool _showMultiMonitorButton;
    private bool _isMultiMonitorActive;
    private bool _showTaskbarModeButton;
    private bool _showMinimizeButton = true;

    public ImmersiveFloatingBar()
    {
        InitializeComponent();
        _hideTimer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(750)
        };
        _hideTimer.Tick += (_, _) => HideIfNeeded();
    }

    public bool ShowTabSwitcher
    {
        get => _showTabSwitcher;
        set
        {
            _showTabSwitcher = value;
            if (TabSwitcherScroll != null)
                TabSwitcherScroll.IsVisible = value;
            if (SingleTitleText != null)
                SingleTitleText.IsVisible = !value;
        }
    }

    public string SingleTitle
    {
        get => SingleTitleText?.Text ?? string.Empty;
        set
        {
            if (SingleTitleText != null)
                SingleTitleText.Text = value;
        }
    }

    public IEnumerable? TabItemsSource
    {
        get => FloatingTabItems?.ItemsSource;
        set
        {
            if (FloatingTabItems != null)
                FloatingTabItems.ItemsSource = value;
        }
    }

    public bool ShowMultiMonitorButton
    {
        get => _showMultiMonitorButton;
        set
        {
            _showMultiMonitorButton = value;
            if (MultiMonitorButton != null)
                MultiMonitorButton.IsVisible = value;
            UpdateExtraActionsDivider();
        }
    }

    public bool IsMultiMonitorEnabled
    {
        get => MultiMonitorButton?.IsEnabled ?? false;
        set
        {
            if (MultiMonitorButton != null)
            {
                MultiMonitorButton.IsEnabled = value;
                MultiMonitorButton.Opacity = value ? 1.0 : 0.45;
            }
        }
    }

    public bool IsMultiMonitorActive
    {
        get => _isMultiMonitorActive;
        set
        {
            _isMultiMonitorActive = value;
            UpdateMultiMonitorVisual();
        }
    }

    public bool ShowTaskbarModeButton
    {
        get => _showTaskbarModeButton;
        set
        {
            _showTaskbarModeButton = value;
            if (TaskbarModeButton != null)
                TaskbarModeButton.IsVisible = value;
            UpdateExtraActionsDivider();
        }
    }

    public string TaskbarModeLabel
    {
        get => TaskbarModeText?.Text ?? string.Empty;
        set
        {
            if (TaskbarModeText != null)
                TaskbarModeText.Text = value;
        }
    }

    public bool ShowMinimizeButton
    {
        get => _showMinimizeButton;
        set
        {
            _showMinimizeButton = value;
            if (MinimizeButton != null)
                MinimizeButton.IsVisible = value;
        }
    }

    public string RestoreToolTip
    {
        get => RestoreButton != null ? ToolTip.GetTip(RestoreButton)?.ToString() ?? string.Empty : string.Empty;
        set
        {
            if (RestoreButton != null)
                ToolTip.SetTip(RestoreButton, value);
        }
    }

    public string FullScreenToolTip
    {
        get => FullScreenButton != null ? ToolTip.GetTip(FullScreenButton)?.ToString() ?? string.Empty : string.Empty;
        set
        {
            if (FullScreenButton != null)
                ToolTip.SetTip(FullScreenButton, value);
        }
    }

    public string CloseToolTip
    {
        get => CloseButton != null ? ToolTip.GetTip(CloseButton)?.ToString() ?? string.Empty : string.Empty;
        set
        {
            if (CloseButton != null)
                ToolTip.SetTip(CloseButton, value);
        }
    }

    public event Action<object>? TabSelected;
    public event Action<object>? TabCloseRequested;
    public event Action<Control>? MultiMonitorRequested;
    public event Action? TaskbarModeToggleRequested;
    public event Action? MinimizeRequested;
    public event Action? RestoreRequested;
    public event Action? ToggleFullScreenRequested;
    public event Action? CloseWindowRequested;
    public event Action<PointerPressedEventArgs>? DragRestoreRequested;
    public event Action? InteractionCompleted;

    public void SetImmersiveActive(bool isImmersive)
    {
        _isImmersiveActive = isImmersive;
        IsVisible = isImmersive;

        if (!isImmersive)
        {
            _hideTimer.Stop();
            _isPointerOver = false;
            _isExternalPopupOpen = false;
            UpdateMultiMonitorVisual();
            if (FloatingBarPanel != null)
                FloatingBarPanel.IsVisible = false;
            return;
        }

        if (FloatingBarPanel != null)
            FloatingBarPanel.IsVisible = true;

        if (!_isPinned && !_isExternalPopupOpen)
        {
            _hideTimer.Interval = TimeSpan.FromMilliseconds(1500);
            _hideTimer.Stop();
            _hideTimer.Start();
        }
    }

    public void SetExternalPopupOpen(bool isOpen)
    {
        _isExternalPopupOpen = isOpen;
        UpdateMultiMonitorVisual();

        if (isOpen)
        {
            _hideTimer.Stop();
            if (_isImmersiveActive && FloatingBarPanel != null)
                FloatingBarPanel.IsVisible = true;
            return;
        }

        if (_isImmersiveActive && !_isPointerOver && !_isPinned)
        {
            _hideTimer.Interval = TimeSpan.FromMilliseconds(750);
            _hideTimer.Stop();
            _hideTimer.Start();
        }
    }

    public void StopTimer()
    {
        _hideTimer.Stop();
    }

    private void UpdateExtraActionsDivider()
    {
        if (ExtraActionsDivider != null)
            ExtraActionsDivider.IsVisible = _showMultiMonitorButton || _showTaskbarModeButton;
    }

    private void UpdateMultiMonitorVisual()
    {
        if (MultiMonitorButton != null)
        {
            MultiMonitorButton.Background = (_isMultiMonitorActive || _isExternalPopupOpen)
                ? ActionActiveBrush
                : ActionNormalBrush;
        }
    }

    private void FloatingBar_OnPointerEntered(object? sender, PointerEventArgs e)
    {
        _isPointerOver = true;
        _hideTimer.Stop();

        if (_isImmersiveActive && FloatingBarPanel != null)
            FloatingBarPanel.IsVisible = true;
    }

    private void FloatingBar_OnPointerExited(object? sender, PointerEventArgs e)
    {
        _isPointerOver = false;
        if (_isPinned || _isExternalPopupOpen)
            return;

        _hideTimer.Interval = TimeSpan.FromMilliseconds(750);
        _hideTimer.Stop();
        _hideTimer.Start();
    }

    private void HideIfNeeded()
    {
        _hideTimer.Stop();
        if (_isPointerOver || _isPinned || _isExternalPopupOpen)
            return;

        if (FloatingBarPanel != null)
            FloatingBarPanel.IsVisible = false;
    }

    private void FloatingBar_OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)
            return;

        if (e.Source is Control source && IsInsideButton(source))
            return;

        DragRestoreRequested?.Invoke(e);
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

    private void PinButton_OnClick(object? sender, RoutedEventArgs e)
    {
        _isPinned = !_isPinned;
        if (PinButton != null)
        {
            PinButton.Background = _isPinned
                ? new SolidColorBrush(Color.Parse("#4464D8FF"))
                : Brushes.Transparent;
        }

        if (_isPinned)
        {
            _hideTimer.Stop();
            if (FloatingBarPanel != null)
                FloatingBarPanel.IsVisible = true;
        }
        else if (!_isPointerOver)
        {
            _hideTimer.Interval = TimeSpan.FromMilliseconds(500);
            _hideTimer.Start();
        }

        InteractionCompleted?.Invoke();
    }

    private void TabItem_OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (e.Source is Control source && IsInsideButton(source))
            return;

        if (sender is Control { DataContext: { } item })
        {
            TabSelected?.Invoke(item);
            e.Handled = true;
        }
    }

    private void CloseTabButton_OnClick(object? sender, RoutedEventArgs e)
    {
        if (sender is Control { DataContext: { } item })
        {
            e.Handled = true;
            TabCloseRequested?.Invoke(item);
        }
    }

    private void MultiMonitorButton_OnClick(object? sender, RoutedEventArgs e)
    {
        if (MultiMonitorButton != null)
            MultiMonitorRequested?.Invoke(MultiMonitorButton);
    }

    private void TaskbarModeButton_OnClick(object? sender, RoutedEventArgs e)
    {
        TaskbarModeToggleRequested?.Invoke();
        InteractionCompleted?.Invoke();
    }

    private void MinimizeButton_OnClick(object? sender, RoutedEventArgs e) => MinimizeRequested?.Invoke();
    private void RestoreButton_OnClick(object? sender, RoutedEventArgs e) => RestoreRequested?.Invoke();
    private void FullScreenButton_OnClick(object? sender, RoutedEventArgs e) => ToggleFullScreenRequested?.Invoke();
    private void CloseButton_OnClick(object? sender, RoutedEventArgs e) => CloseWindowRequested?.Invoke();
}
