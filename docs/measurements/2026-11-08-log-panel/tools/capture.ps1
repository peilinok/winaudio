param([int]$CapturePid, [string]$OutputPath)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class LogPanelCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y, int width, int height, uint flags);
}
'@
$captureProcess = Get-Process -Id $CapturePid
$captureWindow = $captureProcess.MainWindowHandle
if ($captureWindow -eq [IntPtr]::Zero) { throw 'The owned GUI has no window' }
[void][LogPanelCapture]::SetProcessDPIAware()
$captureWidth = [Math]::Min(1600, [LogPanelCapture]::GetSystemMetrics(0) - 40)
$captureHeight = [Math]::Min(960, [LogPanelCapture]::GetSystemMetrics(1) - 80)
if (-not [LogPanelCapture]::SetWindowPos($captureWindow, [IntPtr]::Zero, 10, 10, $captureWidth, $captureHeight, 4)) {
    throw 'Cannot fit owned screenshot window on screen'
}
[void][LogPanelCapture]::SetForegroundWindow($captureWindow)
Start-Sleep -Milliseconds 300
$rect = [LogPanelCapture+Rect]::new()
$point = [LogPanelCapture+Point]::new()
if (-not [LogPanelCapture]::GetClientRect($captureWindow, [ref]$rect) -or
    -not [LogPanelCapture]::ClientToScreen($captureWindow, [ref]$point)) { throw 'Cannot locate owned GUI' }
$bitmap = [Drawing.Bitmap]::new($rect.Right, $rect.Bottom)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
try {
    $graphics.CopyFromScreen($point.X, $point.Y, 0, 0, $bitmap.Size)
    $bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)
} finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}
