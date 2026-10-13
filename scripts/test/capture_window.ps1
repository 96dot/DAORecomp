param([string]$out)
# Saves the DAORecomp window's contents (the launcher's, even behind other windows) to a
# PNG: capture_window.ps1 <png path>
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class C {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@
$h = [IntPtr](Get-Process DAORecomp | Select-Object -First 1).MainWindowHandle
$r = New-Object C+RECT
[C]::GetClientRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap($r.R, $r.B)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[C]::PrintWindow($h, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
"saved $out"
