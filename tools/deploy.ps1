<#
.SYNOPSIS
Builds Palpedia Enhancer and deploys it for testing.

.DESCRIPTION
-Package is the default and the only target that counts as a real test. It stages the full loader
package under the Steam Workshop root, the same path the Palworld loader installs from once this
ships, so testing through it proves the mod actually loads and runs the way a player's install will
work, whether that install comes from Steam Workshop or, once packaged separately for it, Nexus Mods.

  -Package   (default) Stages a DEV build under the workshop root at $LocalPackageId as its own
             package, "PalpediaEnhancerDev" ("Palpedia Enhancer (Dev)"), with "DebugMode": true
             so the loader reinstalls it on every launch. The released Workshop item
             (PackageName PalpediaEnhancer, subscribed, Steam-managed) is never touched; instead
             the script flips the loader's enable switch in Mods\PalModSettings.ini so that ONLY
             the dev package is active. Safe to run with Palworld open; the game never loads from
             the staging folder directly.

  -Unstage   Removes the dev package (staging folder, installed copy, loader record) and gives
             the enable switch back to the released Workshop item. Run it when a test session is
             over, or the next launch keeps running the dev build.

  -Direct    Copies the dll straight into UE4SS's Mods folder, bypassing the loader entirely. Fast
             for iterating inside a single running session, but it is NOT a real test: it does not
             exercise the loader's install/reinstall path at all, and on 2026-08-21 that gap cost an
             entire round of user testing when -Direct-only deploys were silently reverted by the
             loader's reinstall on the next launch. Never trust a fix as verified from -Direct alone.
             Requires Palworld closed, since the dll is locked while the game holds it open.

  Every build is checked with tools\Check-Ue4ssAbi.py against the UE4SS export lists in
  tools\ue4ss-exports\ (the Workshop build and the GitHub/Nexus release), so a dll that would
  fail to load under one of them stops here.

  -Release   Writes the two publishable packages:
               out/PalpediaEnhancer-<version>.zip        Steam Workshop. Loader package: Info.json
                                                         exactly as committed (DebugMode false,
                                                         depends on the Workshop UE4SS), dlls/, Assets/.
               out/PalpediaEnhancer-<version>-nexus.zip  Nexus Mods. Full-game-path UE4SS mod
                                                         (Pal\Binaries\Win64\ue4ss\Mods\
                                                         PalpediaEnhancer\ with enabled.txt, no
                                                         Info.json) for a UE4SS from Nexus Mods;
                                                         the Pal\ prefix is what Vortex needs.
             Same dll and assets in both; only the wrapper differs. Never upload the staged test copy.

  Publishing is NOT done here. The separate publishing tool takes <this repo> and
             builds the release through -Release, renders the localized Workshop descriptions and
             uploads through the Steamworks API from out\, so Steam's own subscribed folder is
             never written by hand.

Pass any combination. Passing none runs -Package only.
#>
[CmdletBinding()]
param(
    [switch]$Direct,
    [switch]$Package,
    [switch]$Release,
    [switch]$Unstage,
    [switch]$SkipBuild,
    [string]$ToolchainRoot = "C:\Dev\RE-UE4SS",
    [string]$XmakeExe = "C:\Dev\xmake293\xmake.exe",
    [string]$GameRoot = "D:\Steam\steamapps\common\Palworld",
    [string]$LocalPackageId = "9000000001"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageName = "PalpediaEnhancer"

if (-not $Direct -and -not $Package -and -not $Release -and -not $Unstage) { $Package = $true }
if ($Unstage) { $SkipBuild = $true }

$devPackageName = "$($packageName)Dev"
$settingsIni = Join-Path $GameRoot "Mods\PalModSettings.ini"

# The loader's enable switch: one "ActiveModList=<PackageName>" line per enabled package in
# PalModSettings.ini, the same file Options -> Mod Management writes. Exactly one of the released
# package and the dev package is ever enabled.
function Set-ActivePackage([string]$enable, [string]$disable) {
    $lines = Get-Content -LiteralPath $settingsIni
    $lines = @($lines | Where-Object { $_ -ne "ActiveModList=$disable" })
    if (-not ($lines -contains "ActiveModList=$enable")) {
        $index = [array]::FindLastIndex($lines, [Predicate[string]]{ param($l) $l -like "ActiveModList=*" })
        if ($index -lt 0) { $index = $lines.Count - 1 }
        $lines = $lines[0..$index] + @("ActiveModList=$enable") + $(if ($index + 1 -lt $lines.Count) { $lines[($index + 1)..($lines.Count - 1)] } else { @() })
    }
    Set-Content -LiteralPath $settingsIni -Value $lines -Encoding UTF8
    Write-Host "Loader enable switch: $enable on, $disable off"
}

if ($Unstage) {
    $workshopRoot = (Select-String -Path $settingsIni -Pattern "^WorkshopRootDir=(.+)$").Matches[0].Groups[1].Value.Trim()
    foreach ($p in (Join-Path $workshopRoot $LocalPackageId),
                   (Join-Path $GameRoot "Mods\NativeMods\UE4SS\Mods\$devPackageName"),
                   (Join-Path $GameRoot "Mods\ManagedMods\$devPackageName")) {
        if (Test-Path $p) { Remove-Item -LiteralPath $p -Recurse -Force; Write-Host "Removed $p" }
    }
    Set-ActivePackage -enable $packageName -disable $devPackageName
    Write-Host "Dev build unstaged. Next launch runs the released Workshop item."
    return
}

if ($Direct -and -not $Package) {
    Write-Warning "-Direct alone does not exercise the loader's real install path (see -Package in the synopsis). Do not report a fix as verified from this alone; run -Package too before trusting a relaunch test."
}

# Only the direct install writes over a dll the running game holds open. Package staging writes
# somewhere the game never loads from, so it is safe with Palworld open.
if ($Direct -and (Get-Process -Name "Palworld*" -ErrorAction SilentlyContinue)) {
    throw "Palworld is running. Close it first, or use -Package only; the installed dll is locked while the game is loaded."
}

# Info.json is the version source and dllmain.cpp only mirrors it for the load line. Nothing in
# the build made them agree, and they silently drifted a whole release apart (1.10.0 in the dll
# against 1.11.0 everywhere else, found 2026-09-07), so the build refuses to run while they differ.
$declaredVersion = (Get-Content -Raw -LiteralPath (Join-Path $repoRoot "Info.json") | ConvertFrom-Json).Version
$sourceVersion = [regex]::Match(
    (Get-Content -Raw -LiteralPath (Join-Path $repoRoot "Source\PalpediaEnhancer\src\dllmain.cpp")),
    'ModVersion\s*=\s*STR\("([^"]+)"\)').Groups[1].Value
if ($sourceVersion -ne $declaredVersion) {
    throw "Version mismatch: Info.json says $declaredVersion, dllmain.cpp ModVersion says $sourceVersion. Bump both together."
}

if (-not $SkipBuild) {
    Write-Host "Building $packageName..."
    # Invoked with -P from outside the toolchain tree on purpose: running xmake with the
    # toolchain as the working directory fails with "toolchain dotnet not found" on this machine.
    # Rust lives in the rustup toolchain folder on this machine (.cargo\bin is empty).
    $env:PATH = "$env:USERPROFILE\.rustup\toolchains\stable-x86_64-pc-windows-msvc\bin;$env:USERPROFILE\.cargo\bin;$env:PATH"
    & $XmakeExe build -P $ToolchainRoot $packageName
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

$built = Join-Path $ToolchainRoot "Binaries\Game__Shipping__Win64\$packageName\$packageName.dll"
if (-not (Test-Path $built)) { throw "Built dll not found at $built" }

# The dll must load under every UE4SS build players run: the Workshop item and the GitHub/Nexus
# release are different builds with different export tables, and a mod importing a symbol one of
# them lacks silently never starts there. tools\ue4ss-exports\ holds each build's export list.
$exportLists = Get-ChildItem -Path (Join-Path $PSScriptRoot "ue4ss-exports") -Filter "*.txt" | Select-Object -ExpandProperty FullName
& python (Join-Path $PSScriptRoot "Check-Ue4ssAbi.py") $built @exportLists
if ($LASTEXITCODE -ne 0) { throw "The built dll imports UE4SS symbols that one supported UE4SS build does not export (see above)." }

# The loader package expects dlls/main.dll next to Info.json, matching how every other native
# UE4SS mod on this machine ships.
$stagedDlls = Join-Path $repoRoot "dlls"
New-Item -ItemType Directory -Force -Path $stagedDlls | Out-Null
Copy-Item -LiteralPath $built -Destination (Join-Path $stagedDlls "main.dll") -Force
Write-Host "Staged dlls/main.dll ($((Get-Item $built).Length) bytes)"

$assets = Join-Path $repoRoot "Assets"

if ($Direct) {
    $target = Join-Path $GameRoot "Mods\NativeMods\UE4SS\Mods\$packageName\dlls"
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item -LiteralPath $built -Destination (Join-Path $target "main.dll") -Force

    if (Test-Path $assets) {
        $assetTarget = Join-Path $GameRoot "Mods\NativeMods\UE4SS\Mods\$packageName\Assets"
        New-Item -ItemType Directory -Force -Path $assetTarget | Out-Null
        Copy-Item -Path (Join-Path $assets "*") -Destination $assetTarget -Recurse -Force
    }

    $modsTxt = Join-Path $GameRoot "Mods\NativeMods\UE4SS\Mods\mods.txt"
    if (-not (Select-String -Path $modsTxt -Pattern "^$packageName" -Quiet)) {
        Add-Content -Path $modsTxt -Value "$packageName : 1"
    }
    Write-Host "Direct install done: $target\main.dll (fast loop only, not a verified test; see -Package)"
}

if ($Package) {
    # The loader manages its own mods.txt entry. A hand-added duplicate keeps the mod loading after
    # it is disabled in Mod Management, which makes that toggle look broken.
    $modsTxt = Join-Path $GameRoot "Mods\NativeMods\UE4SS\Mods\mods.txt"
    $lines = Get-Content $modsTxt
    $matches = @($lines | Where-Object { $_ -match "^$devPackageName\s*:" })
    if ($matches.Count -gt 1) {
        $seen = $false
        $lines = $lines | ForEach-Object {
            if ($_ -match "^$devPackageName\s*:") {
                if ($seen) { return }
                $seen = $true
            }
            $_
        }
        Set-Content -Path $modsTxt -Value $lines -Encoding ASCII
        Write-Host "Removed $($matches.Count - 1) duplicate mods.txt entry for $devPackageName"
    }

    $workshopRoot = (Select-String -Path (Join-Path $GameRoot "Mods\PalModSettings.ini") -Pattern "^WorkshopRootDir=(.+)$").Matches[0].Groups[1].Value.Trim()
    $staging = Join-Path $workshopRoot $LocalPackageId

    New-Item -ItemType Directory -Force -Path (Join-Path $staging "dlls") | Out-Null
    # A separate package so the loader installs it beside, never over, the released item. The
    # staged copy must reinstall on every launch; the committed manifest must not.
    $info = Get-Content -LiteralPath (Join-Path $repoRoot "Info.json") -Raw | ConvertFrom-Json
    $info.PackageName = $devPackageName
    $info.ModName = "$($info.ModName) (Dev)"
    $info.DebugMode = $true
    $info | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $staging "Info.json") -Encoding UTF8
    Copy-Item -LiteralPath $built -Destination (Join-Path $staging "dlls\main.dll") -Force
    # Info.json names thumbnail.png; without it the Mod Management tile is a white square.
    Copy-Item -LiteralPath (Join-Path $repoRoot "thumbnail.png") -Destination $staging -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE.md") -Destination $staging -Force

    if (Test-Path $assets) {
        New-Item -ItemType Directory -Force -Path (Join-Path $staging "Assets") | Out-Null
        Copy-Item -Path (Join-Path $assets "*") -Destination (Join-Path $staging "Assets") -Recurse -Force
    }
    Set-ActivePackage -enable $devPackageName -disable $packageName
    Write-Host "Dev package staged at $staging"
    Write-Host "Fully close and relaunch Palworld: DebugMode reinstalls from this staged copy on launch, and only a real relaunch proves it."
    Write-Host "When the test session is over: tools/deploy.ps1 -Unstage (re-enables the released Workshop item)."
}

if ($Release) {
    # The ONLY identity that ever ships is the released one. Anything else in the committed
    # manifest (a dev name, debug mode, a stray suffix) stops the release here.
    $info = Get-Content -LiteralPath (Join-Path $repoRoot "Info.json") -Raw | ConvertFrom-Json
    if ($info.PackageName -ne $packageName) { throw "Info.json PackageName is '$($info.PackageName)'; a release must be exactly '$packageName'." }
    if ($info.ModName -ne "Palpedia Enhancer") { throw "Info.json ModName is '$($info.ModName)'; a release must be exactly 'Palpedia Enhancer'." }
    if ($info.DebugMode) { throw "Info.json has DebugMode true; a release package must ship with it false." }
    $outDir = Join-Path $repoRoot "out"
    $pkgDir = Join-Path $outDir $packageName
    if (Test-Path $pkgDir) { Remove-Item -Recurse -Force $pkgDir }
    New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "dlls") | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot "Info.json") -Destination $pkgDir -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE.md") -Destination $pkgDir -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "thumbnail.png") -Destination $pkgDir -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "PACKAGE_README.txt") -Destination (Join-Path $pkgDir "README.txt") -Force
    Copy-Item -LiteralPath $built -Destination (Join-Path $pkgDir "dlls\main.dll") -Force
    if (Test-Path $assets) {
        New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "Assets") | Out-Null
        Copy-Item -Path (Join-Path $assets "*") -Destination (Join-Path $pkgDir "Assets") -Recurse -Force
    }
    $zip = Join-Path $outDir "$packageName-$($info.Version).zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path $pkgDir -DestinationPath $zip
    Write-Host "Steam Workshop package written to $zip"

    # Nexus Mods package: the same dll and assets as a plain UE4SS mod. A Nexus or GitHub UE4SS
    # lives in Pal\Binaries\Win64\ue4ss\ and loads Mods\<Name>\dlls\main.dll when enabled.txt
    # exists. The zip carries the FULL game path, Pal\Binaries\Win64\ue4ss\Mods\PalpediaEnhancer\,
    # because that is the one layout every version of Vortex's Palworld extension auto-installs:
    # its root-mod installer (present since 0.1.5) matches a top-level Pal\ folder and deploys the
    # archive verbatim to the game root. The previous ue4ss\Mods\... layout matched NO installer on
    # extensions before 0.5.0 (no cpp installer until 2026-08-11), so Vortex dumped it in the wrong
    # place (reported by marr75 on Nexus, 2026-08-23). Manual users extract into the Palworld
    # install folder. No Info.json: the Palworld loader is not involved.
    $nexusRoot = Join-Path $outDir "nexus"
    if (Test-Path $nexusRoot) { Remove-Item -Recurse -Force $nexusRoot }
    $nexusDir = Join-Path $nexusRoot "Pal\Binaries\Win64\ue4ss\Mods\$packageName"
    New-Item -ItemType Directory -Force -Path (Join-Path $nexusDir "dlls") | Out-Null
    Copy-Item -LiteralPath $built -Destination (Join-Path $nexusDir "dlls\main.dll") -Force
    if (Test-Path $assets) {
        New-Item -ItemType Directory -Force -Path (Join-Path $nexusDir "Assets") | Out-Null
        Copy-Item -Path (Join-Path $assets "*") -Destination (Join-Path $nexusDir "Assets") -Recurse -Force
    }
    Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE.md") -Destination $nexusDir -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "PACKAGE_README_NEXUS.txt") -Destination (Join-Path $nexusDir "README.txt") -Force
    Set-Content -LiteralPath (Join-Path $nexusDir "enabled.txt") -Value "" -NoNewline
    $nexusZip = Join-Path $outDir "$packageName-$($info.Version)-nexus.zip"
    if (Test-Path $nexusZip) { Remove-Item -Force $nexusZip }
    Compress-Archive -Path (Join-Path $nexusRoot "Pal") -DestinationPath $nexusZip
    Write-Host "Nexus Mods package written to $nexusZip"
}

