#requires -Version 5.1
<#
.SYNOPSIS
Build offline SILPOM diagnostics, then evaluate or train checkpoint candidates.
.DESCRIPTION
Keeps shipping shaders, weights and levels unchanged. Reports/checkpoints go to
ignored project user data. The Editor can remain closed; no Editor is launched.
.EXAMPLE
./Evaluate-LevelOne.ps1
.EXAMPLE
./Evaluate-LevelOne.ps1 -Train -RunTests
#>
[CmdletBinding()]
param(
    [string] $PythonExe = '',
    [string] $Manifest = '',
    [string] $BuildPath = '',
    [string] $OutputPath = '',
    [int[]] $Profiles = @(1,3,4),
    [ValidateRange(30,1000000)] [int] $Rays = 4000,
    [ValidateRange(0,1000)] [int] $AuditRays = 12,
    [ValidateRange(100,1000000)] [int] $TrainingRays = 230000,
    [ValidateRange(30,1000000)] [int] $SelectionRays = 4000,
    [ValidateRange(1,100000)] [int] $Steps = 3300,
    [switch] $Train,
    [switch] $RunTests
)
$ErrorActionPreference = 'Stop'
$gem = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$project = (Resolve-Path -LiteralPath (Join-Path $gem '../..')).Path
$workspace = Split-Path -Parent $project
$cmake = (Get-Command cmake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
if (-not $PythonExe) {
    . (Join-Path $project 'scripts/ProjectPaths.ps1')
    $engine = Resolve-TGEnginePath -ProjectPath $project
    $engineId = (& $cmake -P (Join-Path $engine 'cmake/CalculateEnginePathId.cmake') $engine | Select-Object -Last 1).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $engineId) { throw 'Cannot find the O3DE Python environment; pass -PythonExe.' }
    $venvRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.o3de/Python/venv'
    $candidates = @((Join-Path $venvRoot "$engineId/Scripts/python.exe"))
    # A source checkout can use an existing SDK Python environment before its
    # own path-specific environment has been created. Probe installed venvs.
    if (Test-Path -LiteralPath $venvRoot) {
        $candidates += @(Get-ChildItem -LiteralPath $venvRoot -Directory | Sort-Object Name |
            ForEach-Object { Join-Path $_.FullName 'Scripts/python.exe' })
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        & $candidate -c 'import numpy; import PIL' *> $null
        if ($LASTEXITCODE -eq 0) { $PythonExe = $candidate; break }
    }
    if (-not $PythonExe) { throw 'No installed O3DE Python has numpy/Pillow; pass -PythonExe.' }
}
Write-Output "Using Python: $PythonExe"
& $PythonExe -c 'import numpy; import PIL'
if ($LASTEXITCODE -ne 0) { throw 'Use -PythonExe with a Python installation containing numpy and Pillow.' }
if (-not $Manifest) {
    $Manifest = Join-Path $workspace 'assets-source/Blender/tower-first/IrishFieldstoneKit/tower-export/welded-open-shaft-no-landings/experiment.json'
}
if (-not $BuildPath) { $BuildPath = Join-Path $project 'user/SilPOMLevelOne/build' }
if (-not $OutputPath) { $OutputPath = Join-Path $project ('user/SilPOMLevelOne/runs/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$configure = @('-S', $gem, '-B', $BuildPath, "-DSILPOM_PYTHON=$PythonExe")
if (-not (Test-Path -LiteralPath (Join-Path $BuildPath 'CMakeCache.txt'))) {
    $configure += @('-G', 'Visual Studio 18 2026', '-A', 'x64')
}
$dxc = Join-Path $project 'build/windows/bin/profile/Builders/DirectXShaderCompiler/dxc.exe'
if ($RunTests -and (Test-Path -LiteralPath $dxc)) { $configure += "-DSILPOM_DXC=$dxc" }
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'Offline tool configuration failed.' }
$build = @('--build', $BuildPath, '--config', 'Release', '--parallel', '4')
if (-not $RunTests) { $build += @('--target', 'silpom_level_one', 'silpom_welded_teacher') }
& $cmake @build -- /nr:false /nologo
if ($LASTEXITCODE -ne 0) { throw 'Offline tool build failed.' }
if ($RunTests) {
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    & $ctest --test-dir $BuildPath -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Level 1 regression tests failed.' }
}
$arguments = @((Join-Path $PSScriptRoot 'level_one.py'), $Manifest,
    (Join-Path $BuildPath 'Release'), $OutputPath, '--rays', "$Rays",
    '--audit-rays', "$AuditRays", '--training-rays', "$TrainingRays",
    '--selection-rays', "$SelectionRays", '--steps', "$Steps")
if ($Profiles.Count) { $arguments += '--profiles'; $arguments += @($Profiles | ForEach-Object { "$_" }) }
if ($Train) { $arguments += '--train' }
& $PythonExe @arguments
if ($LASTEXITCODE -ne 0) { throw 'Level 1 evaluation failed.' }
Write-Output "Level 1 evaluation complete: $OutputPath"
