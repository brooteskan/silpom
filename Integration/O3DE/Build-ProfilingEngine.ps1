#requires -Version 5.1
<#
.SYNOPSIS
Apply the frozen engine integration, build TG and process assets for profiling.
.DESCRIPTION
Uses a source engine, not an installed SDK. The default compatibility target is
SM 6.2 in a separate build directory. The saved tower settings and AA are untouched.
PlanOnly validates sources and prints commands without changing any files.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $EnginePath,
    [string] $ProjectPath = '',
    [string] $BuildPath = '',
    [ValidateSet('6_2', '6_7')] [string] $ShaderModel = '6_2',
    [ValidateRange(1, 64)] [int] $Parallel = 4,
    [string] $Generator = '',
    [string] $Python = 'python',
    [switch] $PlanOnly
)
$ErrorActionPreference = 'Stop'
$transcribing = $false
function Invoke-Checked {
    param([string] $Executable, [string[]] $Arguments)
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed (exit $LASTEXITCODE)." }
}
try {
    if (-not $ProjectPath) { $ProjectPath = Join-Path $PSScriptRoot '../..' }
    $projectRoot = (Resolve-Path -LiteralPath $ProjectPath).Path
    $engineRoot = (Resolve-Path -LiteralPath $EnginePath).Path
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot 'project.json'))) {
        throw 'Specify -ProjectPath pointing to TGProject.'
    }
    if (-not $BuildPath) { $BuildPath = Join-Path $projectRoot "build/profile-5700g-sm$ShaderModel" }
    $buildRoot = [IO.Path]::GetFullPath($BuildPath)
    $pythonExe = (Get-Command $Python -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $cmakeExe = (Get-Command cmake -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    $apply = @((Join-Path $PSScriptRoot 'apply.py'), '--engine', $engineRoot, '--project', $projectRoot)
    Invoke-Checked $pythonExe ($apply + @('--check-only'))
    $configure = @('-S', $projectRoot, '-B', $buildRoot, "-DCMAKE_MODULE_PATH=$($engineRoot.Replace('\', '/'))/cmake",
        "-DTG_DX12_SHADER_MODEL=$ShaderModel", '-DCMAKE_CONFIGURATION_TYPES=profile',
        '-DLY_DISABLE_TEST_MODULES=ON', '-DLY_UNITY_BUILD=ON')
    $cache = Join-Path $buildRoot 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) {
        if ($Generator) { $configure += @('-G', $Generator) }
        $configure += @('-A', 'x64')
    } else {
        $homeLine = Get-Content -LiteralPath $cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' }
        if ($homeLine -and [IO.Path]::GetFullPath(($homeLine -split '=', 2)[1]) -ne $projectRoot) {
            throw 'The build cache belongs to another project. Choose a fresh -BuildPath.'
        }
    }
    $targets = @('Editor', 'AssetProcessor', 'AssetProcessorBatch', 'AssetBuilder',
        'AtomShader.Builders', 'Atom_RHI_DX12.Builders', 'Atom_RHI_DX12.Private',
        'SilPOM', 'SilPOM.Editor', 'TGProject.GameLauncher')
    $build = @('--build', $buildRoot, '--config', 'profile', '--target') + $targets + @('--parallel', "$Parallel")
    $assetProcessor = Join-Path $buildRoot 'bin/profile/AssetProcessorBatch.exe'
    $processAssets = @("--project-path=$projectRoot", "--engine-path=$engineRoot", '--platforms=pc')
    if ($PlanOnly) {
        Write-Output "Source engine: $engineRoot"
        Write-Output "DX12 target: $ShaderModel; GPU/driver support must be validated on the profiling PC."
        Write-Output 'Will bootstrap engine Python, apply patches and bind user/project.json to this engine.'
        Write-Output ("Configure: " + ($configure | ConvertTo-Json -Compress))
        Write-Output ("Build: " + ($build | ConvertTo-Json -Compress))
        Write-Output ("Process assets: $assetProcessor " + ($processAssets | ConvertTo-Json -Compress))
        exit 0
    }
    $active = @(Get-Process Editor, AssetProcessor, AssetProcessorBatch, AssetBuilder -ErrorAction SilentlyContinue)
    if ($active.Count) { throw 'Save and close Editor and asset tools before applying engine changes or rebuilding.' }
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
    Start-Transcript -Path (Join-Path $buildRoot 'engine-integration-build.log') -Append | Out-Null
    $transcribing = $true
    Invoke-Checked $pythonExe $apply
    Invoke-Checked (Join-Path $engineRoot 'python/get_python.bat') @()
    $userDirectory = Join-Path $projectRoot 'user'
    New-Item -ItemType Directory -Path $userDirectory -Force | Out-Null
    $userJson = Join-Path $userDirectory 'project.json'
    $selection = if (Test-Path -LiteralPath $userJson) { Get-Content -LiteralPath $userJson -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
    $selection | Add-Member -NotePropertyName engine_path -NotePropertyValue ($engineRoot.Replace('\', '/')) -Force
    [IO.File]::WriteAllText($userJson, ($selection | ConvertTo-Json -Depth 100), (New-Object Text.UTF8Encoding($false)))
    Invoke-Checked $cmakeExe $configure
    Invoke-Checked $cmakeExe $build
    Invoke-Checked $assetProcessor $processAssets
    Invoke-Checked $pythonExe ($apply + @('--verify-only'))
    $silpomHead = & git -C (Join-Path $projectRoot 'Gems/silpom') rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Unable to record SILPOM revision.' }
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
    $record = [ordered]@{
        bundle = $manifest.bundle; engine_base = $manifest.components[0].upstream_commit
        engine_patch_sha256 = $manifest.components[0].patch_sha256; silpom_commit = "$silpomHead".Trim()
        dx12_shader_model = $ShaderModel; engine_path = $engineRoot; project_path = $projectRoot
        build_path = $buildRoot; completed_utc = [DateTime]::UtcNow.ToString('o')
    }
    [IO.File]::WriteAllText((Join-Path $buildRoot 'engine-integration-build.json'), ($record | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    Write-Output "Build and asset processing completed. Launch $buildRoot/bin/profile/Editor.exe with --project-path and --engine-path shown above."
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    if ($transcribing) { Stop-Transcript | Out-Null }
}
