#requires -Version 5.1
<#
.SYNOPSIS
Check Level 5 proxy selection, raster depth, transitions, dynamic lighting and runtime decals; no benchmark.
.DESCRIPTION
Requires the source-built Editor and processed assets. Opens a dedicated Editor,
uses fixed cameras and render readbacks, restores controls, and never saves the level.
Temporarily changes one valid coefficient in each learned source bundle to verify
Asset Processor hot reload, then restores the original bytes even after failure.
Results and screenshots go under ignored user/SilPOMLevelFiveSmoke. The original
component settings, camera and console variables are restored after comparison.
#>
[CmdletBinding()]
param([string] $OutputPath = '', [switch] $LeaveOpen)
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
. (Join-Path $project 'scripts/ProjectPaths.ps1')
$engine = Resolve-TGEnginePath -ProjectPath $project
$bin = Join-Path $project 'build/windows/bin/profile'
$prefix = [IO.Path]::GetFullPath($bin).TrimEnd([char]92, [char]47) + [IO.Path]::DirectorySeparatorChar
$active = @(Get-CimInstance Win32_Process -Filter "Name = 'Editor.exe' OR Name = 'AssetProcessor.exe' OR Name = 'AssetProcessorBatch.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) })
if ($active.Count) { throw 'Save and close the TG Editor and Asset Processor before starting this comparison.' }
if (-not $OutputPath) { $OutputPath = Join-Path $project ('user/SilPOMLevelFiveSmoke/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Path $output -Force | Out-Null
$oldOutput = $env:TG_SILPOM_LEVEL_FIVE_TEST_OUTPUT
$oldLeaveOpen = $env:TG_SILPOM_LEVEL_FIVE_LEAVE_OPEN
$server = $null
$bundleBackups = @{}
foreach ($name in @('wall.l4mat','wall-shared.l4shared','wall-appearance.l5app','wall-depth.l5depth')) {
    $path = Join-Path $project ('Assets/SilPOMLearned/' + $name)
    $bundleBackups[$path] = [IO.File]::ReadAllBytes($path)
    [IO.File]::WriteAllBytes((Join-Path $output $name), $bundleBackups[$path])
}
try {
    $env:TG_SILPOM_LEVEL_FIVE_TEST_OUTPUT = $output
    $env:TG_SILPOM_LEVEL_FIVE_LEAVE_OPEN = if ($LeaveOpen) { '1' } else { '0' }
    $arguments = @("--project-path=$project", "--engine-path=$engine")
    $server = Start-Process -FilePath (Join-Path $bin 'AssetProcessor.exe') -ArgumentList ($arguments + '--start-hidden') -WorkingDirectory $project -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 5
    $script = Join-Path $PSScriptRoot 'level_five_editor_smoke.py'
    $parameters = $arguments + @('--autotest_mode', '--skipWelcomeScreenDialog', '--rhi=dx12', '--runpython', $script)
    $windowStyle = if ($LeaveOpen) { 'Normal' } else { 'Hidden' }
    $editor = Start-Process -FilePath (Join-Path $bin 'Editor.exe') -ArgumentList $parameters -WorkingDirectory $project -WindowStyle $windowStyle -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log') -PassThru
    if ($LeaveOpen) {
        $deadline = (Get-Date).AddMinutes(20)
        $complete = $false
        while ((Get-Date) -lt $deadline -and -not $editor.HasExited) {
            $path = Join-Path $output 'results.json'
            if (Test-Path -LiteralPath $path) {
                try {
                    $state = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
                    if ($state.left_open_with_4b_5b) { $complete = $true; break }
                    if ($state.error) { break }
                } catch { }
            }
            Start-Sleep -Seconds 2
        }
        if (-not $complete -and -not $editor.HasExited) { Stop-Process -Id $editor.Id -Force; throw 'Editor correctness check did not complete; inspect results.json.' }
    } elseif (-not $editor.WaitForExit(1200000)) { Stop-Process -Id $editor.Id -Force; throw 'The dedicated correctness-test editor timed out.' }
    Copy-Item -LiteralPath (Join-Path $project 'user/log/Editor.log') -Destination (Join-Path $output 'Editor.log')
    $results = Get-Content -LiteralPath (Join-Path $output 'results.json') -Raw | ConvertFrom-Json
    if (-not $results.passed) { throw $results.error }
    if (-not $results.restored -and -not ($LeaveOpen -and $results.left_open_with_4b_5b)) { throw 'The correctness test did not restore its controls.' }
    if ((Get-Content -LiteralPath (Join-Path $output 'Editor.log') -Raw) -match 'Trace::Assert|Failed to bind shader resources for draw item') {
        throw 'The correctness test encountered an assertion or a skipped draw from missing shader bindings; inspect Editor.log.'
    }
    if ($editor.HasExited -and $editor.ExitCode -ne 0) { throw "The correctness-test Editor exited with code $($editor.ExitCode)." }
    Write-Output "Correctness test complete: $output"
} finally {
    # Also restore if the editor process crashes before its Python finally runs.
    foreach ($path in $bundleBackups.Keys) {
        $actual = [IO.File]::ReadAllBytes($path)
        $expected = $bundleBackups[$path]
        if ([Convert]::ToBase64String($actual) -ne [Convert]::ToBase64String($expected)) {
            [IO.File]::WriteAllBytes($path, $expected)
        }
    }
    if ($server -and -not $server.HasExited -and -not ($LeaveOpen -and $complete)) {
        $null = $server.CloseMainWindow()
        if (-not $server.WaitForExit(15000)) { Stop-Process -Id $server.Id -Force }
    }
    if ($server -and $server.HasExited) {
        $children=@(Get-CimInstance Win32_Process -Filter "Name = 'AssetBuilder.exe'" |
            Where-Object { $_.ParentProcessId -eq $server.Id -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) })
        foreach ($child in $children) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
    }
    $env:TG_SILPOM_LEVEL_FIVE_TEST_OUTPUT = $oldOutput
    $env:TG_SILPOM_LEVEL_FIVE_LEAVE_OPEN = $oldLeaveOpen
}
