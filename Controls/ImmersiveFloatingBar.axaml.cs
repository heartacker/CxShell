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
    private readonly DispatcherTimer _hideTimer;
    private bool _isImmersiveActive;
    private bool _isPointerOver;
    private bool _isPinned;
    private bool _showTabSwitcher;

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

    public event Action<object>? TabSelected;
    public event Action<object>? TabCloseRequested;
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
            if (FloatingBarPanel != null)
                FloatingBarPanel.IsVisible = false;
            return;
        }

        if (FloatingBarPanel != null)
            FloatingBarPanel.IsVisible = true;

        if (!_isPinned)
        {
            _hideTimer.Interval = TimeSpan.FromMilliseconds(1500);
            _hideTimer.Stop();
            _hideTimer.Start();
        }
    }

    public void StopTimer()
    {
        _hideTimer.Stop();
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
        if (_isPinned)
            return;

        _hideTimer.Interval = TimeSpan.FromMilliseconds(750);
        _hideTimer.Stop();
        _hideTimer.Start();
    }

    private void HideIfNeeded()
    {
        _hideTimer.Stop();
        if (_isPointerOver || _isPinned)
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

    private void MinimizeButton_OnClick(object? sender, RoutedEventArgs e) => MinimizeRequested?.Invoke();
    private void RestoreButton_OnClick(object? sender, RoutedEventArgs e) => RestoreRequested?.Invoke();
    private void FullScreenButton_OnClick(object? sender, RoutedEventArgs e) => ToggleFullScreenRequested?.Invoke();
    private void CloseButton_OnClick(object? sender, RoutedEventArgs e) => CloseWindowRequested?.Invoke();
}
