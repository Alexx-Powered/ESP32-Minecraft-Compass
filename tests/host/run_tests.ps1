param(
    [string]$IdfPath = 'D:\ESP\v5.5.5\esp-idf',
    [string]$VcVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$outputPath = Join-Path $projectPath 'build-host-tests'
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$cjsonPath = Join-Path $IdfPath 'components/json/cJSON'
foreach ($required in @($VcVars, (Join-Path $cjsonPath 'cJSON.c'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required tool/source missing: $required" }
}
$exePath = Join-Path $outputPath 'test_geojson.exe'
$compileArgs = @(
    '/nologo', '/TC', '/W3', '/WX', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS',
    " /I`"$PSScriptRoot/stubs`"", "/I`"$projectPath/main`"", "/I`"$cjsonPath`"",
    "`"$PSScriptRoot/test_geojson.c`"", "`"$projectPath/main/geo_config.c`"",
    "`"$cjsonPath/cJSON.c`"", "/Fe`"$exePath`"", "/Fo`"$outputPath\\`""
)
# cmd loads the Visual C++ environment only. File operations use PowerShell;
# this harness never connects to or flashes hardware.
$compileCommand = "`"$VcVars`" >nul && cl $($compileArgs -join ' ')"
& $env:ComSpec /d /s /c $compileCommand
if ($LASTEXITCODE -ne 0) { throw "Host compiler failed with $LASTEXITCODE" }
& $exePath (Join-Path $projectPath 'docs/examples/beihang-fence.geojson')
if ($LASTEXITCODE -ne 0) { throw "Tests failed with $LASTEXITCODE" }
