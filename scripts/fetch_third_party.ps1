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
        $url = "https://media.xiph.org/rnnoise/models/$model"
        Write-Host ">> downloading RNNoise model $model"
        $tgz = Join-Path $TP "rnnoise\$model"
        Invoke-WebRequest -Uri $url -OutFile $tgz
        & tar -xzf $tgz -C (Join-Path $TP "rnnoise")
        Remove-Item $tgz -Force
    } else {
        Write-Warning "could not determine RNNoise model name; run download_model.sh in Git Bash"
    }
}

Write-Host ""
Write-Host "third_party/ ready:"
Get-ChildItem $TP -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
