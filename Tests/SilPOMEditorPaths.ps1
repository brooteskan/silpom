# Locations for SilPOM's Editor runners; no registration or file changes.
# The project defaults to the bundled fixture project (Tests/Fixtures/TowerBenchmark).
# The engine comes from -EnginePath, <project>/user/project.json engine_path,
# O3DE_ENGINE_PATH / O3DE_PATH, or the single registered engine whose name
# matches the project's "engine". Binaries default to <project>/build/windows/bin/profile.
function Resolve-SilPOMProjectPath {
    param([string] $ProjectPath = '')
    if (-not $ProjectPath) { $ProjectPath = Join-Path $PSScriptRoot 'Fixtures/TowerBenchmark' }
    $resolved = (Resolve-Path -LiteralPath $ProjectPath).Path
    if (-not (Test-Path -LiteralPath (Join-Path $resolved 'project.json'))) { throw "$resolved is not an O3DE project." }
    return $resolved
}

function Resolve-SilPOMEnginePath {
    param([Parameter(Mandatory = $true)] [string] $ProjectPath, [string] $EnginePath = '')
    $local = @{}
    $localFile = Join-Path $ProjectPath 'user/project.json'
    if (Test-Path -LiteralPath $localFile) { $local = Get-Content -LiteralPath $localFile -Raw | ConvertFrom-Json }
    $shared = Get-Content -LiteralPath (Join-Path $ProjectPath 'project.json') -Raw | ConvertFrom-Json
    $selected = $EnginePath
    if (-not $selected) { $selected = $local.engine_path }
    if (-not $selected) { $selected = $env:O3DE_ENGINE_PATH }
    if (-not $selected) { $selected = $env:O3DE_PATH }
    if ($selected) {
        if (-not [IO.Path]::IsPathRooted($selected)) { $selected = Join-Path $ProjectPath $selected }
        if (Test-Path -LiteralPath (Join-Path $selected 'engine.json') -PathType Leaf) { return (Resolve-Path -LiteralPath $selected).Path }
        throw "The selected engine is missing: $selected"
    }
    $expected = $shared.engine
    if ($local.engine) { $expected = $local.engine }
    $registration = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.o3de/o3de_manifest.json'
    $found = @()
    if (Test-Path -LiteralPath $registration) {
        foreach ($path in (Get-Content -LiteralPath $registration -Raw | ConvertFrom-Json).engines) {
            $json = Join-Path $path 'engine.json'
            if ((Test-Path -LiteralPath $json -PathType Leaf) -and (Get-Content -LiteralPath $json -Raw | ConvertFrom-Json).engine_name -eq $expected) {
                $found += (Resolve-Path -LiteralPath $path).Path
            }
        }
    }
    $found = @($found | Select-Object -Unique)
    if ($found.Count -ne 1) { throw "Pass -EnginePath, set engine_path in user/project.json or O3DE_ENGINE_PATH; found $($found.Count) registered engines named '$expected'." }
    return $found[0]
}

function Resolve-SilPOMBinaryPath {
    param([Parameter(Mandatory = $true)] [string] $ProjectPath, [string] $BinaryPath = '')
    if (-not $BinaryPath) { $BinaryPath = Join-Path $ProjectPath 'build/windows/bin/profile' }
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryPath 'Editor.exe'))) { throw "No Editor.exe in $BinaryPath; pass -BinaryPath." }
    return [IO.Path]::GetFullPath($BinaryPath)
}
