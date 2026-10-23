<#
.SYNOPSIS
  Enables the repo's git hooks (githooks\) for this checkout: git config core.hooksPath githooks.

.EXAMPLE
  .\githooks\install.ps1           # enable: a local merge into develop auto-runs deploy.ps1 -Build
  .\githooks\install.ps1 -Remove   # disable (unset core.hooksPath)

  Opt out without uninstalling: $env:SR2MODS_NO_DEPLOY = '1', or git config sr2mods.autodeploy false.
  core.hooksPath is local git config; it is not committed.
#>
param([switch]$Remove)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if ($Remove) {
  & git -C $repo config --unset core.hooksPath
  if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 5) { throw "git config --unset core.hooksPath failed" }  # 5 = was not set
  Write-Host "Hooks disabled (core.hooksPath unset) in $repo"
  exit 0
}

& git -C $repo config core.hooksPath githooks
if ($LASTEXITCODE -ne 0) { throw "git config core.hooksPath failed" }
Write-Host "Hooks enabled in $repo (core.hooksPath = githooks)."
Write-Host "post-merge: a local merge into develop now runs deploy.ps1 -Build (log: dist\deploy-hook.log)."
