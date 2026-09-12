# Windows equivalent of fetch_third_party.sh (Windows PowerShell 5.1 compatible).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\fetch_third_party.ps1 [-Shallow]
param([switch]$Shallow)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$TP = Join-Path $Root "third_party"
New-Item -ItemType Directory -Force $TP | Out-Null

$refs = @{
    rnnoise       = @{ url = "https://github.com/xiph/rnnoise";          ref = "v0.2";   recursive = $false }
    "whisper.cpp" = @{ url = "https://github.com/ggml-org/whisper.cpp";  ref = "v1.7.5"; recursive = $false }
    ctranslate2   = @{ url = "https://github.com/OpenNMT/CTranslate2";   ref = "v4.5.0"; recursive = $true }
    sentencepiece = @{ url = "https://github.com/google/sentencepiece";  ref = "v0.2.0"; recursive = $false }
    "llama.cpp"   = @{ url = "https://github.com/ggml-org/llama.cpp";    ref = "b5030";  recursive = $false }
}

foreach ($name in $refs.Keys) {
    $dest = Join-Path $TP $name
    if (Test-Path (Join-Path $dest ".git")) {
        Write-Host ">> $name already present"
        continue
    }
    $r = $refs[$name]
    Write-Host ">> cloning $name @ $($r.ref)"
    $args = @("clone", "--branch", $r.ref)
    if ($Shallow) { $args += "--depth"; $args += "1" }
    if ($r.recursive) { $args += "--recursive"; if ($Shallow) { $args += "--shallow-submodules" } }
    $args += $r.url; $args += $dest
    & git @args
    if ($LASTEXITCODE -ne 0) { throw "git clone failed for $name" }
}

# RNNoise weights: the upstream script needs bash/curl; on Windows fetch the tarball directly.
$rnData = Join-Path $TP "rnnoise\src\rnnoise_data.c"
$dl = Join-Path $TP "rnnoise\download_model.sh"
if ((Test-Path $dl) -and -not (Test-Path $rnData)) {
    $line = Get-Content $dl | Where-Object { $_ -match "^model=" } | Select-Object -First 1
    if ($line) {
        $model = ($line -split "=", 2)[1].Trim()
        Write-Host ">> downloading RNNoise model $model"
        $tgz = Join-Path $TP "rnnoise\$model"
        # media.xiph.org goes down for hours at a time; a copy sits on this repository's
        # "third-party" release. Either source must match the hash recorded in fetch_third_party.sh.
        $expected = "4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43"   # rnnoise_data-0b50c45.tar.gz
        $ok = $false
        foreach ($url in @("https://github.com/Kevin-KIM98/offline-translator/releases/download/third-party/$model",
                           "https://media.xiph.org/rnnoise/models/$model")) {
            try {
                Invoke-WebRequest -Uri $url -OutFile $tgz -TimeoutSec 120
                if ((Get-FileHash $tgz -Algorithm SHA256).Hash.ToLower() -eq $expected) { $ok = $true; break }
                Write-Warning "$url : hash mismatch, trying the next source"
            } catch {
                Write-Warning "$url : $($_.Exception.Message)"
            }
        }
        if (-not $ok) { throw "RNNoise weights: no source delivered $model" }
        & tar -xzf $tgz -C (Join-Path $TP "rnnoise")
        Remove-Item $tgz -Force
    } else {
        Write-Warning "could not determine RNNoise model name; run download_model.sh in Git Bash"
    }
}

# miniaudio is a single public-domain header; the desktop CLI uses it for `listen`.
$ma = Join-Path $TP "miniaudio\miniaudio.h"
if (-not (Test-Path $ma)) {
    Write-Host ">> downloading miniaudio"
    New-Item -ItemType Directory -Force (Split-Path $ma) | Out-Null
    $maRef = if ($env:MINIAUDIO_REF) { $env:MINIAUDIO_REF } else { "0.11.25" }
    Invoke-WebRequest -Uri "https://raw.githubusercontent.com/mackron/miniaudio/$maRef/miniaudio.h" -OutFile $ma
}

Write-Host ""
Write-Host "third_party/ ready:"
Get-ChildItem $TP -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
