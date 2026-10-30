# Moves the game window off the visible screen (it keeps rendering; shotgame.ps1 still captures it).
# offscreen.ps1 [-Back]   (-Back puts it back at 100,100)
param([switch]$Back)
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WO {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
for ($i = 0; $i -lt 60; $i++) {
    $p = Get-Process TWFC -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($p) { break }
    Start-Sleep 1
}
if (-not $p) { "no game window"; return }
$x = if ($Back) { 100 } else { -4000 }
# SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
[WO]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, $x, 100, 0, 0, 0x15) | Out-Null
"moved to x=$x"
