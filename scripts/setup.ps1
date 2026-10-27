# Full pipeline from your ISO: extract -> analyze -> recompile.
# Requires the toolchain built first (scripts\build.ps1 without -Runner).
param(
    [Parameter(Mandatory = $true)][string]$IsoPath,
    [string]$WorkDir = "disc"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# 1. extract boot ELF + IOP modules from the ISO (Mount-DiskImage, no admin)
& "$root\extract_iso.ps1" -IsoPath $IsoPath -DestDir $WorkDir
if ($LASTEXITCODE -ge 8) { throw "extract failed" }

# 2. locate the boot ELF (SYSTEM.CNF BOOT2) - Vanguard: MOH5RDVD.ELF
$elf = Join-Path $WorkDir "MOH5RDVD.ELF"
if (-not (Test-Path $elf)) { throw "boot ELF not found at $elf" }

# 3. analyzer -> recompiler config (stubs/skips/entry candidates)
$toolsDir = Join-Path $root "..\out\build\ps2xAnalyzer\Release"
& (Join-Path $toolsDir "ps2_analyzer.exe") $elf (Join-Path $root "..\config\recompiler.toml")

# 4. recompile: generates the engine code (gen\) - minutes to ~1 h for 18.8k functions
$recomp = Join-Path $root "..\out\build\ps2xRecomp\Release\ps2_recomp.exe"
& $recomp (Join-Path $root "..\config\recompiler.toml")

# 5. gen sanity check: register_functions.cpp must be the FULL table (~519k lines),
#    not the ~6-line upstream placeholder (the runner dies at the first instruction
#    with an empty table).
$rf = Join-Path $root "..\ps2xRuntime\src\runner\register_functions.cpp"
$lines = (Get-Content $rf | Measure-Object -Line).Lines
Write-Output ("register_functions.cpp lines: {0}" -f $lines)
if ($lines -lt 1000) { throw "register_functions.cpp looks like the empty placeholder - recomp did not install the gen" }
Write-Output "Setup complete. Next: scripts\build.ps1 -Runner"
