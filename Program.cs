using System;
using Avalonia;
using Avalonia.ReactiveUI;
using AtomUI;
using CxShell.Services;
using Velopack;

namespace CxShell;

internal class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        VelopackApp.Build()
            .SetArgs(args)
            .Run();

        if (CommandLineHandoffService.TrySendToExistingInstance(args))
            return;

        BuildAvaloniaApp()
            .StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp()
    {
        return AppBuilder.Configure<App>()
            .UseReactiveUI()
            .UseAtomUIPlatformDetect()
            .With(new MacOSPlatformOptions
            {
                DisableSetProcessName = false
            })
            .WithAtomUIDefaultOptions()
            .UseAtomUI(App.ConfigureAtomUI)
            .LogToTrace();
    }
}
