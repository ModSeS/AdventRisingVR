[CmdletBinding(SupportsShouldProcess)]
param(
 [ValidateSet('Install','Uninstall','Check')][string]$Action='Check',
 [string]$GameSystem='C:/Program Files (x86)/Steam/steamapps/common/Advent Rising/System'
)
$ErrorActionPreference='Stop'
$gameRoot=(Resolve-Path -LiteralPath $GameSystem).Path
$stateFile=Join-Path $gameRoot 'AdventRisingVR-install.json'
$packageFiles=@('d3d8.dll','dinput8.dll','AdventRisingVRBridge.exe','AdventRisingVR.ini')
$configFiles=@('default.ini','Mydefault.ini')
if($Action -eq 'Uninstall'){
 if(Get-Process -Name advent,AdventRisingVRBridge -ErrorAction SilentlyContinue){throw 'Close Advent Rising and its VR bridge first.'}
 if(!(Test-Path -LiteralPath $stateFile)){throw 'Installation manifest not found.'}
 $state=Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
 if(!$state.backup -or $state.backup -notmatch '^AdventRisingVR-backup-[a-zA-Z0-9-]+$'){throw 'Invalid backup name.'}
 if(@($state.files).Count -ne 6 -or @($state.files.name | Select-Object -Unique).Count -ne 6){throw 'Incomplete installation manifest.'}
 $backup=[IO.Path]::GetFullPath((Join-Path $gameRoot $state.backup))
 if(!$backup.StartsWith($gameRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid backup path.'}
 foreach($entry in $state.files){
  if($entry.name -notin ($packageFiles+$configFiles)){throw 'Unknown file in manifest.'}
  $target=Join-Path $gameRoot $entry.name
  if((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target).Hash -ne $entry.installedHash){throw "File changed since installation; manual review needed: $target"}
  if($entry.existed -and !(Test-Path -LiteralPath (Join-Path $backup $entry.name))){throw 'Backup is incomplete.'}
 }
 if($PSCmdlet.ShouldProcess($gameRoot,'Restore original configuration and remove installed mod files')){
  foreach($entry in $state.files){
   $target=Join-Path $gameRoot $entry.name
   if($entry.existed){Copy-Item -LiteralPath (Join-Path $backup $entry.name) -Destination $target -Force}
   elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target}
  }
  Remove-Item -LiteralPath $stateFile
  Write-Output "Uninstalled. Backup retained: $backup"
 }
 return
}
$binaryCheck=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'binary-check.json') -Raw | ConvertFrom-Json
foreach($name in @('Engine.dll','Core.dll','EonEngine.dll')){
 if((Get-FileHash -LiteralPath (Join-Path $gameRoot $name) -Algorithm SHA256).Hash -ne $binaryCheck.$name.sha256){throw "Unsupported game binary: $name"}
}
Write-Output 'Game binary fingerprints: MATCH'
$hashes=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-sha256.json') -Raw | ConvertFrom-Json
foreach($name in $packageFiles){
 $item=@($hashes | Where-Object File -eq $name)
 $file=Join-Path $PSScriptRoot "dist/$name"
 if($item.Count -ne 1 -or !(Test-Path -LiteralPath $file)){throw "Missing package file/hash: $name"}
 if((Get-FileHash -LiteralPath $file).Hash -ne $item[0].Hash){throw "Package checksum mismatch: $name"}
}
if(!(Test-Path -LiteralPath "$env:WINDIR/SysWOW64/d3dx9_43.dll")){throw 'DirectX June 2010 runtime (x86 d3dx9_43.dll) is required.'}
$runtime=(Get-ItemProperty 'HKLM:/SOFTWARE/Khronos/OpenXR/1' -ErrorAction SilentlyContinue).ActiveRuntime
Write-Output "OpenXR runtime: $runtime"
if(!$runtime -or !(Test-Path -LiteralPath $runtime)){Write-Warning 'OpenXR runtime is not configured. Select SteamVR as active OpenXR runtime before playing.'}
foreach($name in $configFiles){if(!(Test-Path -LiteralPath (Join-Path $gameRoot $name))){throw "Missing game configuration: $name. Run the original game once first."}}
if($Action -eq 'Check'){Write-Output 'Package preflight: PASS (headset and in-game behavior are not tested)';return}
if(Get-Process -Name advent,AdventRisingVRBridge -ErrorAction SilentlyContinue){throw 'Close Advent Rising and its VR bridge first.'}
if(Test-Path -LiteralPath $stateFile){throw 'This package is already installed; uninstall it before reinstalling.'}
function Set-IniValue([string]$Text,[string]$Section,[string]$Key,[string]$Value){
 $sectionPattern='(?ms)^\['+[regex]::Escape($Section)+'\][^\r\n]*\r?\n.*?(?=^\[|\z)'
 $match=[regex]::Match($Text,$sectionPattern)
 if(!$match.Success){return $Text+"`r`n[$Section]`r`n$Key=$Value`r`n"}
 $body=$match.Value
 $keyPattern='(?m)^'+[regex]::Escape($Key)+'\s*=.*$'
 if([regex]::IsMatch($body,$keyPattern)){$body=[regex]::Replace($body,$keyPattern,"$Key=$Value")}
 else{$body=$body.TrimEnd()+"`r`n$Key=$Value`r`n`r`n"}
 return $Text.Substring(0,$match.Index)+$body+$Text.Substring($match.Index+$match.Length)
}
# Prepare every configuration change before touching the game directory.
$configs=@{}
foreach($name in $configFiles){
 $text=Get-Content -LiteralPath (Join-Path $gameRoot $name) -Raw
 foreach($pair in @(@('FullscreenViewportX','2560'),@('FullscreenViewportY','1440'),@('WindowedViewportX','2560'),@('WindowedViewportY','1440'),@('StartupFullscreen','False'),@('HorizontalSensitivity','0.28'),@('VerticalSensitivity','0.28'))){
  $text=Set-IniValue $text 'WinDrv.WindowsClient' $pair[0] $pair[1]
 }
 $text=Set-IniValue $text 'D3DDrv.D3DRenderDevice' 'UseVSync' 'False'
 $configs[$name]=$text
}
if(!$PSCmdlet.ShouldProcess($gameRoot,'Install four VR files, back up and configure default.ini/Mydefault.ini for 2560x1440 windowed rendering')){return}
$backupName='AdventRisingVR-backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$backup=Join-Path $gameRoot $backupName
New-Item -ItemType Directory -Path $backup | Out-Null
$records=@()
foreach($name in ($packageFiles+$configFiles)){
 $target=Join-Path $gameRoot $name;$existed=Test-Path -LiteralPath $target
 if($existed){Copy-Item -LiteralPath $target -Destination (Join-Path $backup $name)}
 $records+=@{name=$name;existed=$existed;installedHash=''}
}
try{
 foreach($name in $packageFiles){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "dist/$name") -Destination (Join-Path $gameRoot $name) -Force}
 foreach($name in $configFiles){[IO.File]::WriteAllText((Join-Path $gameRoot $name),$configs[$name],[Text.Encoding]::Default)}
 foreach($entry in $records){$entry.installedHash=(Get-FileHash -LiteralPath (Join-Path $gameRoot $entry.name)).Hash}
 @{backup=$backupName;files=$records;version='0.8.30-dev'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $stateFile -Encoding UTF8
}catch{
 foreach($entry in $records){
  $target=Join-Path $gameRoot $entry.name
  if($entry.existed){Copy-Item -LiteralPath (Join-Path $backup $entry.name) -Destination $target -Force}
  elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target}
 }
 if(Test-Path -LiteralPath $stateFile){Remove-Item -LiteralPath $stateFile}
 throw
}
Write-Output "Installed 0.8.30-dev. Original files backed up in $backup"
