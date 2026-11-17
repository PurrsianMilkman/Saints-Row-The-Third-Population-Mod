# Send hardware-level key presses (scan codes via SendInput) to the Saints Row window. Works with DirectInput games.
#   . .\keys.ps1 ; Focus-Game ; Send-Key 0x1C   (Enter)      Hold-Key 0x11 3000  (W for 3 s)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class KB {
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=28)] public struct IN { [FieldOffset(0)] public uint type; [FieldOffset(4)] public KI ki; }
  [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] i, int size);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct MI { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit, Size=28)] public struct INM { [FieldOffset(0)] public uint type; [FieldOffset(4)] public MI mi; }
  [DllImport("user32.dll", EntryPoint="SendInput")] static extern uint SendInputM(uint n, INM[] i, int size);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  public static void Mouse(int x, int y, uint flags) {   // absolute screen pixel position + MOUSEEVENTF flags
    INM[] a = new INM[1]; a[0].type = 0;
    a[0].mi.dx = (int)((x * 65535L) / (GetSystemMetrics(0) - 1)); a[0].mi.dy = (int)((y * 65535L) / (GetSystemMetrics(1) - 1));
    a[0].mi.flags = flags | 0x8000u | 0x0001u;   // ABSOLUTE | MOVE
    SendInputM(1, a, Marshal.SizeOf(typeof(INM)));
  }
  public static void Key(ushort scan, bool up) {
    IN[] a = new IN[1]; a[0].type = 1; a[0].ki.scan = scan; a[0].ki.flags = 0x0008u | (up ? 0x0002u : 0u);
    SendInput(1, a, Marshal.SizeOf(typeof(IN)));
  }
}
"@
function Focus-Game {
    $p = Get-Process SaintsRow* -ea 0 | select -First 1; if (-not $p) { return $false }
    $h = $p.MainWindowHandle
    for ($i = 0; $i -lt 5 -and [KB]::GetForegroundWindow() -ne $h; $i++) {
        [KB]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [KB]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)   # tap Alt: lets us take the foreground
        [KB]::ShowWindow($h, 9) | Out-Null; [KB]::BringWindowToTop($h) | Out-Null; [KB]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 400
    }
    return ([KB]::GetForegroundWindow() -eq $h)
}
function Send-Key([int]$scan) { [KB]::Key($scan, $false); Start-Sleep -Milliseconds 80; [KB]::Key($scan, $true); Start-Sleep -Milliseconds 150 }
function Hold-Key([int]$scan, [int]$ms) { [KB]::Key($scan, $false); Start-Sleep -Milliseconds $ms; [KB]::Key($scan, $true) }

# click at a screen position given as fractions of the primary screen (0..1)
function Click-At([double]$fx, [double]$fy) {
    $w = [KB]::GetSystemMetrics(0); $h = [KB]::GetSystemMetrics(1); $x = [int]($w * $fx); $y = [int]($h * $fy)
    [KB]::Mouse($x, $y, 0); Start-Sleep -Milliseconds 200; [KB]::Mouse($x, $y, 0x0002); Start-Sleep -Milliseconds 90; [KB]::Mouse($x, $y, 0x0004); Start-Sleep -Milliseconds 200
}
