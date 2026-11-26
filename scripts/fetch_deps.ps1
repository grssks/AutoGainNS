$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tpDir = Join-Path $root 'third_party'
[void][IO.Directory]::CreateDirectory($tpDir)

$vstDir = Join-Path $tpDir 'vst3sdk'
if (-not (Test-Path (Join-Path $vstDir 'public.sdk\source\vst\vstaudioeffect.cpp'))) {
    Write-Host '[*] Cloning Steinberg VST 3 SDK...' -ForegroundColor Cyan
    git clone --depth 1 https://github.com/steinbergmedia/vst3sdk.git $vstDir
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone vst3sdk' }
    Push-Location $vstDir
    git submodule update --init --depth 1 base pluginterfaces public.sdk
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw 'Failed to init vst3sdk submodules' }
    Pop-Location
} else {
    Write-Host '[*] third_party\vst3sdk already present.' -ForegroundColor Green
}

$rnnDir = Join-Path $tpDir 'rnnoise'
if (-not (Test-Path (Join-Path $rnnDir 'src\denoise.c'))) {
    Write-Host '[*] Cloning Xiph.Org RNNoise...' -ForegroundColor Cyan
    git clone --depth 1 https://github.com/xiph/rnnoise.git $rnnDir
    if ($LASTEXITCODE -ne 0) { throw 'Failed to clone rnnoise' }
} else {
    Write-Host '[*] third_party\rnnoise already present.' -ForegroundColor Green
}

$rnnDataC = Join-Path $rnnDir 'src\rnnoise_data.c'
if (-not (Test-Path $rnnDataC)) {
    Write-Host '[*] Downloading official RNNoise model weights...' -ForegroundColor Cyan
    $modelVer = (Get-Content (Join-Path $rnnDir 'model_version') -Raw).Trim()
    $tarPath = Join-Path $tpDir "rnnoise_data-$modelVer.tar.gz"
    Invoke-WebRequest -Uri "https://media.xiph.org/rnnoise/models/rnnoise_data-$modelVer.tar.gz" -OutFile $tarPath
    $actualHash = (Get-FileHash -LiteralPath $tarPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $modelVer.ToLowerInvariant()) {
        Remove-Item -LiteralPath $tarPath -Force -ErrorAction SilentlyContinue
        throw "RNNoise model checksum mismatch: expected $modelVer, got $actualHash"
    }
    tar -xzf $tarPath -C $rnnDir
    if ($LASTEXITCODE -ne 0) { throw 'Failed to extract RNNoise model weights' }
    Remove-Item -LiteralPath $tarPath -Force -ErrorAction SilentlyContinue
} else {
    Write-Host '[*] RNNoise model weights already present.' -ForegroundColor Green
}

Write-Host '[*] All dependencies ready. Run .\build.ps1 to compile.' -ForegroundColor Green
