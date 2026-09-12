<#
    Crimson Desert Combat Menu - uninstaller

    Removes the two mod files from the game's bin64 folder. Nothing else is
    touched: the installer never modified a game file, so there is nothing to
    restore. Backups the installer made are left in place.

    Usage:
        .\Uninstall.ps1
        .\Uninstall.ps1 -GamePath "D:\...\Crimson Desert"
#>
[CmdletBinding()]
param(
    [string]$GamePath
)

$ErrorActionPreference = 'Stop'
$ModFiles = @('Trinity.asi', 'winmm.dll')

function Write-Step { param($m) Write-Host "  $m" }
function Write-Good { param($m) Write-Host "  $m" -ForegroundColor Green }
function Write-Bad  { param($m) Write-Host "  $m" -ForegroundColor Red }

function Get-SteamLibraries {
    $steam = $null
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        try {
            $v = (Get-ItemProperty -Path $key -ErrorAction Stop)
            if ($v.SteamPath)       { $steam = $v.SteamPath }
            elseif ($v.InstallPath) { $steam = $v.InstallPath }
            if ($steam) { break }
        } catch { }
    }
    if (-not $steam) { return @() }

    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '"path"\s+"(.+?)"') {
                $libraries += $Matches[1] -replace '\\\\', '\'
            }
        }
    }
    return $libraries | Sort-Object -Unique
}

Write-Host ''
Write-Host '  Crimson Desert Combat Menu - uninstaller' -ForegroundColor Cyan
Write-Host ''

if (-not $GamePath) {
    foreach ($lib in Get-SteamLibraries) {
        $candidate = Join-Path $lib 'steamapps\common\Crimson Desert'
        if (Test-Path -LiteralPath (Join-Path $candidate 'bin64\CrimsonDesert.exe')) {
            $GamePath = $candidate
            break
        }
    }
}
if (-not $GamePath) {
    Write-Bad 'Could not find Crimson Desert automatically.'
    Write-Step 'Re-run with -GamePath "<your Crimson Desert folder>".'
    exit 1
}

$bin64 = Join-Path $GamePath 'bin64'
Write-Step "Game folder: $GamePath"

if (Get-Process -Name 'CrimsonDesert' -ErrorAction SilentlyContinue) {
    Write-Bad 'Crimson Desert is running. Close it completely, then run this again.'
    exit 1
}

$removed = 0
foreach ($f in $ModFiles) {
    $target = Join-Path $bin64 $f
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Force
        Write-Good "Removed $f"
        $removed++
    }
    else {
        Write-Step "$f was not installed."
    }
}

Write-Host ''
if ($removed -gt 0) {
    Write-Good 'Done. The game is back to normal.'
    Write-Step 'Backups (*.bak) and Trinity.log, if any, were left in bin64 for you to delete.'
}
else {
    Write-Step 'Nothing to remove.'
}
Write-Host ''
