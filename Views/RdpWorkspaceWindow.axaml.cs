using System;
using System.Collections.ObjectModel;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using CommunityToolkit.Mvvm.ComponentModel;
using CxShell.ViewModels;

namespace CxShell.Views;

public partial class RdpWorkspaceTabItem : ObservableObject
{
    private static readonly IBrush SelectedBrush = new SolidColorBrush(Color.Parse("#2B3440"));
    private static readonly IBrush UnselectedBrush = Brushes.Transparent;

    public RdpWorkspaceTabItem(TerminalTabViewModel tab)
    {
        Tab = tab;
    }

    public TerminalTabViewModel Tab { get; }
    public string Title => Tab.Title;

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(TabBackground))]
    [NotifyPropertyChangedFor(nameof(TitleFontWeight))]
    private bool _isSelected;

    public IBrush TabBackground => IsSelected ? SelectedBrush : UnselectedBrush;
    public FontWeight TitleFontWeight => IsSelected ? FontWeight.SemiBold : FontWeight.Normal;
}

/// <summary>
/// Standalone multi-tab window dedicated to hosting RDP sessions outside the main window.
/// </summary>
public partial class RdpWorkspaceWindow : AtomUI.Desktop.Controls.Window
{
    private static RdpWorkspaceWindow? _instance;
    private readonly ObservableCollection<RdpWorkspaceTabItem> _tabs = [];
    private WindowState _windowStateBeforeFullScreen = WindowState.Normal;

    public RdpWorkspaceWindow()
    {
        InitializeComponent();
        TabStripItems.ItemsSource = _tabs;
        RdpViewsItems.ItemsSource = _tabs;
        Closed += OnClosed;
        PropertyChanged += OnWindowPropertyChanged;
        AddHandler(KeyDownEvent, OnWindowKeyDown, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    public static void OpenTab(TerminalTabViewModel tab)
    {
        var window = _instance;
        if (window == null)
        {
            window = new RdpWorkspaceWindow();
            _instance = window;
            window.Show();
        }

        window.AddAndSelectTab(tab);

        if (window.WindowState == WindowState.Minimized)
            window.WindowState = WindowState.Normal;

        window.Activate();
    }

    public static void CloseAll()
    {
        var window = _instance;
        _instance = null;
        window?.Close();
    }

    private void AddAndSelectTab(TerminalTabViewModel tab)
    {
        var item = new RdpWorkspaceTabItem(tab);
        _tabs.Add(item);
        SelectTab(item);
    }

    private void SelectTab(RdpWorkspaceTabItem item)
    {
        foreach (var existing in _tabs)
            existing.IsSelected = ReferenceEquals(existing, item);

        Title = $"{item.Title} - RDP";
    }

    private void CloseTabItem(RdpWorkspaceTabItem item)
    {
        var wasSelected = item.IsSelected;
        var index = _tabs.IndexOf(item);
        if (index < 0)
            return;

        _tabs.RemoveAt(index);
        try
        {
            item.Tab.Dispose();
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"RDP workspace tab cleanup failed: {ex.Message}");
        }

        if (_tabs.Count == 0)
        {
            Close();
            return;
        }

        if (wasSelected)
        {
            var nextIndex = Math.Min(index, _tabs.Count - 1);
            SelectTab(_tabs[nextIndex]);
        }
    }

    private void TabItem_OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (sender is Control { DataContext: RdpWorkspaceTabItem item })
            SelectTab(item);
    }

    private void CloseTabButton_OnClick(object? sender, RoutedEventArgs e)
    {
        if (sender is Control { DataContext: RdpWorkspaceTabItem item })
        {
            e.Handled = true;
            CloseTabItem(item);
        }
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        if (ReferenceEquals(_instance, this))
            _instance = null;

        foreach (var item in _tabs.ToArray())
        {
            try
            {
                item.Tab.Dispose();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"RDP workspace tab cleanup failed: {ex.Message}");
            }
        }

        _tabs.Clear();
    }

    private void OnWindowPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property != WindowStateProperty)
            return;

        if (WindowState != WindowState.FullScreen)
            _windowStateBeforeFullScreen = WindowState;
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

        if (WindowState == WindowState.FullScreen)
            ExitFullScreen();
        else
            WindowState = WindowState.FullScreen;

        e.Handled = true;
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
