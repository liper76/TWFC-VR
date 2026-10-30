# Test run: backs up the saves, launches BinariesVR\TWFC.exe on MockXR with automation, walks to Ventilation Shafts.
# Usage: run_vent.ps1 [-ExtraIni "Key=Value",...]   (keys go to [Debug]; remove them afterwards with -Cleanup)
param([string[]]$ExtraIni = @(), [switch]$Cleanup, [int]$PopupWait = 46)
$root = "C:\Games\Transformers - War for Cybertron"
$bin = "$root\BinariesVR"
$ini = "$bin\twfcvr.ini"
$cmd = "$bin\twfcvr_cmd.txt"

# extra [Debug] keys go between marker comments right after [Debug]; Automation is switched in place
function Set-Debug([string[]]$lines, [bool]$on) {
    $t = [IO.File]::ReadAllText($ini)
    $t = [regex]::Replace($t, "(?s); --test--\r?\n.*?; --end test--\r?\n", "")
    $t = [regex]::Replace($t, "(?m)^Automation=\d+", "Automation=" + $(if ($on) { 1 } else { 0 }))
    if ($lines.Count) { $t = $t.Replace("[Debug]`r`n", "[Debug]`r`n; --test--`r`n" + ($lines -join "`r`n") + "`r`n; --end test--`r`n") }
    [IO.File]::WriteAllText($ini, $t)
}

if ($Cleanup) { Set-Debug @() $false; Remove-Item $cmd -ErrorAction SilentlyContinue; "cleaned"; return }

$saves = [Environment]::GetFolderPath("MyDocuments") + "\My Games\Transformers\TransGame\SaveData"
$bk = "$root\_vrmod\backup\SaveData_" + (Get-Date -Format "yyyy-MM-dd_HHmm")
if (-not (Test-Path $bk)) { Copy-Item $saves $bk -Recurse }
Set-Debug $ExtraIni $true
Remove-Item $cmd -ErrorAction SilentlyContinue
$env:XR_RUNTIME_JSON = "$root\_vrmod\tools\mockxr\mockxr.json"
Start-Process "$bin\TWFC.exe" -WorkingDirectory $bin
Remove-Item Env:\XR_RUNTIME_JSON
# intro videos: START every 8 s until the Accounts popup is up
for ($t = 16; $t -lt $PopupWait; $t += 8) { Start-Sleep 8; [IO.File]::WriteAllText($cmd, "press START 150`r`n") }
Start-Sleep 6
Copy-Item "$root\_vrmod\tools\go_vent.txt" $cmd
"launched; saves backed up to $bk"
