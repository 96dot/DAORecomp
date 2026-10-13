param([int]$Seconds = 20, [string]$Match = "Dragon Age: Origins")
# Logs the outer and client size, position, visibility and DPI of windows whose title
# contains $Match, on every change, for $Seconds (window size and fullscreen bugs).
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$last = @{}
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
  $found = @{}
  [W]::EnumWindows({ param($h, $l)
    $sb = New-Object Text.StringBuilder 256
    [void][W]::GetWindowText($h, $sb, 256)
    $t = $sb.ToString()
    if ($t -like "*$Match*") {
      $r = New-Object W+RECT; $c = New-Object W+RECT
      [void][W]::GetWindowRect($h, [ref]$r); [void][W]::GetClientRect($h, [ref]$c)
      $v = [W]::IsWindowVisible($h)
      $found["$h"] = "$t | visible=$v outer=$($r.R-$r.L)x$($r.B-$r.T) at $($r.L),$($r.T) client=$($c.R)x$($c.B) dpi=$([W]::GetDpiForWindow($h))"
    }
    return $true }, [IntPtr]::Zero) | Out-Null
  foreach ($k in $found.Keys) {
    if ($last[$k] -ne $found[$k]) { "{0,7:N2}s {1}: {2}" -f $sw.Elapsed.TotalSeconds, $k, $found[$k]; $last[$k] = $found[$k] }
  }
  foreach ($k in @($last.Keys)) { if (-not $found.ContainsKey($k)) { "{0,7:N2}s {1}: gone" -f $sw.Elapsed.TotalSeconds, $k; $last.Remove($k) } }
  Start-Sleep -Milliseconds 30
}
