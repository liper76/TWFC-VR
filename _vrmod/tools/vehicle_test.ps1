# Vehicle wall test on Ventilation Shafts (after run_vent.ps1): sets [VR] keys live, transforms, drives forward 4 s
# and prints the pawn's speed. Usage: vehicle_test.ps1 "SyncEyeTimeVehicle=1" "HeldFrameTime=0.001"
param([string[]]$Keys = @())
$root = "C:\Games\Transformers - War for Cybertron"
$bin = "$root\BinariesVR"
$ini = "$bin\twfcvr.ini"
$t = [IO.File]::ReadAllText($ini)
$t = [regex]::Replace($t, "(?s); --vtest--\r?\n.*?; --end vtest--\r?\n", "")
if ($Keys.Count) { $t = $t.Replace("[VR]`r`n", "[VR]`r`n; --vtest--`r`n" + ($Keys -join "`r`n") + "`r`n; --end vtest--`r`n") }
[IO.File]::WriteAllText($ini, $t)
if (-not $Keys.Count) { "vehicle test keys removed"; return }
Start-Sleep 1
$pawn = ((Get-Content "$bin\twfcvr.log" | Select-String 'ik: .*pawn' | Select-Object -Last 1).Line -replace '.*pawn ([0-9A-F]+)\).*', '$1')
"pawn $pawn"
[IO.File]::WriteAllText("$bin\twfcvr_cmd.txt", "press LS 200`nwait 2500`nstick LY 1 4000`n")
& "$root\_vrmod_tools\venv\Scripts\python.exe" "$root\_vrmod\tools\speed.py" $pawn 7.5
