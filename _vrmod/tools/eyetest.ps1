# Debug: per-eye brightness of a screen region over several window shots (game on MockXR, windoweye debug command).
# eyetest.ps1 -Setup "rightvs 0" [-Shots 6] [-Fire]   prints mean brightness of the fire region for eye 1 and eye 2
param([string]$Setup = "", [int]$Shots = 6, [switch]$Fire, [double]$X0 = 0.40, [double]$X1 = 0.60, [double]$Y0 = 0.50, [double]$Y1 = 0.70)
Add-Type -AssemblyName System.Drawing
$cmd = "C:\Games\Transformers - War for Cybertron\BinariesVR\twfcvr_cmd.txt"
$t = $PSScriptRoot
function Get-Bright([string]$f) {
    $b = [System.Drawing.Bitmap]::FromFile($f)
    $sum = 0.0; $n = 0
    for ($y = [int]($b.Height * $Y0); $y -lt [int]($b.Height * $Y1); $y += 2) {
        for ($x = [int]($b.Width * $X0); $x -lt [int]($b.Width * $X1); $x += 2) {
            $c = $b.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3.0; $n++
        }
    }
    $b.Dispose()
    return $sum / $n
}
$res = @{ 1 = @(); 2 = @() }
foreach ($eye in 1, 2) {
    $pre = ($Setup -replace ';', "`r`n") + "`r`nwindoweye $eye`r`n"
    if ($Fire) { $pre += "press RT 6000`r`n" }
    [IO.File]::WriteAllText($cmd, $pre)
    Start-Sleep -Milliseconds 1500
    for ($i = 0; $i -lt $Shots; $i++) {
        & "$t\shotgame.ps1" "$t\_e.png"
        $res[$eye] += Get-Bright "$t\_e.png"
        Start-Sleep -Milliseconds 300
    }
    if ($Fire) { Start-Sleep 3 }
}
Remove-Item "$t\_e.png" -ErrorAction SilentlyContinue
[IO.File]::WriteAllText($cmd, "windoweye 0`r`n")
"{0,-28} eye1 {1,6:F1}  eye2 {2,6:F1}   (eye1: {3})  (eye2: {4})" -f $Setup, ($res[1] | Measure-Object -Average).Average, ($res[2] | Measure-Object -Average).Average, (($res[1] | ForEach-Object { '{0:F0}' -f $_ }) -join ' '), (($res[2] | ForEach-Object { '{0:F0}' -f $_ }) -join ' ')
