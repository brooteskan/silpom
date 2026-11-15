param(
    [Parameter(Mandatory=$true)][string]$ConfigPath,
    [switch]$TemporaryDeveloperMode,
    [switch]$Elevated,
    [int]$TimeoutSeconds=240
)
$ErrorActionPreference='Stop'
$ConfigPath=(Resolve-Path -LiteralPath $ConfigPath).Path
$config=Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
$output=[string]$config.output
New-Item -ItemType Directory -Path $output -Force | Out-Null
$isAdmin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if($TemporaryDeveloperMode -and -not $isAdmin){
    if($Elevated){throw 'Administrator approval was not granted'}
    # The explicit switch authorizes a temporary Windows development setting.
    # The user handles the standard UAC prompt; no desktop automation is used.
    $arguments=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),
        '-ConfigPath',('"'+$ConfigPath+'"'),'-TemporaryDeveloperMode','-Elevated',
        '-TimeoutSeconds',[string]$TimeoutSeconds)
    $helper=Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $helper.WaitForExit()
    exit $helper.ExitCode
}
$registryPath='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
$valueName='AllowDevelopmentWithoutDevLicense'
$keyExisted=Test-Path -LiteralPath $registryPath
$valueExisted=$false
$oldValue=$null
$oldKind=$null
if($keyExisted){
    $key=Get-Item -LiteralPath $registryPath
    $valueExisted=$key.GetValueNames() -contains $valueName
    if($valueExisted){$oldValue=$key.GetValue($valueName);$oldKind=$key.GetValueKind($valueName)}
}
$state=[ordered]@{temporary_mode_requested=[bool]$TemporaryDeveloperMode;key_existed=$keyExisted;
    value_existed=$valueExisted;old_value=$oldValue;old_kind=[string]$oldKind;enabled=$false;restored=$false}
$statePath=Join-Path $output 'developer-mode-state.json'
$state | ConvertTo-Json | Set-Content -LiteralPath $statePath
$process=$null
$failed=$false
try{
    if($TemporaryDeveloperMode){
        if(-not $keyExisted){New-Item -Path $registryPath -Force | Out-Null}
        New-ItemProperty -LiteralPath $registryPath -Name $valueName -PropertyType DWord -Value 1 -Force | Out-Null
        $state.enabled=$true
        $state | ConvertTo-Json | Set-Content -LiteralPath $statePath
    }
    $env:TG_SILPOM_RENDERDOC_CONFIG=$ConfigPath
    $profileScript=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\Tests\TowerBenchmarkRenderDocProfile.py')).Path
    $process=Start-Process -FilePath $config.qrenderdoc -ArgumentList @('--python',('"'+$profileScript+'"')) -WorkingDirectory ([IO.Path]::GetDirectoryName([string]$config.qrenderdoc)) -WindowStyle Hidden -PassThru
    if(-not $process.WaitForExit($TimeoutSeconds*1000)){
        Stop-Process -Id $process.Id
        throw "RenderDoc exceeded the $TimeoutSeconds second limit"
    }
    $state.replay_exit=$process.ExitCode
    $profile=Get-Content -LiteralPath (Join-Path $output 'profile.json') -Raw | ConvertFrom-Json
    if(-not $profile.passed){throw 'Profiling did not pass; inspect profile.json'}
}catch{
    $failed=$true
    $state.error=$_.Exception.ToString()
}finally{
    if($process -and -not $process.HasExited){Stop-Process -Id $process.Id -ErrorAction SilentlyContinue}
    if($TemporaryDeveloperMode){
        if($valueExisted){
            New-ItemProperty -LiteralPath $registryPath -Name $valueName -PropertyType $oldKind -Value $oldValue -Force | Out-Null
        }elseif(Test-Path -LiteralPath $registryPath){
            Remove-ItemProperty -LiteralPath $registryPath -Name $valueName -ErrorAction SilentlyContinue
            $key=Get-Item -LiteralPath $registryPath
            if(-not $keyExisted -and $key.GetValueNames().Count -eq 0 -and $key.GetSubKeyNames().Count -eq 0){Remove-Item -LiteralPath $registryPath}
        }
    }
    $after=if(Test-Path -LiteralPath $registryPath){(Get-Item -LiteralPath $registryPath).GetValue($valueName,$null)}else{$null}
    $state.restored=if($valueExisted){$after -eq $oldValue}else{$null -eq $after}
    $state.completed_utc=[DateTime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath
}
if($failed -or -not $state.restored){exit 1}
