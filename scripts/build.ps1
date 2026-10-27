# Build the toolchain (default) and/or the game runner.
# ABSOLUTE build path: the shell cwd flaps between calls.
param(
    [switch]$Runner,
    [string]$BuildDir = "E:\wt-vanguard\out\build"
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
    cmake -S (Split-Path $BuildDir -Parent) -B $BuildDir -DPS2X_ENABLE_AGRESSIVE_LOGS=ON
}
if ($Runner) {
    # NOTE: before a FULL runner build, ps2xRuntime\src\runner\register_functions.cpp
    # must hold the generated table (~519k lines) - the setup pipeline installs it.
    cmake --build $BuildDir --config Release --target ps2EntryRunner
} else {
    cmake --build $BuildDir --config Release --target ps2_analyzer ps2_recomp ps2x_tests
}
