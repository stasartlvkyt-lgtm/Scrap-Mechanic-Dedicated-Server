param([switch]$Test, [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Visual Studio C++ Build Tools not found' }
$devShell = Join-Path $installation 'Common7\Tools\Launch-VsDevShell.ps1'
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$env:VSLANG = '1033'
$cmake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = 'cmake' }
if (!(Test-Path -LiteralPath $ninja)) { $ninja = 'ninja' }
& $cmake -S $projectRoot -B (Join-Path $projectRoot $BuildDir) -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build (Join-Path $projectRoot $BuildDir) --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if ($Test) {
  $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
  & $ctest --test-dir (Join-Path $projectRoot $BuildDir) --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
