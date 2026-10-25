<#
.SYNOPSIS
  Builds the sr2mods proxy (version.dll) with MSVC x64 + CMake/Ninja.

.EXAMPLE
  .\build.ps1                 # Release
  .\build.ps1 -Config Debug   # Debug (debug console available)
  .\build.ps1 -Clean          # wipe build dir first
  .\build.ps1 -Autotest:$false  # without the test harness (like the GitHub release builds)
  .\build.ps1 -Regen          # regenerate proxy exports from System32\version.dll (needs Python + pefile)

  Output: build\<Config>\version.dll, copied with sr2mods.json to dist\<Config>\.
  Every new DLL is also archived with its PDB to dist\archive\<yyyyMMdd-HHmmss>-<sha8>\ (last 20 kept).
  Nothing is ever copied into the game folder by this script.
#>
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string]$Config = 'Release',
  [switch]$Clean,
  [switch]$Regen,
  [switch]$NoVerify,
  [int]$Jobs = 0,
  # Compile in the test harness (modules/autotest). On for local builds; the GitHub release builds
  # pass -Autotest:$false (no pipe code, no "Test harness" switch).
  [bool]$Autotest = $true
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root "build\$Config"
$distDir = Join-Path $root "dist\$Config"

function Find-Python {
  foreach ($c in 'python', 'py') {
    $cmd = Get-Command $c -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
  }
  return $null
}

# ---- locate MSVC via vswhere -------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio / Build Tools with the C++ workload." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with MSVC x64 tools found." }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vsPath" }
Write-Host "MSVC: $vsPath"

# ---- import the x64 developer environment into this PowerShell session -------
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
if (-not (Test-Path $cmake)) { throw "cmake not found (install the 'C++ CMake tools' VS component)" }
if (-not (Test-Path $ninja)) { throw "ninja not found (install the 'C++ CMake tools' VS component)" }

# ---- optional: regenerate export stubs ---------------------------------------
if ($Regen) {
  $py = Find-Python
  if (-not $py) { throw "-Regen needs Python with pefile" }
  & $py (Join-Path $root 'proxy\gen_proxy.py')
  if ($LASTEXITCODE -ne 0) { throw "gen_proxy.py failed" }
}

# ---- embedded icons -------------------------------------------------------------
# CMake runs assets/icons/src/gen_inc.py (build/generated/sr2mods_icons_data.inc from
# assets/icons/*.png + the local-only assets/icons/local/*.png); hand it the Python we find.
$iconArgs = @()
$py = Find-Python
if ($py) { $iconArgs = @("-DPython3_EXECUTABLE=$py") }
else { Write-Warning "Python not found - building without the mod's icons (vanilla icons show)" }

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

# ---- configure + build ---------------------------------------------------------
$autotestFlag = if ($Autotest) { 'ON' } else { 'OFF' }
Write-Host "Test harness: $autotestFlag"
& $cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_MAKE_PROGRAM=$ninja" `
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl "-DSR2MODS_AUTOTEST=$autotestFlag" @iconArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
# a linked worktree (agent) has its own .git file; its git dir differs from the common dir
$gitDir = (& git -C $root rev-parse --path-format=absolute --git-dir 2>$null)
$common = (& git -C $root rev-parse --path-format=absolute --git-common-dir 2>$null)
$isWorktree = [bool]($gitDir -and $common -and $gitDir -ne $common)
if ($Jobs -le 0 -and $env:SR2MODS_BUILD_JOBS) { $Jobs = [int]$env:SR2MODS_BUILD_JOBS }
# `git config sr2mods.parallelbuilds true` (the user is away from the PC): agent builds run
# side by side at normal priority with all cores; unset it to go back to taking turns.
$parallel = ((& git -C $root config --get sr2mods.parallelbuilds 2>$null) -eq 'true')
if ($Jobs -le 0 -and $isWorktree -and -not $parallel) { $Jobs = 2 }
# Agent builds take turns (one worktree compiles at a time) at below-normal priority, which
# ninja and cl inherit, so several agents don't flood the user's CPU.
$buildLock = $null
if ($isWorktree -and -not $parallel) {
  try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch {}
  $buildLock = New-Object System.Threading.Mutex($false, 'Local\sr2mods-agent-build')
  Write-Host "Waiting for other agent builds to finish..."
  try { [void]$buildLock.WaitOne() } catch [System.Threading.AbandonedMutexException] {}
}
try {
  if ($Jobs -gt 0) {
    Write-Host "Parallel jobs: $Jobs"
    & $cmake --build $buildDir -j $Jobs
  } else {
    & $cmake --build $buildDir
  }
} finally {
  if ($buildLock) { $buildLock.ReleaseMutex(); $buildLock.Dispose() }
}
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

$dll = Join-Path $buildDir 'version.dll'
if (-not (Test-Path $dll)) { throw "Build produced no version.dll" }

New-Item -ItemType Directory -Force $distDir | Out-Null
Copy-Item $dll $distDir -Force
Copy-Item (Join-Path $buildDir 'version.pdb') $distDir -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $root 'sr2mods.json') $distDir -Force
Write-Host "Built: $dll"
Write-Host "Dist : $distDir"

# ---- build identity + archive ---------------------------------------------------
# The game log prints "self: PE timestamp ..., sha256 <first 16>, pdb <GUID> age <n>"
# at startup; match it against the archive folder name / build-info.txt to get the
# exact version.pdb for a crash.
$pdb = Join-Path $buildDir 'version.pdb'
$sha = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLowerInvariant()
$pdbId = 'unknown'
$dumpbin = (Get-Command dumpbin -ErrorAction SilentlyContinue).Source
if ($dumpbin) {
  $rsds = & $dumpbin /nologo /headers $dll | Select-String -Pattern 'Format: RSDS, \{([0-9A-Fa-f-]+)\}, (\d+),' | Select-Object -First 1
  if ($rsds) { $pdbId = "$($rsds.Matches[0].Groups[1].Value.ToUpperInvariant()) age $($rsds.Matches[0].Groups[2].Value)" }
}
Write-Host "SHA-256: $sha"
Write-Host "PDB    : $pdbId"

$archiveRoot = Join-Path $root 'dist\archive'
$sha8 = $sha.Substring(0, 8)
New-Item -ItemType Directory -Force $archiveRoot | Out-Null
$existing = Get-ChildItem $archiveRoot -Directory -Filter "*-$sha8" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($existing) {
  Write-Host "Archive: $($existing.FullName) (unchanged DLL, already archived)"
} else {
  $archiveDir = Join-Path $archiveRoot ("{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $sha8)
  New-Item -ItemType Directory -Force $archiveDir | Out-Null
  Copy-Item $dll $archiveDir -Force
  if (Test-Path $pdb) { Copy-Item $pdb $archiveDir -Force }
  Copy-Item (Join-Path $root 'sr2mods.json') $archiveDir -Force
  @(
    "config  : $Config"
    "built   : $(Get-Date -Format 's')"
    "sha256  : $sha"
    "pdb     : $pdbId"
  ) | Set-Content -Path (Join-Path $archiveDir 'build-info.txt') -Encoding utf8
  Write-Host "Archive: $archiveDir"
}
# Keep the newest 20 archives (folder names start with the timestamp).
Get-ChildItem $archiveRoot -Directory | Sort-Object Name -Descending | Select-Object -Skip 20 |
  ForEach-Object { Remove-Item -Recurse -Force $_.FullName }

# ---- verify exports against System32\version.dll -----------------------------
if (-not $NoVerify) {
  $py = Find-Python
  if ($py) {
    & $py (Join-Path $root 'proxy\verify_exports.py') $dll
    if ($LASTEXITCODE -ne 0) { throw "Export verification FAILED" }
  } else {
    Write-Warning "Python not found - skipping export verification (use: dumpbin /exports $dll)"
  }
}
