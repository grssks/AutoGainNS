param(
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

if (-not (Test-Path 'third_party\rnnoise\src\rnnoise_data.c') -or
    -not (Test-Path 'third_party\vst3sdk\public.sdk\source\vst\vstaudioeffect.cpp')) {
    throw 'Dependencies missing. Run .\scripts\fetch_deps.ps1 first.'
}

function Find-VCVars64 {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($installPath) {
            $candidate = Join-Path $installPath 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    $fallbacks = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    )
    foreach ($p in $fallbacks) {
        if (Test-Path $p) { return $p }
    }
    throw 'Visual Studio x64 C++ build tools (vcvars64.bat) not found.'
}

$vcvars = Find-VCVars64

$objDir = Join-Path $root 'build\obj'
$binDir = Join-Path $root 'out\AutoGainNS.vst3\Contents\x86_64-win'
[void][IO.Directory]::CreateDirectory($objDir)
[void][IO.Directory]::CreateDirectory($binDir)
[void][IO.Directory]::CreateDirectory((Join-Path $root 'build\test'))

$rnnoiseSources = @(
    'third_party\rnnoise\src\denoise.c',
    'third_party\rnnoise\src\rnn.c',
    'third_party\rnnoise\src\pitch.c',
    'third_party\rnnoise\src\kiss_fft.c',
    'third_party\rnnoise\src\celt_lpc.c',
    'third_party\rnnoise\src\nnet.c',
    'third_party\rnnoise\src\nnet_default.c',
    'third_party\rnnoise\src\parse_lpcnet_weights.c',
    'third_party\rnnoise\src\rnnoise_data.c',
    'third_party\rnnoise\src\rnnoise_tables.c'
)

$sdkBaseSources = @(
    'third_party\vst3sdk\pluginterfaces\base\conststringtable.cpp',
    'third_party\vst3sdk\pluginterfaces\base\coreiids.cpp',
    'third_party\vst3sdk\pluginterfaces\base\funknown.cpp',
    'third_party\vst3sdk\pluginterfaces\base\ustring.cpp',
    'third_party\vst3sdk\base\thread\source\fcondition.cpp',
    'third_party\vst3sdk\base\thread\source\flock.cpp',
    'third_party\vst3sdk\base\source\baseiids.cpp',
    'third_party\vst3sdk\base\source\fbuffer.cpp',
    'third_party\vst3sdk\base\source\fdebug.cpp',
    'third_party\vst3sdk\base\source\fdynlib.cpp',
    'third_party\vst3sdk\base\source\fobject.cpp',
    'third_party\vst3sdk\base\source\fstreamer.cpp',
    'third_party\vst3sdk\base\source\fstring.cpp',
    'third_party\vst3sdk\base\source\timer.cpp',
    'third_party\vst3sdk\base\source\updatehandler.cpp',
    'third_party\vst3sdk\public.sdk\source\common\commoniids.cpp',
    'third_party\vst3sdk\public.sdk\source\common\commonstringconvert.cpp',
    'third_party\vst3sdk\public.sdk\source\common\memorystream.cpp',
    'third_party\vst3sdk\public.sdk\source\common\pluginview.cpp',
    'third_party\vst3sdk\public.sdk\source\common\threadchecker_win32.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstaudioeffect.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstbus.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstcomponent.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstcomponentbase.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vsteditcontroller.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstinitiids.cpp',
    'third_party\vst3sdk\public.sdk\source\vst\vstparameters.cpp',
    'third_party\vst3sdk\public.sdk\source\main\dllmain.cpp',
    'third_party\vst3sdk\public.sdk\source\main\moduleinit.cpp',
    'third_party\vst3sdk\public.sdk\source\main\pluginfactory.cpp'
)

$pluginSources = @(
    'src\plugin\processor.cpp',
    'src\plugin\controller.cpp',
    'src\plugin\editor_view.cpp',
    'src\plugin\factory.cpp'
)

$cmdLines = @()
$cmdLines += "@echo off"
$cmdLines += "call `"$vcvars`" >nul"
$cmdLines += "if errorlevel 1 exit /b 1"

# Compile RNNoise C files with x86 runtime CPU dispatch (SSE2 baseline + SSE4.1 + isolated AVX2)
$rnnoiseCommonFlags = '/nologo /c /O2 /MT /DRNNOISE_BUILD /DRNN_ENABLE_X86_RTCD /DOPUS_X86_MAY_HAVE_SSE /DOPUS_X86_MAY_HAVE_SSE2 /D_CRT_SECURE_NO_WARNINGS /Ithird_party\rnnoise\include /Ithird_party\rnnoise\src /wd4244 /wd4305 /wd4018 /wd4267 /wd4996'
$allObjs = @()
$rnnoiseObjs = @()

$rnnoiseRtcdSources = @(
    @{ Path = 'third_party\rnnoise\src\x86\x86cpu.c';      Extra = '' },
    @{ Path = 'third_party\rnnoise\src\x86\x86_dnn_map.c'; Extra = '' },
    @{ Path = 'third_party\rnnoise\src\x86\nnet_sse4_1.c'; Extra = '/DOPUS_X86_MAY_HAVE_SSE4_1' },
    @{ Path = 'third_party\rnnoise\src\x86\nnet_avx2.c';   Extra = '/arch:AVX2 /DOPUS_X86_MAY_HAVE_SSE4_1 /DOPUS_X86_MAY_HAVE_AVX2' }
)

foreach ($src in $rnnoiseSources) {
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
    $allObjs += "`"$obj`""
    $rnnoiseObjs += "`"$obj`""
    $cmdLines += "echo [CC] $src"
    $cmdLines += "cl $rnnoiseCommonFlags `"$src`" /Fo`"$obj`""
    $cmdLines += "if errorlevel 1 exit /b 1"
}

foreach ($entry in $rnnoiseRtcdSources) {
    $src = $entry.Path
    $extra = $entry.Extra
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
    $allObjs += "`"$obj`""
    $rnnoiseObjs += "`"$obj`""
    $cmdLines += "echo [CC] $src"
    $cmdLines += "cl $rnnoiseCommonFlags $extra `"$src`" /Fo`"$obj`""
    $cmdLines += "if errorlevel 1 exit /b 1"
}

# Compile VST3 SDK + Plugin C++ files
$cppFlags = '/nologo /c /O2 /MT /EHsc /std:c++17 /utf-8 /DRELEASE=1 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Isrc /Ithird_party\rnnoise\include /Ithird_party\vst3sdk /wd4996 /wd4244 /wd4267'
foreach ($src in ($sdkBaseSources + $pluginSources)) {
    $obj = Join-Path $objDir ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
    $allObjs += "`"$obj`""
    $cmdLines += "echo [CXX] $src"
    $cmdLines += "cl $cppFlags `"$src`" /Fo`"$obj`""
    $cmdLines += "if errorlevel 1 exit /b 1"
}

$outDll = Join-Path $binDir 'AutoGainNS.vst3'
$objsJoined = $allObjs -join ' '
$cmdLines += "echo [LINK] $outDll"
$cmdLines += "link /nologo /DLL /OUT:`"$outDll`" /DEF:src\plugin\exports.def $objsJoined kernel32.lib user32.lib gdi32.lib ole32.lib advapi32.lib shell32.lib"
$cmdLines += "if errorlevel 1 exit /b 1"

if (-not $SkipTests) {
    $rnnoiseJoined = $rnnoiseObjs -join ' '
    $testSdkObjs = "`"$objDir\funknown.obj`" `"$objDir\coreiids.obj`" `"$objDir\commoniids.obj`" `"$objDir\conststringtable.obj`" `"$objDir\vstinitiids.obj`""
    $testExe = Join-Path $root 'build\test\test_dsp.exe'
    $cmdLines += "echo [TEST] Building test_dsp.exe"
    $cmdLines += "cl /nologo /O2 /MT /EHsc /std:c++17 /DNDEBUG /DUNICODE /D_UNICODE /Isrc /Ithird_party\rnnoise\include /Ithird_party\vst3sdk tests\test_dsp.cpp $rnnoiseJoined $testSdkObjs /Fo`"build\test\test_dsp.obj`" /Fe`"$testExe`" kernel32.lib user32.lib ole32.lib"
    $cmdLines += "if errorlevel 1 exit /b 1"
}

$cmdLines += "echo === DUMPBIN DEPENDENTS ==="
$cmdLines += "dumpbin /nologo /dependents `"$outDll`""

$batPath = Join-Path $root 'build\run_build.bat'
$cmdLines -join "`r`n" | Set-Content -LiteralPath $batPath -Encoding ascii

cmd /c "`"$batPath`""
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

# Remove linker import/export artifacts from the .vst3 bundle
Remove-Item -LiteralPath (Join-Path $binDir 'AutoGainNS.lib'), (Join-Path $binDir 'AutoGainNS.exp') -Force -ErrorAction SilentlyContinue

Write-Host "[*] Granting Read & Execute ACLs to Users and LOCAL SERVICE on out\AutoGainNS.vst3..." -ForegroundColor Cyan
icacls (Join-Path $root 'out\AutoGainNS.vst3') /grant "Users:(OI)(CI)(RX)" "NT AUTHORITY\LOCAL SERVICE:(OI)(CI)(RX)" /T | Out-Null

if (-not $SkipTests) {
    Write-Host "`n[*] Running automated DSP & VST3 test suite..." -ForegroundColor Cyan
    & (Join-Path $root 'build\test\test_dsp.exe')
    if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE" }
}
