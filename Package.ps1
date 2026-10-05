[CmdletBinding()]
param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'releases'))
$ErrorActionPreference='Stop'
$version=(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'VERSION') -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+(-[a-zA-Z0-9.-]+)?$'){throw 'Invalid version'}
$expected=@('d3d8.dll','dinput8.dll','AdventRisingVRBridge.exe','AdventRisingVR.ini')
$hashes=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'package-sha256.json') -Raw | ConvertFrom-Json
foreach($name in $expected){
 $h=@($hashes | Where-Object File -eq $name)
 if($h.Count -ne 1 -or (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot "dist/$name")).Hash -ne $h[0].Hash){throw "Checksum mismatch: $name"}
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stage=Join-Path ([IO.Path]::GetTempPath()) ('ARVR-package-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$files=@('Install.cmd','Uninstall.cmd','Check.cmd','Launcher.ps1','Manage-Mod.ps1','binary-check.json','package-sha256.json','VERSION','LICENSE','README.md','README_RU.md','CONTROLS.md','TROUBLESHOOTING.md','RELEASE_NOTES.md','THIRD_PARTY_NOTICES.md','BUILDING.md','PUBLISH_RU.md')
foreach($name in $files){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $stage -Recurse
New-Item -ItemType Directory -Path (Join-Path $stage 'dist') | Out-Null
foreach($name in $expected){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "dist/$name") -Destination (Join-Path $stage 'dist')}
$zip=Join-Path $OutputDirectory "AdventRisingVR-$version-Windows.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
$hash=(Get-FileHash -LiteralPath $zip).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding ASCII
Write-Output "Player archive: $zip"
Write-Output "Packaging staging files retained at: $stage"
