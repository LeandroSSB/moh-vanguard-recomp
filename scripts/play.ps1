# Play: run the game runner with mouse-look (MKB) active.
# Requires: setup (gen installed) + build -Runner done.
param(
    [string]$Elf = "disc\MOH5RDVD.ELF",
    [string]$MkbConfig = "config\mkb.ini",
    [string]$InputScript = "",
    [int]$Seconds = 0
)
$ErrorActionPreference = "Stop"
$exe = "E:\wt-vanguard\out\build\ps2xRuntime\Release\ps2EntryRunner.exe"
$env:PS2X_MKB = "1"
$env:PS2X_MKB_CONFIG = (Join-Path $PSScriptRoot "..\$MkbConfig")
if ($InputScript) { $env:PS2X_INPUT_SCRIPT = $InputScript }
if ($Seconds -gt 0) {
    $p = Start-Process -FilePath $exe -ArgumentList "`"$Elf`"" -PassThru -WorkingDirectory (Get-Location)
    Start-Sleep -Seconds $Seconds
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
} else {
    & $exe $Elf
}
