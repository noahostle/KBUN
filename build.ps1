param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot "build"
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) {
    & $cmake.Source -S $projectRoot -B $buildRoot -A x64
    & $cmake.Source --build $buildRoot --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    if ($RunTests) {
        & ctest --test-dir $buildRoot -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    exit 0
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vsRoot) {
    $fallback = "C:\Program Files\Microsoft Visual Studio\2022\Community"
    if (Test-Path $fallback) { $vsRoot = $fallback }
}
if (-not $vsRoot) {
    throw "Visual Studio C++ tools were not found. Install the Desktop development with C++ workload."
}

$devCmd = Join-Path $vsRoot "Common7\Tools\VsDevCmd.bat"
$resourceOutput = Join-Path $buildRoot "KBUN.res"
$exeOutput = Join-Path $buildRoot "KBUN.exe"
$debugFlags = if ($Configuration -eq "Debug") { "/Od /Zi /DDEBUG" } else { "/O2 /DNDEBUG" }
$sources = @(
    "src\main.cpp",
    "src\app.cpp",
    "src\automation.cpp",
    "src\config.cpp",
    "src\hints.cpp",
    "src\overlay.cpp"
) -join " "

$compile = "rc /nologo /fo `"$resourceOutput`" resources\KBUN.rc && " +
    "cl /nologo /std:c++20 /EHsc /permissive- /W4 $debugFlags /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX " +
    "$sources `"$resourceOutput`" /Fe:`"$exeOutput`" /link /SUBSYSTEM:WINDOWS " +
    "advapi32.lib dwmapi.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib shlwapi.lib uiautomationcore.lib user32.lib"

$command = "call `"$devCmd`" -arch=x64 -host_arch=x64 && cd /d `"$projectRoot`" && $compile"
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($RunTests) {
    $testExe = Join-Path $buildRoot "kbun_hints_test.exe"
    $testCompile = "cl /nologo /std:c++20 /EHsc /permissive- /W4 $debugFlags /Isrc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX " +
        "tests\hints_test.cpp src\hints.cpp /Fe:`"$testExe`""
    $testCommand = "call `"$devCmd`" -arch=x64 -host_arch=x64 && cd /d `"$projectRoot`" && $testCompile && `"$testExe`""
    & $env:ComSpec /d /s /c $testCommand
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "Built $exeOutput"
