using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform;
using CxShell.ViewModels;

namespace CxShell.Views;

public partial class RdpMonitorWindow : Window
{
    private readonly RdpView _ownerView;
    private readonly RdpViewModel _vm;
    private readonly Screen _screen;
    private readonly int _monitorIndex;
    private readonly Action<int> _onCloseRequested;
    private Rect _virtualSliceRect;
    private bool _keepTaskbar;
    private ushort _buttonFlags;

    public RdpMonitorWindow(
        RdpView ownerView,
        RdpViewModel vm,
        Screen screen,
        int monitorIndex,
        Rect virtualSliceRect,
        Action<int> onCloseRequested)
    {
        InitializeComponent();
        _ownerView = ownerView;
        _vm = vm;
        _screen = screen;
        _monitorIndex = monitorIndex;
        _virtualSliceRect = virtualSliceRect;
        _onCloseRequested = onCloseRequested;

        AddHandler(KeyDownEvent, OnMonitorKeyDown, RoutingStrategies.Tunnel);
        AddHandler(KeyUpEvent, OnMonitorKeyUp, RoutingStrategies.Tunnel);
        AddHandler(DragDrop.DragOverEvent, (_, e) => _ownerView.ForwardDragOver(e));
        AddHandler(DragDrop.DropEvent, (_, e) => _ownerView.ForwardDrop(e));
        GotFocus += (_, _) => _ownerView.ForwardGotFocus();

        var scalePct = (int)Math.Round(screen.Scaling * 100);
        MonitorTitleText.Text = $"屏幕 {monitorIndex + 1} · {screen.Bounds.Width}×{screen.Bounds.Height} · {scalePct}%";

        ApplyWindowGeometry();
        UpdateSlice(_virtualSliceRect);

        _vm.FramebufferInvalidated += OnFramebufferInvalidated;
        _vm.PropertyChanged += OnViewModelPropertyChanged;
        Closed += (_, _) =>
        {
            _vm.FramebufferInvalidated -= OnFramebufferInvalidated;
            _vm.PropertyChanged -= OnViewModelPropertyChanged;
        };
    }

    private void OnMonitorKeyDown(object? sender, KeyEventArgs e)
    {
        _ownerView.ForwardKeyDown(e);
    }

    private void OnMonitorKeyUp(object? sender, KeyEventArgs e)
    {
        _ownerView.ForwardKeyUp(e);
    }

    public int MonitorIndex => _monitorIndex;

    public void UpdateSlice(Rect virtualSliceRect)
    {
        _virtualSliceRect = virtualSliceRect;
        SliceSurface.Source = _vm.Framebuffer;
        SliceSurface.SourceRect = virtualSliceRect;
        SliceSurface.InvalidateVisual();
    }

    private void ApplyWindowGeometry()
    {
        var rect = _keepTaskbar ? _screen.WorkingArea : _screen.Bounds;
        var scaling = _screen.Scaling > 0 ? _screen.Scaling : 1.0;
        Position = new PixelPoint(rect.X, rect.Y);
        Width = rect.Width / scaling;
        Height = rect.Height / scaling;
        ToggleImmersiveModeButton.Content = _keepTaskbar ? "真全屏" : "保留任务栏";
    }

    private void OnFramebufferInvalidated()
    {
        SliceSurface.Source = _vm.Framebuffer;
        SliceSurface.InvalidateVisual();
    }

    private void OnViewModelPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(RdpViewModel.Framebuffer))
        {
            SliceSurface.Source = _vm.Framebuffer;
            SliceSurface.InvalidateVisual();
        }
    }

    private void OnTopHotZoneEntered(object? sender, PointerEventArgs e)
    {
        FloatingToolbar.IsVisible = true;
    }

    private void OnFloatingToolbarExited(object? sender, PointerEventArgs e)
    {
        FloatingToolbar.IsVisible = false;
    }

    private void OnToggleImmersiveModeClick(object? sender, RoutedEventArgs e)
    {
        _keepTaskbar = !_keepTaskbar;
        ApplyWindowGeometry();
    }

    private void OnCloseMonitorClick(object? sender, RoutedEventArgs e)
    {
        _onCloseRequested(_monitorIndex);
    }

    private bool TryMapPointer(PointerEventArgs e, out ushort remoteX, out ushort remoteY)
    {
        remoteX = 0;
        remoteY = 0;
        var bounds = SliceSurface.Bounds;
        if (bounds.Width <= 0 || bounds.Height <= 0 || _virtualSliceRect.Width <= 0 || _virtualSliceRect.Height <= 0)
            return false;

        var pos = e.GetPosition(SliceSurface);
        var relX = Math.Clamp(pos.X / bounds.Width, 0, 1);
        var relY = Math.Clamp(pos.Y / bounds.Height, 0, 1);
        var vx = _virtualSliceRect.X + relX * _virtualSliceRect.Width;
        var vy = _virtualSliceRect.Y + relY * _virtualSliceRect.Height;
        remoteX = (ushort)Math.Clamp((int)Math.Round(vx), 0, ushort.MaxValue);
        remoteY = (ushort)Math.Clamp((int)Math.Round(vy), 0, ushort.MaxValue);
        return true;
    }

    private void OnSurfacePointerMoved(object? sender, PointerEventArgs e)
    {
        if (TryMapPointer(e, out var x, out var y))
            _vm.SendPointer((ushort)(0x0800 | _buttonFlags), x, y);
    }

    private void OnSurfacePointerPressed(object? sender, PointerPressedEventArgs e)
    {
        SliceSurface.Focus();
        if (!TryMapPointer(e, out var x, out var y))
            return;

        var point = e.GetCurrentPoint(SliceSurface);
        ushort flag = 0x1000; // left button (also covers touch/pen primary tap)
        if (point.Properties.IsRightButtonPressed)
            flag = 0x2000;
        else if (point.Properties.IsMiddleButtonPressed)
            flag = 0x4000;

        _buttonFlags |= flag;
        _vm.SendPointer((ushort)(0x8000 | flag), x, y);
    }

    private void OnSurfacePointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!TryMapPointer(e, out var x, out var y))
            return;

        ushort flag = e.InitialPressMouseButton switch
        {
            MouseButton.Right => 0x2000,
            MouseButton.Middle => 0x4000,
            _ => 0x1000
        };

        _buttonFlags = (ushort)(_buttonFlags & ~flag);
        _vm.SendPointer(flag, x, y);
    }

    private void OnSurfacePointerWheelChanged(object? sender, PointerWheelEventArgs e)
    {
        if (!TryMapPointer(e, out var x, out var y))
            return;

        var delta = (int)Math.Round(e.Delta.Y * 120);
        if (delta == 0)
            return;

        var step = (ushort)(Math.Min(Math.Abs(delta), 0xFF) & 0xFF);
        ushort flags = (ushort)(0x0200 | step);
        if (delta < 0)
            flags |= 0x0100;

        _vm.SendPointer(flags, x, y);
    }
}
