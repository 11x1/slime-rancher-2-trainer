<#
.SYNOPSIS
  Deploys the latest sr2mods build (version.dll) into the Slime Rancher 2 game folder.

.EXAMPLE
  .\deploy.ps1                       # deploy dist\Release\version.dll
  .\deploy.ps1 -Build                # run build.ps1 first, abort if it fails
  .\deploy.ps1 -WaitForExit          # wait for SlimeRancher2.exe to close instead of refusing
  .\deploy.ps1 -GameDir 'D:\Games\Slime Rancher 2'   # or set $env:SR2MODS_GAMEDIR
  .\deploy.ps1 -Uninstall            # remove version.dll (backed up first); settings stay
  .\deploy.ps1 -Uninstall -Purge     # also delete sr2mods.json, logs and sr2mods_saves\ (asks first)

  What it does:
  - Backs up the SR2 saves (LocalLow\MonomiPark\SlimeRancher2\Steam\<steamid>\) to save-backups\<yyyy-MM-dd_HHmmss>\
    (skip with -NoSaveBackup).
  - Backs up the currently deployed version.dll to dist\deployed-backup\version.<timestamp>.<sha8>.dll (last 5 kept).
  - Copies version.dll into the game folder (skipped if the deployed one is identical).
  - Copies the default sr2mods.json ONLY if the game folder has none. The live sr2mods.json is never overwritten.
  Nothing else is ever written into the game folder.
#>
param(
  [string]$GameDir,
  [string]$Source,
  [switch]$Build,
  [switch]$WaitForExit,
  [switch]$NoSaveBackup,
  [switch]$Uninstall,
  [switch]$Purge,
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
trap { Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$root = $PSScriptRoot
$appId = '1657630'
$exeName = 'SlimeRancher2.exe'
$keepDllBackups = 5

function Get-Sha8([string]$path) {
  return (Get-FileHash -Algorithm SHA256 $path).Hash.ToLowerInvariant().Substring(0, 8)
}

function Test-GameDir([string]$dir) {
  return ($dir -and (Test-Path -LiteralPath (Join-Path $dir $exeName) -PathType Leaf))
}

# ---- locate the game folder ----------------------------------------------------
function Find-GameDir {
  $default = Join-Path ${env:ProgramFiles(x86)} 'Steam\steamapps\common\Slime Rancher 2'
  if (Test-GameDir $default) { return $default }

  # Steam library folders: <SteamPath>\steamapps\libraryfolders.vdf lists every library
  # with the appids it holds. Find the one containing SR2.
  $steamPath = $null
  try { $steamPath = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath } catch {}
  if (-not $steamPath) { return $null }
  $vdf = Join-Path ($steamPath -replace '/', '\') 'steamapps\libraryfolders.vdf'
  if (-not (Test-Path -LiteralPath $vdf)) { return $null }

  $libs = @()
  $current = $null
  foreach ($line in Get-Content -LiteralPath $vdf) {
    if ($line -match '^\s*"path"\s+"(.+)"\s*$') {
      $current = $Matches[1] -replace '\\\\', '\'
      $libs += $current
    } elseif ($current -and $line -match "^\s*`"$appId`"\s+`"") {
      $candidate = Join-Path $current 'steamapps\common\Slime Rancher 2'
      if (Test-GameDir $candidate) { return $candidate }
    }
  }
  # appid not listed (stale vdf) - try every library anyway
  foreach ($lib in $libs) {
    $candidate = Join-Path $lib 'steamapps\common\Slime Rancher 2'
    if (Test-GameDir $candidate) { return $candidate }
  }
  return $null
}

if (-not $GameDir -and $env:SR2MODS_GAMEDIR) { $GameDir = $env:SR2MODS_GAMEDIR }
if ($GameDir) {
  if (-not (Test-GameDir $GameDir)) { throw "Game folder '$GameDir' (-GameDir / SR2MODS_GAMEDIR) does not contain $exeName" }
} else {
  $GameDir = Find-GameDir
  if (-not $GameDir) { throw "Slime Rancher 2 not found (default Steam folder and libraryfolders.vdf checked). Pass -GameDir <folder>." }
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
Write-Host "Game   : $GameDir"

# ---- refuse while the game is running --------------------------------------------
$procName = [IO.Path]::GetFileNameWithoutExtension($exeName)
$running = Get-Process -Name $procName -ErrorAction SilentlyContinue
if ($running) {
  if (-not $WaitForExit) {
    throw "$exeName is running (PID $(($running | ForEach-Object Id) -join ', ')). Close the game first, or pass -WaitForExit."
  }
  Write-Host "Waiting for $exeName to exit..."
  while (Get-Process -Name $procName -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 1 }
  Start-Sleep -Seconds 2   # let the game release version.dll
  Write-Host "Game exited."
}

# ---- source DLL (deploy only) ----------------------------------------------------
if (-not $Uninstall) {
  if ($Build) {
    Write-Host "Building (build.ps1)..."
    & (Join-Path $root 'build.ps1')
    if (-not $?) { throw "build.ps1 failed - nothing deployed" }
  }
  if (-not $Source) { $Source = Join-Path $root 'dist\Release\version.dll' }
  if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    throw "Source DLL not found: $Source (run .\build.ps1 or pass -Build)"
  }
  $Source = (Resolve-Path -LiteralPath $Source).Path
  $srcItem = Get-Item -LiteralPath $Source
  $srcSha8 = Get-Sha8 $Source
  Write-Host ("Source : {0}  ({1:yyyy-MM-dd HH:mm:ss}, sha256 {2})" -f $Source, $srcItem.LastWriteTime, $srcSha8)
}

# ---- back up the SR2 saves (read-only for the saves folder) ---------------------
$stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
if ($NoSaveBackup) {
  Write-Host "Saves  : backup skipped (-NoSaveBackup)"
} else {
  $savesRoot = Join-Path $env:USERPROFILE 'AppData\LocalLow\MonomiPark\SlimeRancher2\Steam'
  $idDirs = @(Get-ChildItem -LiteralPath $savesRoot -Directory -ErrorAction SilentlyContinue)
  if ($idDirs.Count -eq 0) {
    Write-Warning "No save folders under $savesRoot - nothing to back up."
  } else {
    $backupDir = Join-Path $root "save-backups\$stamp"
    New-Item -ItemType Directory -Force $backupDir | Out-Null
    if ($idDirs.Count -eq 1) {
      # single Steam account: flat copy, same layout as earlier manual backups
      Copy-Item -Path (Join-Path $idDirs[0].FullName '*') -Destination $backupDir -Recurse -Force
    } else {
      foreach ($d in $idDirs) { Copy-Item -LiteralPath $d.FullName -Destination $backupDir -Recurse -Force }
    }
    $n = @(Get-ChildItem -LiteralPath $backupDir -Recurse -File).Count
    Write-Host "Saves  : $n file(s) from $($idDirs.Count) account folder(s) -> $backupDir"
  }
}

# ---- back up the deployed version.dll --------------------------------------------
$gameDll = Join-Path $GameDir 'version.dll'
$dllBackupRoot = Join-Path $root 'dist\deployed-backup'

function Backup-DeployedDll {
  if (-not (Test-Path -LiteralPath $gameDll -PathType Leaf)) { return }
  New-Item -ItemType Directory -Force $dllBackupRoot | Out-Null
  $sha8 = Get-Sha8 $gameDll
  $dest = Join-Path $dllBackupRoot ("version.{0}.{1}.dll" -f $stamp, $sha8)
  Copy-Item -LiteralPath $gameDll -Destination $dest -Force
  Write-Host "Backup : deployed version.dll ($sha8) -> $dest"
  # keep the newest N (names start with the sortable timestamp)
  Get-ChildItem -LiteralPath $dllBackupRoot -File -Filter 'version.*.dll' | Sort-Object Name -Descending |
    Select-Object -Skip $keepDllBackups | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
}

# ---- uninstall ----------------------------------------------------------------------
if ($Uninstall) {
  if (Test-Path -LiteralPath $gameDll -PathType Leaf) {
    Backup-DeployedDll
    Remove-Item -LiteralPath $gameDll -Force
    Write-Host "Removed: $gameDll"
  } else {
    Write-Host "version.dll is not installed in the game folder."
  }

  if ($Purge) {
    $targets = @('sr2mods.json', 'sr2mods.log', 'sr2mods.prev.log', 'sr2mods_saves') |
      ForEach-Object { Join-Path $GameDir $_ } | Where-Object { Test-Path -LiteralPath $_ }
    if (-not $targets) {
      Write-Host "Purge  : nothing to delete."
    } else {
      Write-Host "Purge will delete:"
      $targets | ForEach-Object { Write-Host "  $_" }
      if (-not $Force) {
        $answer = Read-Host "Type 'yes' to delete these"
        if ($answer -ne 'yes') { Write-Host "Purge cancelled."; exit 1 }
      }
      foreach ($t in $targets) { Remove-Item -LiteralPath $t -Recurse -Force }
      Write-Host "Purge  : done."
    }
  } else {
    Write-Host "Kept   : sr2mods.json, sr2mods_saves\ (use -Purge to delete)"
  }
  Write-Host "Uninstall complete."
  exit 0
}

# ---- deploy -------------------------------------------------------------------------
if ((Test-Path -LiteralPath $gameDll -PathType Leaf) -and
    (Get-FileHash -Algorithm SHA256 $gameDll).Hash -eq (Get-FileHash -Algorithm SHA256 $Source).Hash) {
  Write-Host "DLL    : deployed version.dll is identical ($srcSha8) - copy skipped"
} else {
  Backup-DeployedDll
  Copy-Item -LiteralPath $Source -Destination $gameDll -Force
  Write-Host "DLL    : deployed $srcSha8 -> $gameDll"
}

# Settings: never overwrite the user's live sr2mods.json.
$gameCfg = Join-Path $GameDir 'sr2mods.json'
if (Test-Path -LiteralPath $gameCfg) {
  Write-Host "Config : kept existing $gameCfg"
} else {
  $defaultCfg = Join-Path $root 'sr2mods.json'
  if (-not (Test-Path -LiteralPath $defaultCfg)) { throw "Default sr2mods.json not found at $defaultCfg" }
  Copy-Item -LiteralPath $defaultCfg -Destination $gameCfg
  Write-Host "Config : no sr2mods.json in game folder - copied default"
}

Write-Host "Deploy complete."
exit 0
