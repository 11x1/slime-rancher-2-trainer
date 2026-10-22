<#
.SYNOPSIS
  Builds the minimal injection-test version.dll (inject_test\) with MSVC x64 + CMake/Ninja.

  Output: build\inject_test\version.dll, copied with version.pdb and README.txt
  to dist\InjectTest\. Nothing is ever copied into the game folder.
#>
param(
  [switch]$Clean,
  [switch]$NoVerify
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Split-Path $here -Parent
$buildDir = Join-Path $root 'build\inject_test'
$distDir = Join-Path $root 'dist\InjectTest'

function Find-Python {
  foreach ($c in 'python', 'py') {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }
  return $null
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found" }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with MSVC x64 tools found." }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
Write-Host "MSVC: $vsPath"

if (-not $env:VSCMD_ARG_TGT_ARCH -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
  $envDump = & cmd.exe /s /c "`"$vcvars`" >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed" }
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
  }
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
$ninja = (Get-Command ninja -ErrorAction SilentlyContinue).Source
if (-not $ninja) { $ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' }

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

& $cmake -S $here -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninja" `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
& $cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$dll = Join-Path $buildDir 'version.dll'
if (-not (Test-Path $dll)) { throw "Build produced no version.dll" }

New-Item -ItemType Directory -Force $distDir | Out-Null
Copy-Item $dll $distDir -Force
Copy-Item (Join-Path $buildDir 'version.pdb') $distDir -Force
Copy-Item (Join-Path $here 'README.txt') $distDir -Force
Write-Host "Built: $dll"
Write-Host "Dist : $distDir"

if (-not $NoVerify) {
  $py = Find-Python
  if (-not $py) { throw "Python not found - cannot verify exports (use -NoVerify to skip)" }
  & $py (Join-Path $root 'proxy\verify_exports.py') (Join-Path $distDir 'version.dll')
  if ($LASTEXITCODE -ne 0) { throw "Export verification FAILED" }
}
