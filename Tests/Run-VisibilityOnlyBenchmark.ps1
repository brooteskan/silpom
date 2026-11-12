#requires -Version 5.1
<#
.SYNOPSIS
Compare baseline, Gather, historical hybrid and neural-only local shadows in VueLiveLink at 9/64 samples.
.DESCRIPTION
Requires the source-built Editor and processed assets. Opens a dedicated Editor,
uses fixed cameras and GPU timestamps, restores controls, and never saves the level.
Results and screenshots go under ignored user/SilPOMNeuralOnlyAB. The original
component settings, camera and console variables are restored after comparison.
#>
[CmdletBinding()]
param([string] $OutputPath = '', [ValidateRange(30, 2000)] [int] $Samples = 600)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
. (Join-Path $project 'scripts/ProjectPaths.ps1')
$engine = Resolve-TGEnginePath -ProjectPath $project
$bin = Join-Path $project 'build/windows/bin/profile'
$prefix = [IO.Path]::GetFullPath($bin).TrimEnd([char]92, [char]47) + [IO.Path]::DirectorySeparatorChar
$active = @(Get-CimInstance Win32_Process -Filter "Name = 'Editor.exe' OR Name = 'AssetProcessor.exe' OR Name = 'AssetProcessorBatch.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
if ($active.Count) { throw 'Save and close the TG Editor and Asset Processor before starting this comparison.' }
if (-not $OutputPath) { $OutputPath = Join-Path $project ('user/SilPOMNeuralOnlyAB/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$oldOutput = $env:TG_SILPOM_HEIGHT_AB_OUTPUT
$oldSamples = $env:TG_SILPOM_HEIGHT_AB_SAMPLES
$server = $null
try {
    $env:TG_SILPOM_HEIGHT_AB_OUTPUT = $output
    $env:TG_SILPOM_HEIGHT_AB_SAMPLES = "$Samples"
    $arguments = @("--project-path=$project", "--engine-path=$engine")
    $server = Start-Process -FilePath (Join-Path $bin 'AssetProcessor.exe') -ArgumentList ($arguments + '--start-hidden') -WorkingDirectory $project -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 5
    $script = Join-Path $PSScriptRoot 'visibility_only_editor_benchmark.py'
    $parameters = $arguments + @('--autotest_mode', '--skipWelcomeScreenDialog', '--rhi=dx12', '--runpython', $script)
    $editor = Start-Process -FilePath (Join-Path $bin 'Editor.exe') -ArgumentList $parameters -WorkingDirectory $project -WindowStyle Hidden -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log') -PassThru
    if (-not $editor.WaitForExit(1200000)) { Stop-Process -Id $editor.Id -Force; throw 'The dedicated benchmark editor timed out.' }
    Copy-Item -LiteralPath (Join-Path $project 'user/log/Editor.log') -Destination (Join-Path $output 'Editor.log')
    $results = Get-Content -LiteralPath (Join-Path $output 'results.json') -Raw | ConvertFrom-Json
    if (-not $results.passed) { throw $results.error }
    if (-not $results.restored) { throw 'The benchmark did not restore its controls.' }
    if ((Get-Content -LiteralPath (Join-Path $output 'Editor.log') -Raw) -match 'Trace::Assert') {
        throw 'The benchmark encountered an Editor assertion; inspect Editor.log.'
    }
    if ($editor.ExitCode -ne 0) { throw "The benchmark Editor exited with code $($editor.ExitCode)." }
    Write-Output "Comparison complete: $output"
} finally {
    if ($server -and -not $server.HasExited) {
        $null = $server.CloseMainWindow()
        if (-not $server.WaitForExit(15000)) { Stop-Process -Id $server.Id -Force }
    }
    if ($server -and $server.HasExited) {
        $children=@(Get-CimInstance Win32_Process -Filter "Name = 'AssetBuilder.exe'" |
            Where-Object { $_.ParentProcessId -eq $server.Id -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) })
        foreach ($child in $children) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    $env:TG_SILPOM_HEIGHT_AB_OUTPUT = $oldOutput
    $env:TG_SILPOM_HEIGHT_AB_SAMPLES = $oldSamples
}
