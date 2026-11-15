<#
.SYNOPSIS
    Windows PowerShell driver for the pokemonweb Emscripten / WSL2 WASM build.

.DESCRIPTION
    Audits WSL2 distribution availability, converts Windows paths to /mnt/<drive>/...,
    can provision Ubuntu/Debian + emsdk inside WSL2 via tools/setup_wsl_toolchain.sh,
    and invokes engine/wasm_build.mk to compile into engine/build/wasm/.

.PARAMETER CheckOnly
    Verify WSL2 distro, toolchain, and engine layout without modifying files.

.PARAMETER SetupToolchain
    Run tools/setup_wsl_toolchain.sh inside WSL2 (installs apt deps + ~/.emsdk).

.PARAMETER WithGbaGcc
    Passed with -SetupToolchain to also install gcc-arm-none-eabi for vanilla GBA builds.

.PARAMETER SmokeTest
    Build ONLY the standalone platform shim smoke test (engine/build/wasm/smoke.js + .wasm).

.PARAMETER HostTools
    Build native host utilities in engine/tools/ inside WSL2.

.PARAMETER Clean
    Clean WASM build outputs under engine/build/wasm/.

.PARAMETER PublishToPublic
    Copy compiled index.js / index.wasm from engine/build/wasm/ into public/ after build.

.PARAMETER ChunkAssets
    When publishing assets, invoke python tools/chunk_assets.py (<19 MiB .partXX).

.PARAMETER Distro
    Explicit WSL distro name (defaults to the default/first installed distro).

.PARAMETER Jobs
    Parallel make jobs (defaults to number of logical CPU cores).
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$SetupToolchain,
    [switch]$WithGbaGcc,
    [switch]$SmokeTest,
    [switch]$HostTools,
    [switch]$Clean,
    [switch]$PublishToPublic,
    [switch]$ChunkAssets,
    [string]$Distro = "",
    [int]$Jobs = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$EngineDir = Join-Path $RepoRoot "engine"
$BuildOutDir = Join-Path $EngineDir "build\wasm"
$PublicDir = Join-Path $RepoRoot "public"

if ($Jobs -le 0) {
    $Jobs = [Math]::Max(1, [Environment]::ProcessorCount)
}

function Convert-ToWslPath {
    param([Parameter(Mandatory = $true)][string]$WindowsPath)
    $full = [System.IO.Path]::GetFullPath($WindowsPath)
    if ($full -match '^([A-Za-z]):\\(.*)$') {
        $drive = $Matches[1].ToLowerInvariant()
        $rest = $Matches[2] -replace '\\', '/'
        return "/mnt/$drive/$rest"
    }
    throw "Cannot convert non-drive path to WSL path: $WindowsPath"
}

function Get-InstalledWslDistros {
    $wslCmd = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wslCmd) {
        return @()
    }
    # wsl.exe prints UTF-16LE even when captured; strip NUL chars cleanly.
    $prevEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::Unicode
        $raw = & wsl.exe -l -q 2>$null
        $exit = $LASTEXITCODE
    }
    finally {
        [Console]::OutputEncoding = $prevEncoding
    }
    if ($exit -ne 0 -or -not $raw) {
        return @()
    }
    $distros = @()
    foreach ($line in $raw) {
        $clean = ($line -replace "`0", "").Trim()
        if ($clean -and $clean -notmatch 'docker-desktop') {
            $distros += $clean
        }
    }
    return $distros
}

function Invoke-WslBash {
    param(
        [Parameter(Mandatory = $true)][string]$SelectedDistro,
        [Parameter(Mandatory = $true)][string]$Command
    )
    # Encode script as base64 so Windows PowerShell 5.1 never strips quotes or
    # mangles $HOME/newlines, while keeping stdin attached to the terminal for sudo.
    $normalized = $Command -replace "`r`n", "`n"
    $b64 = [Convert]::ToBase64String([System.Text.Encoding]::UTF8.GetBytes($normalized))
    $wrapper = "source <(printf %s $b64 | base64 -d)"
    $args = @("-d", $SelectedDistro, "--", "bash", "-lc", $wrapper)
    & wsl.exe @args | Out-Default
    return [int]$LASTEXITCODE
}

$WslExe = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $WslExe) {
    Write-Error "wsl.exe was not found on PATH. Install WSL2 first: wsl --install -d Ubuntu-24.04"
    exit 2
}

$InstalledDistros = @(Get-InstalledWslDistros)
if ($InstalledDistros.Count -eq 0) {
    Write-Host ""
    Write-Host "[build_wasm] WSL2 runtime detected, but NO Linux distribution is installed yet." -ForegroundColor Yellow
    Write-Host "[build_wasm] Action required (run in an interactive/elevated PowerShell):" -ForegroundColor Yellow
    Write-Host "    wsl --install -d Ubuntu-24.04" -ForegroundColor Cyan
    Write-Host "[build_wasm] After Ubuntu finishes first-boot user setup, run:" -ForegroundColor Yellow
    Write-Host "    .\tools\build_wasm.ps1 -SetupToolchain -HostTools" -ForegroundColor Cyan
    Write-Host ""
    exit 2
}

$TargetDistro = $Distro
if (-not $TargetDistro) {
    $TargetDistro = $InstalledDistros[0]
}
elseif ($InstalledDistros -notcontains $TargetDistro) {
    Write-Error "Requested WSL distro '$TargetDistro' not found. Installed: $($InstalledDistros -join ', ')"
    exit 2
}

if (-not (Test-Path (Join-Path $EngineDir "Makefile"))) {
    Write-Error "Engine repository not found under $EngineDir. Clone rh-hideout/pokeemerald-expansion first."
    exit 1
}

$WslRepoRoot = Convert-ToWslPath $RepoRoot
$WslEngineDir = Convert-ToWslPath $EngineDir
Write-Host "[build_wasm] Using WSL distro '$TargetDistro' (repo: $WslRepoRoot, jobs: $Jobs)" -ForegroundColor Green

if ($SetupToolchain) {
    $setupFlags = @()
    if ($WithGbaGcc) { $setupFlags += "--with-gba-gcc" }
    if ($HostTools) { $setupFlags += "--build-tools" }
    $flagStr = ($setupFlags -join " ")
    $cmd = "bash '$WslRepoRoot/tools/setup_wsl_toolchain.sh' $flagStr"
    $rc = Invoke-WslBash -SelectedDistro $TargetDistro -Command $cmd
    if ($rc -ne 0) {
        if ($rc -eq 3) {
            Write-Host "[build_wasm] WSL apt-get requires interactive sudo. Open Ubuntu once with 'wsl -d $TargetDistro' and run:" -ForegroundColor Yellow
            Write-Host "    bash '$WslRepoRoot/tools/setup_wsl_toolchain.sh' $flagStr" -ForegroundColor Cyan
        }
        exit $rc
    }
    if ($CheckOnly) {
        exit 0
    }
}

if ($CheckOnly) {
    $cmd = "bash '$WslRepoRoot/tools/setup_wsl_toolchain.sh' --check"
    $rc = Invoke-WslBash -SelectedDistro $TargetDistro -Command $cmd
    exit $rc
}

if ($Clean) {
    $cmd = "make -C '$WslEngineDir' -f wasm_build.mk clean"
    $rc = Invoke-WslBash -SelectedDistro $TargetDistro -Command $cmd
    if ($rc -ne 0) { exit $rc }
    if (-not $SmokeTest -and -not $HostTools) {
        exit 0
    }
}

$EmsdkSource = 'if [ -f "$HOME/.emsdk/emsdk_env.sh" ]; then EMSDK_QUIET=1 source "$HOME/.emsdk/emsdk_env.sh"; else echo "[build_wasm] ERROR: ~/.emsdk/emsdk_env.sh not found. Run -SetupToolchain first." >&2; exit 4; fi'

if ($HostTools -and -not $SetupToolchain) {
    $cmd = "make -C '$WslEngineDir' -f wasm_build.mk host-tools -j$Jobs"
    $rc = Invoke-WslBash -SelectedDistro $TargetDistro -Command $cmd
    if ($rc -ne 0) { exit $rc }
}

$makeTarget = if ($SmokeTest) { "wasm-smoke" } else { "wasm" }
$buildCmd = "$EmsdkSource && make -C '$WslEngineDir' -f wasm_build.mk $makeTarget -j$Jobs"
$rc = Invoke-WslBash -SelectedDistro $TargetDistro -Command $buildCmd
if ($rc -ne 0) {
    exit $rc
}

Write-Host "[build_wasm] Build succeeded -> $BuildOutDir" -ForegroundColor Green

if ($PublishToPublic) {
    if ($SmokeTest) {
        Write-Warning "-PublishToPublic ignored for -SmokeTest (keeps public/ clean for real index.js/index.wasm)."
        exit 0
    }
    $srcJs = Join-Path $BuildOutDir "index.js"
    $srcWasm = Join-Path $BuildOutDir "index.wasm"
    if (-not (Test-Path $srcJs) -or -not (Test-Path $srcWasm)) {
        Write-Error "Missing expected build outputs: $srcJs / $srcWasm"
        exit 1
    }
    Copy-Item -Force $srcJs (Join-Path $PublicDir "index.js")
    Copy-Item -Force $srcWasm (Join-Path $PublicDir "index.wasm")
    Write-Host "[build_wasm] Published index.js and index.wasm to $PublicDir" -ForegroundColor Cyan

    if ($ChunkAssets) {
        $chunkScript = Join-Path $RepoRoot "tools\chunk_assets.py"
        if (Test-Path $chunkScript) {
            & python $chunkScript --public-dir $PublicDir --output-dir (Join-Path $PublicDir "chunks")
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
}
