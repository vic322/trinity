<#
    Crimson Desert Combat Menu - installer

    Finds the game through Steam, refuses to install on a game build this menu
    has not been validated against, backs up anything already in bin64, and
    copies the two mod files in.

    Usage:
        .\Install.ps1                      # auto-detect through Steam
        .\Install.ps1 -GamePath "D:\...\Crimson Desert"
        .\Install.ps1 -Force               # install despite a version mismatch
#>
[CmdletBinding()]
param(
    [string]$GamePath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$SupportedVersion = '1.0.0.2850'
$SupportedTitle   = 'Crimson Desert 2.02.00'
$ModFiles         = @('Trinity.asi', 'winmm.dll')

function Write-Step  { param($m) Write-Host "  $m" }
function Write-Good  { param($m) Write-Host "  $m" -ForegroundColor Green }
function Write-Warn  { param($m) Write-Host "  $m" -ForegroundColor Yellow }
function Write-Bad   { param($m) Write-Host "  $m" -ForegroundColor Red }

function Get-SteamLibraries {
    $steam = $null
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam')) {
        try {
            $v = (Get-ItemProperty -Path $key -ErrorAction Stop)
            if ($v.SteamPath)    { $steam = $v.SteamPath }
            elseif ($v.InstallPath) { $steam = $v.InstallPath }
            if ($steam) { break }
        } catch { }
    }
    if (-not $steam) { return @() }

    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf) {
        # Each library is a "path"  "D:\\SteamLibrary" line; unescape the slashes.
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '"path"\s+"(.+?)"') {
                $libraries += $Matches[1] -replace '\\\\', '\'
            }
        }
    }
    return $libraries | Sort-Object -Unique
}

function Find-Game {
    foreach ($lib in Get-SteamLibraries) {
        $candidate = Join-Path $lib 'steamapps\common\Crimson Desert'
        if (Test-Path -LiteralPath (Join-Path $candidate 'bin64\CrimsonDesert.exe')) {
            return $candidate
        }
    }
    return $null
}

Write-Host ''
Write-Host '  Crimson Desert Combat Menu - installer' -ForegroundColor Cyan
Write-Host "  Built for $SupportedTitle  (executable $SupportedVersion)"
Write-Host ''

# --- The files we are about to install must be sitting next to this script ---
$source = $PSScriptRoot
foreach ($f in $ModFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $f))) {
        Write-Bad "Missing $f next to this script."
        Write-Step 'Extract the whole release archive and run the installer from inside it.'
        exit 1
    }
}

# --- Locate the game -------------------------------------------------------
if (-not $GamePath) {
    Write-Step 'Looking for Crimson Desert through Steam...'
    $GamePath = Find-Game
}
if (-not $GamePath) {
    Write-Bad 'Could not find Crimson Desert automatically.'
    Write-Step 'Re-run with the game folder, for example:'
    Write-Step '  .\Install.ps1 -GamePath "D:\SteamLibrary\steamapps\common\Crimson Desert"'
    exit 1
}

$bin64 = Join-Path $GamePath 'bin64'
$exe   = Join-Path $bin64 'CrimsonDesert.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Bad "No CrimsonDesert.exe in $bin64"
    exit 1
}
Write-Good "Found the game: $GamePath"

# --- Refuse a mismatched build --------------------------------------------
$installed = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
Write-Step "Installed game executable: $installed"
if ($installed -ne $SupportedVersion) {
    Write-Host ''
    Write-Bad  'GAME VERSION MISMATCH'
    Write-Step "This menu was built for $SupportedVersion ($SupportedTitle)."
    Write-Step "Your game is       $installed."
    Write-Step 'Installing it here would do nothing: the mod detects the mismatch and'
    Write-Step 'disables itself rather than hooking a build it does not understand.'
    Write-Host ''
    if (-not $Force) {
        Write-Step 'Nothing was changed. Check for a newer release, or re-run with -Force'
        Write-Step 'to install anyway (the menu will still refuse to enable itself).'
        exit 1
    }
    Write-Warn 'Continuing anyway because -Force was given.'
}
else {
    Write-Good 'Game version matches.'
}

# --- The game must not be running -----------------------------------------
if (Get-Process -Name 'CrimsonDesert' -ErrorAction SilentlyContinue) {
    Write-Bad 'Crimson Desert is running. Close it completely, then run this again.'
    exit 1
}

# --- Back up anything already there ----------------------------------------
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
foreach ($f in $ModFiles) {
    $dest = Join-Path $bin64 $f
    if (Test-Path -LiteralPath $dest) {
        $backup = "$dest.$stamp.bak"
        Copy-Item -LiteralPath $dest -Destination $backup
        Write-Step "Backed up existing $f -> $(Split-Path $backup -Leaf)"
    }
}

# --- Install and verify ----------------------------------------------------
foreach ($f in $ModFiles) {
    $from = Join-Path $source $f
    $to   = Join-Path $bin64  $f
    Copy-Item -LiteralPath $from -Destination $to -Force
    if ((Get-FileHash -LiteralPath $from).Hash -ne (Get-FileHash -LiteralPath $to).Hash) {
        Write-Bad "$f did not copy correctly. Check disk space and permissions."
        exit 1
    }
    Write-Good "Installed $f"
}

Write-Host ''
Write-Good 'Done.'
Write-Host ''
Write-Step 'Launch Crimson Desert normally through Steam and load a save.'
Write-Step 'Press the tilde/backtick key (~ or `, below Esc) to open the menu,'
Write-Step 'or LB + D-pad Down on a controller. All cheats start off.'
Write-Host ''
Write-Step 'To uninstall later, run Uninstall.ps1 from this folder.'
Write-Host ''
