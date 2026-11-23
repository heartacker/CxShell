using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using CxShell.Models;
using Renci.SshNet;

namespace CxShell.Services;

public sealed class RdpFramebufferEventArgs : EventArgs
{
    public RdpFramebufferEventArgs(int width, int height, int stride, byte[] pixels)
    {
        Width = width;
        Height = height;
        Stride = stride;
        Pixels = pixels;
    }

    public int Width { get; }
    public int Height { get; }
    public int Stride { get; }
    public byte[] Pixels { get; }
}

public sealed class RdpBridgeClient : IDisposable
{
    private const string LibraryName = "CxRdpBridge";
    public const uint ClipboardImageFormatPng = 1;
    public const uint ClipboardImageFormatDib = 2;
    private const uint DriveRedirectionCapability = 0x00000002;
    private const uint AudioPlaybackCapability = 0x00000004;
    private const uint MicrophoneCapability = 0x00000008;
    private const uint KeyboardHookCapability = 0x00000010;
    private const uint DisplayScaleCapability = 0x00000020;
    private const uint SecurityModeCapability = 0x00000040;
    private const uint ClipboardImageCapability = 0x00000080;
    private const uint ClipboardFilesCapability = 0x00000100;
    private const uint DynamicDisplayCapability = 0x00000200;
    private const uint PointerShapeCapability = 0x00000400;
    private const uint DirtyRectCapability = 0x00000800;
    private const uint ClipboardRichTextCapability = 0x00001000;
    private const uint GraphicsOptionsCapability = 0x00002000;
    private const uint SyncLockKeysCapability = 0x00004000;
    private const uint ClipboardLockCapability = 0x00008000;
    private const uint AudioRedirectionApiVersion = 3;
    private const uint KeyboardOptionsApiVersion = 4;
    private const uint DisplayScaleApiVersion = 5;
    private const uint SecurityModeApiVersion = 6;
    private const uint ClipboardImageApiVersion = 7;
    private const uint ClipboardFilesApiVersion = 8;
    private const uint DynamicDisplayApiVersion = 9;
    private const uint PointerShapeApiVersion = 10;
    private const uint DirtyRectApiVersion = 11;
    private const uint ClipboardRichTextApiVersion = 12;
    private const uint GraphicsOptionsApiVersion = 13;
    private const uint SyncLockKeysApiVersion = 14;
    private const uint ClipboardLockApiVersion = 15;
    private static readonly object DebugLogLock = new();
    private static readonly object NativeLoadFailureLock = new();
    private static readonly bool DetailedDebugLogEnabled = IsEnvironmentFlagEnabled("CXSHELL_RDP_DEBUG_LOG");
    private static string? _lastNativeLoadFailure;
    private readonly FrameCallback _frameCallback;
    private readonly FrameRectCallback _frameRectCallback;
    private readonly StatusCallback _statusCallback;
    private readonly DisconnectCallback _disconnectCallback;
    private readonly ClipboardTextCallback _clipboardTextCallback;
    private readonly ClipboardImageCallback _clipboardImageCallback;
    private readonly ClipboardFilesCallback _clipboardFilesCallback;
    private readonly PointerCallback _pointerCallback;
    private bool _supportsClipboardImage;
    private bool _supportsClipboardFiles;
    private bool _supportsDynamicDisplay;
    private bool _supportsClipboardRichText;
    private bool _supportsSyncLockKeys;
    private IntPtr _handle;
    private SshClient? _sshTunnelClient;
    private ForwardedPortLocal? _sshTunnelPort;
    private LoopbackProxyRelay? _proxyRelay;
    private string? _lastAttemptedSecurityMode;

    static RdpBridgeClient()
    {
        NativeLibrary.SetDllImportResolver(typeof(RdpBridgeClient).Assembly, ResolveNativeLibrary);
    }

    public event EventHandler<RdpFramebufferEventArgs>? FramebufferUpdated;
    public event Action<int, int, int, IntPtr>? RawFrameReceived;
    public event Action<int, int, int, IntPtr, int, int, int, int>? RawFrameRectReceived;
    public event Action<string>? StatusChanged;
    public event Action? Disconnected;
    public event Action<string>? ClipboardTextReceived;
    public event Action<uint, byte[]>? ClipboardImageReceived;
    public event Action<string[]>? ClipboardFilesReceived;
    public event Action<int, int, int, int, int, byte[]?>? PointerShapeChanged;
    public event Action<string>? SecurityModeConnected;

    public static string DebugLogPath => Path.Combine(GetDebugLogDirectory(), "rdp-debug.log");

    public static string? LastNativeLoadFailure
    {
        get
        {
            lock (NativeLoadFailureLock)
                return _lastNativeLoadFailure;
        }
    }

    public RdpBridgeClient()
    {
        _frameCallback = OnFrame;
        _frameRectCallback = OnFrameRect;
        _statusCallback = OnStatus;
        _disconnectCallback = OnDisconnected;
        _clipboardTextCallback = OnClipboardText;
        _clipboardImageCallback = OnClipboardImage;
        _clipboardFilesCallback = OnClipboardFiles;
        _pointerCallback = OnPointer;
    }

    public void Connect(SessionInfo session, string? password, int? desktopWidth = null, int? desktopHeight = null, int desktopScaleFactor = 100)
    {
        Disconnect();

        var host = session.Host ?? string.Empty;
        var port = session.Port > 0 ? session.Port : 3389;
        var width = Math.Max(1, desktopWidth ?? session.RdpDesktopWidth);
        var height = Math.Max(1, desktopHeight ?? session.RdpDesktopHeight);
        var clampedDesktopScale = Math.Clamp(desktopScaleFactor, 100, 500);
        var deviceScale = clampedDesktopScale >= 180 ? 180 : clampedDesktopScale >= 140 ? 140 : 100;
        var driveName = NormalizeDriveName(session.RdpDriveName);
        var drivePath = string.Empty;
        var audioPlaybackMode = RdpAudioOptions.ResolvePlaybackMode(session.RdpAudioMode);
        if (session.RdpRedirectDrives)
        {
            if (string.IsNullOrWhiteSpace(session.RdpDrivePath))
                throw new InvalidOperationException("Select a local folder to redirect to the RDP session.");

            drivePath = Path.GetFullPath(session.RdpDrivePath.Trim());
            if (!Directory.Exists(drivePath))
                throw new DirectoryNotFoundException($"The RDP redirected folder does not exist: {drivePath}");
        }

        var targetDescription = $"{host}:{port}";
        var transportDescription = "direct";
        if (session.RdpUseSshTunnel)
        {
            port = StartSshTunnel(session);
            host = "127.0.0.1";
            targetDescription = $"{session.Host}:{(session.Port > 0 ? session.Port : 3389)} via SSH tunnel";
            transportDescription = "ssh-tunnel";
        }
        else
        {
            var proxy = ProxyConnectionFactory.ResolveEffectiveProxy(session.Proxy);
            if (proxy != null)
            {
                port = StartProxyRelay(session, proxy, host, port);
                host = "127.0.0.1";
                targetDescription = $"{session.Host}:{(session.Port > 0 ? session.Port : 3389)} via {proxy.TypeDisplay} proxy {proxy.Host}:{proxy.Port}";
                transportDescription = "proxy-relay";
            }
        }

        var colorDepth = ResolveColorDepth(session.RdpColorQuality);
        DebugLog($"connect start host={SanitizeLogValue(host)} port={port} target={SanitizeLogValue(targetDescription)} user={SanitizeLogValue(session.Username)} size={width}x{height} colorDepth={colorDepth} hasPassword={!string.IsNullOrEmpty(password)} transport={transportDescription}");
        _handle = NativeMethods.cxrdp_create();
        if (_handle == IntPtr.Zero)
        {
            DebugLog("connect failed: native create returned null");
            StopProxyRelay();
            StopSshTunnel();
            throw new InvalidOperationException("Failed to create RDP bridge session.");
        }

        NativeMethods.cxrdp_set_callbacks(_handle, _frameCallback, _statusCallback, _disconnectCallback, IntPtr.Zero);
        NativeMethods.cxrdp_set_clipboard_callback(_handle, _clipboardTextCallback);

        var nativeApiVersion = GetNativeApiVersion();
        var nativeCapabilities = GetNativeCapabilities();
        _supportsClipboardImage = nativeApiVersion >= ClipboardImageApiVersion &&
                                  (nativeCapabilities & ClipboardImageCapability) != 0;
        if (_supportsClipboardImage)
            NativeMethods.cxrdp_set_clipboard_image_callback(_handle, _clipboardImageCallback);
        _supportsClipboardFiles = nativeApiVersion >= ClipboardFilesApiVersion &&
                                  (nativeCapabilities & ClipboardFilesCapability) != 0;
        if (_supportsClipboardFiles)
            NativeMethods.cxrdp_set_clipboard_files_callback(_handle, _clipboardFilesCallback);
        _supportsDynamicDisplay = nativeApiVersion >= DynamicDisplayApiVersion &&
                                  (nativeCapabilities & DynamicDisplayCapability) != 0;
        _supportsClipboardRichText = nativeApiVersion >= ClipboardRichTextApiVersion &&
                                     (nativeCapabilities & ClipboardRichTextCapability) != 0;
        _supportsSyncLockKeys = nativeApiVersion >= SyncLockKeysApiVersion &&
                                (nativeCapabilities & SyncLockKeysCapability) != 0;
        if (nativeApiVersion >= PointerShapeApiVersion &&
            (nativeCapabilities & PointerShapeCapability) != 0)
        {
            NativeMethods.cxrdp_set_pointer_callback(_handle, _pointerCallback);
        }
        if (nativeApiVersion >= DirtyRectApiVersion &&
            (nativeCapabilities & DirtyRectCapability) != 0)
        {
            NativeMethods.cxrdp_set_frame_rect_callback(_handle, _frameRectCallback);
        }
        DebugLog($"native bridge apiVersion={nativeApiVersion} capabilities=0x{nativeCapabilities:X8}");
        if (session.RdpRedirectDrives)
        {
            if ((nativeCapabilities & DriveRedirectionCapability) == 0)
            {
                Disconnect();
                throw new InvalidOperationException(
                    "The installed CxRdpBridge does not support RDP drive redirection. Reinstall or update CxShell so the native bridge matches the application version.");
            }

            var driveResult = NativeMethods.cxrdp_set_drive_redirection(
                _handle,
                1,
                driveName,
                drivePath);
            if (driveResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP drive redirection configuration failed: {driveResult}"
                        : error);
            }
        }

        if (RdpAudioOptions.RequiresNativeConfiguration(audioPlaybackMode, session.RdpMicrophoneEnabled))
        {
            if (nativeApiVersion < AudioRedirectionApiVersion)
            {
                Disconnect();
                throw new InvalidOperationException(
                    "The installed CxRdpBridge does not support RDP audio settings. Reinstall or update CxShell so the native bridge matches the application version.");
            }

            if (audioPlaybackMode == RdpAudioPlaybackMode.PlayLocal &&
                (nativeCapabilities & AudioPlaybackCapability) == 0)
            {
                Disconnect();
                throw new PlatformNotSupportedException(
                    "This CxRdpBridge build does not include a local RDP audio playback backend.");
            }

            if (session.RdpMicrophoneEnabled &&
                (nativeCapabilities & MicrophoneCapability) == 0)
            {
                Disconnect();
                throw new PlatformNotSupportedException(
                    "This CxRdpBridge build does not include an RDP microphone backend.");
            }

            var audioResult = NativeMethods.cxrdp_set_audio_redirection(
                _handle,
                (int)audioPlaybackMode,
                session.RdpMicrophoneEnabled ? 1 : 0);
            if (audioResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP audio redirection configuration failed: {audioResult}"
                        : error);
            }
        }

        if (nativeApiVersion >= KeyboardOptionsApiVersion &&
            (nativeCapabilities & KeyboardHookCapability) != 0)
        {
            var keyboardResult = NativeMethods.cxrdp_set_keyboard_options(
                _handle,
                session.RdpApplyKeyCombinations ? 1 : 0);
            if (keyboardResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP keyboard configuration failed: {keyboardResult}"
                        : error);
            }
        }

        if (nativeApiVersion >= DisplayScaleApiVersion &&
            (nativeCapabilities & DisplayScaleCapability) != 0)
        {
            var scaleResult = NativeMethods.cxrdp_set_display_scale(
                _handle,
                (uint)clampedDesktopScale,
                (uint)deviceScale);
            if (scaleResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP display scale configuration failed: {scaleResult}"
                        : error);
            }
        }

        _lastAttemptedSecurityMode = null;
        if (nativeApiVersion >= SecurityModeApiVersion &&
            (nativeCapabilities & SecurityModeCapability) != 0)
        {
            var (preferredMode, allowFallback) = ResolveSecurityModeOptions(session);
            var securityResult = NativeMethods.cxrdp_set_security_mode(
                _handle,
                preferredMode,
                allowFallback ? 1 : 0);
            if (securityResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP security mode configuration failed: {securityResult}"
                        : error);
            }
        }

        if (nativeApiVersion >= GraphicsOptionsApiVersion &&
            (nativeCapabilities & GraphicsOptionsCapability) != 0)
        {
            var graphicsResult = NativeMethods.cxrdp_set_graphics_options(
                _handle,
                session.RdpEnableGraphicsAcceleration ? 1 : 0);
            if (graphicsResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP graphics options configuration failed: {graphicsResult}"
                        : error);
            }
        }

        if (nativeApiVersion >= ClipboardLockApiVersion &&
            (nativeCapabilities & ClipboardLockCapability) != 0)
        {
            var lockResult = NativeMethods.cxrdp_set_clipboard_lock_enabled(
                _handle,
                session.RdpEnableClipboardLock ? 1 : 0);
            if (lockResult != 0)
            {
                var error = GetLastError();
                Disconnect();
                throw new InvalidOperationException(
                    string.IsNullOrWhiteSpace(error)
                        ? $"RDP clipboard lock configuration failed: {lockResult}"
                        : error);
            }
        }

        var result = NativeMethods.cxrdp_connect(
            _handle,
            host,
            port,
            session.Username ?? string.Empty,
            password ?? string.Empty,
            width,
            height,
            colorDepth);

        if (result != 0)
        {
            var error = GetLastError();
            DebugLog($"connect failed result={result} error={SanitizeLogValue(error)}");
            Disconnect();
            throw new InvalidOperationException(string.IsNullOrWhiteSpace(error) ? $"RDP bridge connect failed: {result}" : error);
        }

        DebugLog("connect worker started");
    }

    private int StartSshTunnel(SessionInfo session)
    {
        var sshHost = string.IsNullOrWhiteSpace(session.RdpSshHost) ? session.Host : session.RdpSshHost.Trim();
        var sshPort = session.RdpSshPort is >= 1 and <= 65535 ? session.RdpSshPort : 22;
        var sshUser = session.RdpSshUsername?.Trim() ?? string.Empty;
        var remoteHost = string.IsNullOrWhiteSpace(session.Host) ? "127.0.0.1" : session.Host.Trim();
        var remotePort = session.Port is >= 1 and <= 65535 ? session.Port : 3389;
        var localPort = GetFreeLoopbackPort();

        if (string.IsNullOrWhiteSpace(sshHost))
            throw new InvalidOperationException("RDP SSH tunnel host is required.");
        if (string.IsNullOrWhiteSpace(sshUser))
            throw new InvalidOperationException("RDP SSH tunnel username is required.");

        var connectionInfo = new ConnectionInfo(sshHost, sshPort, sshUser, CreateRdpSshAuthMethods(session, sshUser));
        connectionInfo.Timeout = TimeSpan.FromSeconds(12);
        _sshTunnelClient = new SshClient(connectionInfo);
        SshHostKeyTrustService.Shared.Attach(
            _sshTunnelClient,
            sshHost,
            sshPort,
            session.SshAcceptAndSaveHostKey);

        DebugLog($"ssh tunnel connecting ssh={SanitizeLogValue(sshUser)}@{SanitizeLogValue(sshHost)}:{sshPort} local=127.0.0.1:{localPort} remote={SanitizeLogValue(remoteHost)}:{remotePort}");
        StatusChanged?.Invoke($"Opening SSH tunnel to {sshHost}:{sshPort}...");
        try
        {
            _sshTunnelClient.Connect();
        }
        catch (Exception ex)
        {
            var message = $"RDP SSH tunnel login failed: {ex.Message}";
            DebugLog($"ssh tunnel failed {SanitizeLogValue(ex.GetType().Name)}: {SanitizeLogValue(ex.Message)}");
            StopSshTunnel();
            throw new InvalidOperationException(message, ex);
        }

        _sshTunnelPort = new ForwardedPortLocal("127.0.0.1", (uint)localPort, remoteHost, (uint)remotePort);
        _sshTunnelPort.Exception += (_, e) => StatusChanged?.Invoke($"RDP SSH tunnel failed: {e.Exception.Message}");
        _sshTunnelClient.AddForwardedPort(_sshTunnelPort);
        _sshTunnelPort.Start();
        DebugLog($"ssh tunnel started local=127.0.0.1:{localPort} remote={SanitizeLogValue(remoteHost)}:{remotePort}");
        return localPort;
    }

    private static AuthenticationMethod[] CreateRdpSshAuthMethods(SessionInfo session, string username)
    {
        if (session.RdpSshUsePrivateKey)
        {
            if (string.IsNullOrWhiteSpace(session.RdpSshPrivateKeyPath))
                throw new InvalidOperationException("RDP SSH private key path is required.");
            return [new PrivateKeyAuthenticationMethod(username, new PrivateKeyFile(ExpandPath(session.RdpSshPrivateKeyPath)))];
        }

        var password = PasswordEncryptionService.Decrypt(session.RdpSshPassword);
        return [new PasswordAuthenticationMethod(username, password)];
    }

    private static int GetFreeLoopbackPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static string ExpandPath(string path)
    {
        if (path.StartsWith("~"))
        {
            return Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                path[2..]);
        }

        return Path.GetFullPath(path);
    }

    public void SendPointer(ushort flags, ushort x, ushort y)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.cxrdp_send_pointer(_handle, flags, x, y);
    }

    public void SendKey(uint key, bool down)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.cxrdp_send_key(_handle, key, down ? 1 : 0);
    }

    public void SendUnicodeKey(char key, bool down)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.cxrdp_send_unicode_key(_handle, key, down ? 1 : 0);
    }

    public void SyncLockKeys(uint flags)
    {
        if (_handle != IntPtr.Zero && _supportsSyncLockKeys)
            NativeMethods.cxrdp_sync_lock_keys(_handle, flags);
    }

    public void SetClipboardText(string text)
    {
        DebugLogDetailed($"clipboard set local length={text?.Length ?? 0}");
        if (_handle != IntPtr.Zero)
            NativeMethods.cxrdp_set_clipboard_text(_handle, text ?? string.Empty);
    }

    public void SetClipboardRichText(string plainText, byte[]? htmlData, byte[]? rtfData)
    {
        if (_handle == IntPtr.Zero)
            return;

        if (!_supportsClipboardRichText || ((htmlData == null || htmlData.Length == 0) && (rtfData == null || rtfData.Length == 0)))
        {
            SetClipboardText(plainText);
            return;
        }

        DebugLogDetailed($"clipboard set local rich text len={plainText?.Length ?? 0} html={htmlData?.Length ?? 0} rtf={rtfData?.Length ?? 0}");
        NativeMethods.cxrdp_set_clipboard_rich_text(
            _handle,
            plainText ?? string.Empty,
            htmlData,
            (uint)(htmlData?.Length ?? 0),
            rtfData,
            (uint)(rtfData?.Length ?? 0));
    }

    public void SetClipboardImage(byte[]? pngData, byte[]? dibData)
    {
        DebugLogDetailed($"clipboard set local image png={pngData?.Length ?? 0} dib={dibData?.Length ?? 0}");
        if (_handle == IntPtr.Zero || !_supportsClipboardImage)
            return;

        NativeMethods.cxrdp_set_clipboard_image(
            _handle,
            pngData,
            (uint)(pngData?.Length ?? 0),
            dibData,
            (uint)(dibData?.Length ?? 0));
    }

    public bool TryResizeDisplay(int width, int height, int desktopScaleFactor = 100)
    {
        if (_handle == IntPtr.Zero || !_supportsDynamicDisplay)
            return false;

        var clampedDesktopScale = Math.Clamp(desktopScaleFactor, 100, 500);
        var deviceScale = clampedDesktopScale >= 180 ? 180 : clampedDesktopScale >= 140 ? 140 : 100;
        var rc = NativeMethods.cxrdp_resize_display(
            _handle,
            width,
            height,
            (uint)clampedDesktopScale,
            (uint)deviceScale);
        DebugLogDetailed($"dynamic display resize {width}x{height} scale={clampedDesktopScale}% rc={rc}");
        return rc == 0;
    }

    public void SetClipboardFiles(string[] filePaths)
    {
        DebugLogDetailed($"clipboard set local files count={filePaths?.Length ?? 0}");
        if (_handle == IntPtr.Zero || !_supportsClipboardFiles || filePaths == null || filePaths.Length == 0)
            return;

        NativeMethods.cxrdp_set_clipboard_files(_handle, string.Join("\n", filePaths));
    }

    public void Disconnect()
    {
        if (_handle == IntPtr.Zero)
        {
            StopProxyRelay();
            StopSshTunnel();
            return;
        }

        DebugLog("disconnect requested");
        NativeMethods.cxrdp_disconnect(_handle);
        NativeMethods.cxrdp_destroy(_handle);
        _handle = IntPtr.Zero;
        StopProxyRelay();
        StopSshTunnel();
        DebugLog("disconnect completed");
    }

    /// <summary>
    /// Starts a loopback relay that forwards to <paramref name="targetHost"/>:
    /// <paramref name="targetPort"/> through the configured HTTP/SOCKS proxy and
    /// returns the local port the native bridge should connect to. FreeRDP only
    /// accepts a plain host and port, so the proxy hop is established in managed
    /// code and the bridge dials <c>127.0.0.1</c>.
    /// </summary>
    private int StartProxyRelay(SessionInfo session, ProxySettings proxy, string targetHost, int targetPort)
    {
        if (proxy.Protocol is not (ProxyProtocol.Http or ProxyProtocol.Socks4 or ProxyProtocol.Socks4A or ProxyProtocol.Socks5))
        {
            throw new InvalidOperationException(
                $"{proxy.TypeDisplay} proxies are not supported for RDP. Use an HTTP or SOCKS proxy, or enable the RDP SSH tunnel.");
        }

        StatusChanged?.Invoke($"Opening {proxy.TypeDisplay} proxy {proxy.Host}:{proxy.Port}...");
        var ipVersion = string.IsNullOrWhiteSpace(session.AdvancedIpVersion) ? "Auto" : session.AdvancedIpVersion.Trim();
        LoopbackProxyRelay relay;
        try
        {
            relay = LoopbackProxyRelay.Start(
                targetHost,
                targetPort,
                proxy,
                ipVersion,
                ex =>
                {
                    DebugLog($"proxy relay failed: {SanitizeLogValue(ex.GetType().Name)}: {SanitizeLogValue(ex.Message)}");
                    StatusChanged?.Invoke($"RDP proxy connection failed: {ex.Message}");
                });
        }
        catch (Exception ex)
        {
            DebugLog($"proxy relay start failed: {SanitizeLogValue(ex.Message)}");
            throw new InvalidOperationException($"Could not start the RDP proxy relay: {ex.Message}", ex);
        }

        _proxyRelay = relay;
        DebugLog($"proxy relay started local=127.0.0.1:{relay.LocalPort} proxy={SanitizeLogValue(proxy.TypeDisplay)} {SanitizeLogValue(proxy.Host)}:{proxy.Port} target={SanitizeLogValue(targetHost)}:{targetPort}");
        return relay.LocalPort;
    }

    private void StopProxyRelay()
    {
        var relay = _proxyRelay;
        if (relay == null)
            return;

        _proxyRelay = null;
        try
        {
            relay.Dispose();
            DebugLog("proxy relay stopped");
        }
        catch (Exception ex)
        {
            DebugLog($"proxy relay stop failed: {SanitizeLogValue(ex.Message)}");
        }
    }

    private void StopSshTunnel()
    {
        try
        {
            if (_sshTunnelPort?.IsStarted == true)
                _sshTunnelPort.Stop();
        }
        catch
        {
            // Ignore tunnel shutdown failures.
        }

        try
        {
            if (_sshTunnelClient != null && _sshTunnelPort != null)
                _sshTunnelClient.RemoveForwardedPort(_sshTunnelPort);
        }
        catch
        {
            // Ignore tunnel shutdown failures.
        }

        try
        {
            if (_sshTunnelClient?.IsConnected == true)
                _sshTunnelClient.Disconnect();
            _sshTunnelClient?.Dispose();
        }
        catch
        {
            // Ignore tunnel shutdown failures.
        }

        _sshTunnelPort = null;
        _sshTunnelClient = null;
    }

    public void Dispose()
    {
        Disconnect();
    }

    private string GetLastError()
    {
        if (_handle == IntPtr.Zero)
            return string.Empty;

        var pointer = NativeMethods.cxrdp_get_last_error(_handle);
        return pointer == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
    }

    public static string GetExpectedNativeLibraryName()
    {
        if (OperatingSystem.IsWindows())
            return "CxRdpBridge.dll";

        if (OperatingSystem.IsMacOS())
            return "libCxRdpBridge.dylib";

        return "libCxRdpBridge.so";
    }

    public static string GetNativeLibraryLoadErrorMessage(Exception exception)
    {
        var loadFailure = LastNativeLoadFailure;
        if (!string.IsNullOrWhiteSpace(loadFailure))
            return loadFailure;

        return $"{GetExpectedNativeLibraryName()} could not be loaded: {exception.Message}";
    }

    private static IntPtr ResolveNativeLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibraryName, StringComparison.Ordinal))
            return IntPtr.Zero;

        var failures = new List<string>();
        var missing = new List<string>();
        foreach (var candidate in GetNativeLibraryCandidates())
        {
            if (!File.Exists(candidate))
            {
                missing.Add(candidate);
                continue;
            }

            try
            {
                var handle = NativeLibrary.Load(candidate);
                SetLastNativeLoadFailure(null);
                return handle;
            }
            catch (Exception ex) when (ex is DllNotFoundException or BadImageFormatException)
            {
                failures.Add($"{candidate}: {ex.Message}");
            }
        }

        if (failures.Count > 0)
        {
            SetLastNativeLoadFailure(
                $"{GetExpectedNativeLibraryName()} was found, but Windows could not load it. A dependent native DLL may be missing or the architecture may be wrong. {string.Join(" | ", failures)}");
        }
        else
        {
            SetLastNativeLoadFailure(
                $"{GetExpectedNativeLibraryName()} was not found. Checked: {string.Join("; ", missing)}");
        }

        return IntPtr.Zero;
    }

    private static void SetLastNativeLoadFailure(string? message)
    {
        lock (NativeLoadFailureLock)
            _lastNativeLoadFailure = message;
    }

    private static string[] GetNativeLibraryCandidates()
    {
        var baseDirectory = AppContext.BaseDirectory;
        var fileName = GetExpectedNativeLibraryName();
        var rid = GetCurrentRid();
        var candidates = new List<string>
        {
            // Prefer the RID-specific bridge. A stale root-level DLL can be left
            // behind by an earlier local build and may expose an older API.
            Path.Combine(baseDirectory, "runtimes", rid, "native", fileName),
            Path.Combine(baseDirectory, fileName)
        };

        var processDirectory = Path.GetDirectoryName(Environment.ProcessPath);
        if (!string.IsNullOrWhiteSpace(processDirectory) &&
            !string.Equals(processDirectory, baseDirectory, StringComparison.OrdinalIgnoreCase))
        {
            candidates.Add(Path.Combine(processDirectory, "runtimes", rid, "native", fileName));
            candidates.Add(Path.Combine(processDirectory, fileName));
        }

        if (AppContext.GetData("NATIVE_DLL_SEARCH_DIRECTORIES") is string nativeSearchDirectories)
        {
            foreach (var directory in nativeSearchDirectories.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                candidates.Add(Path.Combine(directory, "runtimes", rid, "native", fileName));
                candidates.Add(Path.Combine(directory, fileName));
            }
        }

        return candidates
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static string GetCurrentRid()
    {
        var arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.Arm64 => "arm64",
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            Architecture.Arm => "arm",
            _ => RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant()
        };

        if (OperatingSystem.IsWindows())
            return $"win-{arch}";

        if (OperatingSystem.IsMacOS())
            return $"osx-{arch}";

        return $"linux-{arch}";
    }

    private void OnFrame(IntPtr userData, int width, int height, int stride, IntPtr data)
    {
        OnFrameRect(userData, width, height, stride, data, 0, 0, width, height);
    }

    private void OnFrameRect(
        IntPtr userData,
        int width,
        int height,
        int stride,
        IntPtr data,
        int dirtyX,
        int dirtyY,
        int dirtyW,
        int dirtyH)
    {
        if (width <= 0 || height <= 0 || stride <= 0 || data == IntPtr.Zero)
            return;

        RawFrameRectReceived?.Invoke(width, height, stride, data, dirtyX, dirtyY, dirtyW, dirtyH);
        RawFrameReceived?.Invoke(width, height, stride, data);

        if (FramebufferUpdated != null)
        {
            var length = checked(stride * height);
            var pixels = new byte[length];
            Marshal.Copy(data, pixels, 0, length);
            FramebufferUpdated.Invoke(this, new RdpFramebufferEventArgs(width, height, stride, pixels));
        }
    }

    private void OnStatus(IntPtr userData, IntPtr message)
    {
        var text = message == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(message) ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(text))
        {
            TrackConnectedSecurityMode(text);
            DebugLogStatus(text);
            StatusChanged?.Invoke(text);
        }
    }

    private void TrackConnectedSecurityMode(string text)
    {
        const string attemptPrefix = "RDP security attempt mode=";
        if (text.StartsWith(attemptPrefix, StringComparison.OrdinalIgnoreCase))
        {
            var rest = text[attemptPrefix.Length..];
            var spaceIndex = rest.IndexOf(' ');
            var mode = (spaceIndex >= 0 ? rest[..spaceIndex] : rest).Trim();
            if (!string.IsNullOrEmpty(mode))
                _lastAttemptedSecurityMode = NormalizeSecurityModeToken(mode);
            return;
        }

        if (string.Equals(text, "RDP connected.", StringComparison.OrdinalIgnoreCase) &&
            !string.IsNullOrEmpty(_lastAttemptedSecurityMode))
        {
            SecurityModeConnected?.Invoke(_lastAttemptedSecurityMode);
        }
    }

    private static (string PreferredMode, bool AllowFallback) ResolveSecurityModeOptions(SessionInfo session)
    {
        var configured = NormalizeSecurityModeToken(session.RdpSecurityMode);
        if (string.IsNullOrEmpty(configured) || string.Equals(configured, "auto", StringComparison.Ordinal))
        {
            var remembered = NormalizeSecurityModeToken(session.RdpLastSuccessfulSecurityMode);
            var preferred = string.IsNullOrEmpty(remembered) || string.Equals(remembered, "auto", StringComparison.Ordinal)
                ? "negotiate"
                : remembered;
            return (preferred, true);
        }

        return (configured, false);
    }

    private static string NormalizeSecurityModeToken(string? value)
    {
        var normalized = value?.Trim().ToLowerInvariant();
        return normalized switch
        {
            "auto" => "auto",
            "negotiate" => "negotiate",
            "nla" => "nla",
            "tls" => "tls",
            "rdp" => "rdp",
            _ => string.Empty
        };
    }

    private void OnDisconnected(IntPtr userData)
    {
        DebugLog("disconnected callback");
        Disconnected?.Invoke();
    }

    private void OnClipboardText(IntPtr userData, IntPtr text)
    {
        var value = text == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(text) ?? string.Empty;
        DebugLogDetailed($"clipboard received remote length={value.Length}");
        ClipboardTextReceived?.Invoke(value);
    }

    private void OnClipboardImage(IntPtr userData, uint imageFormat, IntPtr data, uint length)
    {
        if (data == IntPtr.Zero || length == 0)
            return;

        var bytes = new byte[length];
        Marshal.Copy(data, bytes, 0, (int)length);
        DebugLogDetailed($"clipboard received remote image format={imageFormat} bytes={length}");
        ClipboardImageReceived?.Invoke(imageFormat, bytes);
    }

    private void OnClipboardFiles(IntPtr userData, IntPtr pathsPtr)
    {
        if (pathsPtr == IntPtr.Zero)
            return;

        var raw = Marshal.PtrToStringUTF8(pathsPtr);
        if (string.IsNullOrWhiteSpace(raw))
            return;

        var paths = raw.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (paths.Length == 0)
            return;

        DebugLogDetailed($"clipboard received remote files count={paths.Length}");
        ClipboardFilesReceived?.Invoke(paths);
    }

    private void OnPointer(IntPtr userData, int kind, int width, int height, int hotX, int hotY, IntPtr bgraPixels)
    {
        byte[]? pixels = null;
        if (kind == 2 && width > 0 && height > 0 && bgraPixels != IntPtr.Zero)
        {
            var byteCount = checked(width * height * 4);
            pixels = new byte[byteCount];
            Marshal.Copy(bgraPixels, pixels, 0, byteCount);
        }

        PointerShapeChanged?.Invoke(kind, width, height, hotX, hotY, pixels);
    }

    private static string GetDebugLogDirectory()
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(root))
            root = AppContext.BaseDirectory;

        return Path.Combine(root, "CxShell", "Logs");
    }

    private static void DebugLog(string message)
    {
        try
        {
            Directory.CreateDirectory(GetDebugLogDirectory());
            var line = $"{DateTimeOffset.Now:O} {message}{Environment.NewLine}";
            lock (DebugLogLock)
                File.AppendAllText(DebugLogPath, line, Encoding.UTF8);
        }
        catch
        {
            // Debug logging must not break the RDP session.
        }
    }

    private static void DebugLogDetailed(string message)
    {
        if (DetailedDebugLogEnabled)
            DebugLog(message);
    }

    private static void DebugLogStatus(string status)
    {
        if (DetailedDebugLogEnabled || ShouldLogStatus(status))
            DebugLog($"status {SanitizeLogValue(status)}");
    }

    private static bool ShouldLogStatus(string status)
    {
        if (!status.StartsWith("RDP clipboard ", StringComparison.OrdinalIgnoreCase))
            return true;

        return status.Contains("failed", StringComparison.OrdinalIgnoreCase) ||
               status.Contains("closed", StringComparison.OrdinalIgnoreCase) ||
               status.Contains("disabled", StringComparison.OrdinalIgnoreCase) ||
               status.Contains("waiting for server MonitorReady", StringComparison.OrdinalIgnoreCase) ||
               status.StartsWith("RDP clipboard channel ready", StringComparison.OrdinalIgnoreCase);
    }

    private static bool IsEnvironmentFlagEnabled(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        return !string.IsNullOrWhiteSpace(value) &&
               !string.Equals(value, "0", StringComparison.OrdinalIgnoreCase) &&
               !string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) &&
               !string.Equals(value, "off", StringComparison.OrdinalIgnoreCase) &&
               !string.Equals(value, "no", StringComparison.OrdinalIgnoreCase);
    }

    private static string SanitizeLogValue(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return "<empty>";

        return value.Replace("\r", "\\r", StringComparison.Ordinal)
            .Replace("\n", "\\n", StringComparison.Ordinal);
    }

    private static int ResolveColorDepth(string? value)
    {
        return int.TryParse(value, out var parsed) && parsed is 15 or 16 or 24 or 32
            ? parsed
            : 32;
    }

    private static string NormalizeDriveName(string? value)
    {
        var name = string.IsNullOrWhiteSpace(value) ? "CxShell" : value.Trim();
        return name.Length <= 32 ? name : name[..32];
    }

    private static uint GetNativeApiVersion()
    {
        try
        {
            return NativeMethods.cxrdp_get_api_version();
        }
        catch (EntryPointNotFoundException)
        {
            return 1;
        }
    }

    private static uint GetNativeCapabilities()
    {
        try
        {
            return NativeMethods.cxrdp_get_capabilities();
        }
        catch (EntryPointNotFoundException)
        {
            return 0x00000001;
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FrameCallback(IntPtr userData, int width, int height, int stride, IntPtr bgraPixels);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FrameRectCallback(
        IntPtr userData,
        int width,
        int height,
        int stride,
        IntPtr bgraPixels,
        int dirtyX,
        int dirtyY,
        int dirtyW,
        int dirtyH);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void StatusCallback(IntPtr userData, IntPtr message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DisconnectCallback(IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ClipboardTextCallback(IntPtr userData, IntPtr text);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ClipboardImageCallback(IntPtr userData, uint imageFormat, IntPtr data, uint length);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ClipboardFilesCallback(IntPtr userData, IntPtr paths);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void PointerCallback(
        IntPtr userData,
        int kind,
        int width,
        int height,
        int hotX,
        int hotY,
        IntPtr bgraPixels);

    private static class NativeMethods
    {
        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint cxrdp_get_api_version();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint cxrdp_get_capabilities();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr cxrdp_create();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_destroy(IntPtr handle);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_callbacks(
            IntPtr handle,
            FrameCallback frameCallback,
            StatusCallback statusCallback,
            DisconnectCallback disconnectCallback,
            IntPtr userData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_callback(
            IntPtr handle,
            ClipboardTextCallback clipboardTextCallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_image_callback(
            IntPtr handle,
            ClipboardImageCallback clipboardImageCallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_files_callback(
            IntPtr handle,
            ClipboardFilesCallback clipboardFilesCallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_pointer_callback(
            IntPtr handle,
            PointerCallback pointerCallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_frame_rect_callback(
            IntPtr handle,
            FrameRectCallback frameRectCallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public static extern int cxrdp_connect(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string host,
            int port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string username,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string password,
            int width,
            int height,
            int colorDepth);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_disconnect(IntPtr handle);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_send_pointer(IntPtr handle, ushort flags, ushort x, ushort y);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_send_key(IntPtr handle, uint key, int down);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_send_unicode_key(IntPtr handle, ushort code, int down);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_sync_lock_keys(IntPtr handle, uint flags);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_text(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_rich_text(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string plainText,
            byte[]? htmlData,
            uint htmlLength,
            byte[]? rtfData,
            uint rtfLength);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_image(
            IntPtr handle,
            byte[]? pngData,
            uint pngLength,
            byte[]? dibData,
            uint dibLength);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void cxrdp_set_clipboard_files(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string filePathsNewlineSeparated);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_drive_redirection(
            IntPtr handle,
            int enabled,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string driveName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string localPath);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_audio_redirection(
            IntPtr handle,
            int playbackMode,
            int microphoneEnabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_keyboard_options(
            IntPtr handle,
            int applyKeyCombinationsRemotely);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_display_scale(
            IntPtr handle,
            uint desktopScaleFactor,
            uint deviceScaleFactor);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_resize_display(
            IntPtr handle,
            int width,
            int height,
            uint desktopScaleFactor,
            uint deviceScaleFactor);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_security_mode(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string preferredMode,
            int allowFallback);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_graphics_options(
            IntPtr handle,
            int enableGraphicsAcceleration);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int cxrdp_set_clipboard_lock_enabled(
            IntPtr handle,
            int enabled);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr cxrdp_get_last_error(IntPtr handle);
    }
}
