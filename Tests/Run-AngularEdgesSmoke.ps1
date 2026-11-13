#requires -Version 5.1
[CmdletBinding()]
param([string] $OutputPath = '')
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../../..')).Path
. (Join-Path $project 'scripts/ProjectPaths.ps1')
$engine = Resolve-TGEnginePath -ProjectPath $project
$bin = Join-Path $project 'build/windows/bin/profile'
$active = @(Get-CimInstance Win32_Process -Filter "Name = 'Editor.exe'" | Where-Object { $_.ExecutablePath -eq (Join-Path $bin 'Editor.exe') })
if ($active.Count) { throw 'Save and close the TG Editor before the isolated estimator comparison.' }
if (-not $OutputPath) { $OutputPath = Join-Path $project ('user/SilPOMEstimatorSmoke/' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$previous = $env:TG_SILPOM_ESTIMATOR_OUTPUT
try {
    $env:TG_SILPOM_ESTIMATOR_OUTPUT = $output
    $script = Join-Path $PSScriptRoot 'AngularEdgesEditorSmoke.py'
    $parameters = @("--project-path=$project", "--engine-path=$engine", '--autotest_mode', '--skipWelcomeScreenDialog', '--rhi=dx12', '--runpython', $script)
    $testEditor = Start-Process -FilePath (Join-Path $bin 'Editor.exe') -ArgumentList $parameters -WorkingDirectory $project -WindowStyle Hidden -RedirectStandardOutput (Join-Path $output 'stdout.log') -RedirectStandardError (Join-Path $output 'stderr.log') -PassThru
    if (-not $testEditor.WaitForExit(1200000)) { Stop-Process -Id $testEditor.Id; throw 'Isolated estimator test timed out; inspect results.json.' }
    Copy-Item -LiteralPath (Join-Path $project 'user/log/Editor.log') -Destination (Join-Path $output 'Editor.log')
    $result = Get-Content -LiteralPath (Join-Path $output 'results.json') -Raw | ConvertFrom-Json
    if (-not $result.passed -or -not $result.restored) { throw $result.error }
    if ($testEditor.ExitCode -ne 0) { throw "Test Editor exited with code $($testEditor.ExitCode)." }
    if ((Get-Content -LiteralPath (Join-Path $output 'Editor.log') -Raw) -match 'Trace::Assert|Failed to bind shader resources for draw item') { throw 'Renderer assertion or missing binding; inspect Editor.log.' }
    Write-Output "Angular-edge comparison complete: $output"
} finally { $env:TG_SILPOM_ESTIMATOR_OUTPUT = $previous }
