# Screenshot of the game window only (no desktop, not covered by other windows): shotgame.ps1 <out.png>
# Uses PrintWindow(PW_RENDERFULLCONTENT) so overlapping windows never end up in the picture.
param([string]$out)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
$p = Get-Process TWFC -ErrorAction Stop | Select-Object -First 1
$r = New-Object W+RECT
[W]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[W]::PrintWindow($p.MainWindowHandle, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc)
$small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / 2)), ([int]($h / 2))
$small.Save($out)
