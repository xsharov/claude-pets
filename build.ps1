param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$Version = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$buildDir = Join-Path $root "build\$($Configuration.ToLower())"
if ($Version -and $Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version must look like 1.2.3, got '$Version'."
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 or Build Tools with the C++ workload."
}
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw "Visual Studio with the MSVC x64 toolset was not found."
}
$vcvars = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

$configure = "cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DCLAUDE_PETS_VERSION=$Version"
$build = "cmake --build `"$buildDir`""
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
cmd.exe /c "call `"$vcvars`" >nul 2>&1 && $configure && $build"
$ErrorActionPreference = $previousPreference
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}
Write-Host "Built $(Join-Path $buildDir 'claude-pets.exe')"
