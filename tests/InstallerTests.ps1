$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('ARVR installer test '+[guid]::NewGuid().ToString('N'))
$pkg=Join-Path $fixture 'package';$game=Join-Path $fixture 'game/System'
New-Item -ItemType Directory -Path $pkg,$game -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'Manage-Mod.ps1') -Destination $pkg
New-Item -ItemType Directory -Path (Join-Path $pkg 'dist') | Out-Null
$fingerprints=@{}
foreach($name in @('Engine.dll','Core.dll','EonEngine.dll')){
 "fixture $name" | Set-Content -LiteralPath (Join-Path $game $name)
 $fingerprints[$name]=@{sha256=(Get-FileHash -LiteralPath (Join-Path $game $name)).Hash}
}
$fingerprints | ConvertTo-Json | Set-Content (Join-Path $pkg 'binary-check.json')
foreach($name in @('d3d8.dll','dinput8.dll','AdventRisingVRBridge.exe','AdventRisingVR.ini')){"fixture mod $name" | Set-Content -LiteralPath (Join-Path $pkg "dist/$name")}
Get-ChildItem (Join-Path $pkg 'dist') -File | ForEach-Object {@{File=$_.Name;Hash=(Get-FileHash -LiteralPath $_.FullName).Hash}} | ConvertTo-Json | Set-Content (Join-Path $pkg 'package-sha256.json')
$ini="[WinDrv.WindowsClient]`r`nFullscreenViewportX=800`r`nCustomValue=keep`r`n[D3DDrv.D3DRenderDevice]`r`nUseVSync=True`r`n"
foreach($name in @('default.ini','Mydefault.ini')){[IO.File]::WriteAllText((Join-Path $game $name),$ini)}
'previous user DLL' | Set-Content (Join-Path $game 'dinput8.dll')
$oldHash=(Get-FileHash (Join-Path $game 'dinput8.dll')).Hash
$iniHash=(Get-FileHash (Join-Path $game 'default.ini')).Hash
$script=Join-Path $pkg 'Manage-Mod.ps1'
function Assert($condition,$message){if(!$condition){throw "FAIL: $message"}}
function Reject([scriptblock]$operation,[string]$message){$failed=$false;try{& $operation | Out-Null}catch{$failed=$_.Exception.Message -like "*$message*"};Assert $failed "expected rejection: $message"}
& $script -Action Check -GameSystem $game
& $script -Action Install -GameSystem $game -WhatIf
Assert (!(Test-Path (Join-Path $game 'AdventRisingVR-install.json'))) 'WhatIf wrote manifest'
Add-Content (Join-Path $pkg 'dist/dinput8.dll') 'corrupt'
Reject {& $script -Action Install -GameSystem $game} 'checksum mismatch'
'fixture mod dinput8.dll' | Set-Content (Join-Path $pkg 'dist/dinput8.dll')
'bad game' | Set-Content (Join-Path $game 'Engine.dll')
Reject {& $script -Action Install -GameSystem $game} 'Unsupported game binary'
'fixture Engine.dll' | Set-Content (Join-Path $game 'Engine.dll')
& $script -Action Install -GameSystem $game
Assert ((Get-Content (Join-Path $game 'default.ini') -Raw) -match 'CustomValue=keep') 'custom INI value lost'
Assert ((Get-Content (Join-Path $game 'default.ini') -Raw) -match 'FullscreenViewportX=2560') 'INI patch missing'
Reject {& $script -Action Install -GameSystem $game} 'already installed'
Add-Content (Join-Path $game 'AdventRisingVR.ini') 'user changes'
Reject {& $script -Action Uninstall -GameSystem $game} 'File changed'
Assert (Test-Path (Join-Path $game 'AdventRisingVR-install.json')) 'manifest deleted after refusal'
Copy-Item (Join-Path $pkg 'dist/AdventRisingVR.ini') (Join-Path $game 'AdventRisingVR.ini') -Force
# Removal must not depend on a matching game build or available package payload.
'updated game' | Set-Content (Join-Path $game 'Engine.dll')
Rename-Item -LiteralPath (Join-Path $pkg 'dist') -NewName 'payload-unavailable'
& $script -Action Uninstall -GameSystem $game
Assert ((Get-FileHash (Join-Path $game 'dinput8.dll')).Hash -eq $oldHash) 'original DLL not restored'
Assert ((Get-FileHash (Join-Path $game 'default.ini')).Hash -eq $iniHash) 'original INI not restored'
Assert (!(Test-Path (Join-Path $game 'd3d8.dll'))) 'new DLL not removed'
Assert (!(Test-Path (Join-Path $game 'AdventRisingVR-install.json'))) 'manifest not removed'
# Inject one copy failure to verify rollback after partial installation.
Rename-Item -LiteralPath (Join-Path $pkg 'payload-unavailable') -NewName 'dist'
'fixture Engine.dll' | Set-Content (Join-Path $game 'Engine.dll')
$fault=[pscustomobject]@{Injected=$false}
function Copy-Item {
 [CmdletBinding()]param([string]$LiteralPath,[string]$Destination,[switch]$Force)
 if(!$fault.Injected -and $LiteralPath -like '*dist*AdventRisingVRBridge.exe'){$fault.Injected=$true;throw 'injected copy failure'}
 Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
}
Reject {& $script -Action Install -GameSystem $game} 'injected copy failure'
Assert $fault.Injected 'fault injection not exercised'
Assert ((Get-FileHash (Join-Path $game 'dinput8.dll')).Hash -eq $oldHash) 'rollback did not restore original DLL'
Assert ((Get-FileHash (Join-Path $game 'default.ini')).Hash -eq $iniHash) 'rollback changed original INI'
Assert (!(Test-Path (Join-Path $game 'd3d8.dll'))) 'rollback left new DLL'
Assert (!(Test-Path (Join-Path $game 'AdventRisingVR-install.json'))) 'rollback left manifest'
Write-Output "PASS: preflight, dry run, checksums, binary mismatch, install, existing DLL backup, INI preservation, duplicate refusal, changed-file refusal, uninstall without payload, byte-exact restore and injected-error rollback. Fixture: $fixture"
