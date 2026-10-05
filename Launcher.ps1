param([ValidateSet('Install','Uninstall','Check')][string]$Action='Install',[string]$GamePath)
$ErrorActionPreference='Stop'
try {
 if(!$GamePath){
  $roots=@("${env:ProgramFiles(x86)}/Steam", "$env:ProgramFiles/Steam")
  $steam=(Get-ItemProperty 'HKCU:/Software/Valve/Steam' -ErrorAction SilentlyContinue).SteamPath
  if($steam){$roots+=$steam}
  foreach($root in @($roots)){
   $vdf=Join-Path $root 'steamapps/libraryfolders.vdf'
   if(Test-Path -LiteralPath $vdf){
    foreach($match in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw),'"path"\s+"([^"]+)"')){$roots+=$match.Groups[1].Value.Replace('\\','\')}
   }
  }
  $found=@($roots | ForEach-Object {Join-Path $_ 'steamapps/common/Advent Rising/System'} | Where-Object {Test-Path -LiteralPath (Join-Path $_ 'advent.exe')} | Select-Object -Unique)
  if($found.Count -eq 1){$GamePath=$found[0]}
  elseif($found.Count -gt 1){
   for($i=0;$i -lt $found.Count;$i++){Write-Host "[$($i+1)] $($found[$i])"}
   $choice=Read-Host 'Choose game number (or leave blank to enter a path)'
   $num=0;if([int]::TryParse($choice,[ref]$num) -and $num -ge 1 -and $num -le $found.Count){$GamePath=$found[$num-1]}
  }
  if(!$GamePath){$GamePath=(Read-Host 'Enter Advent Rising folder or its System folder').Trim().Trim('"')}
 }
 if(Test-Path -LiteralPath (Join-Path $GamePath 'System/advent.exe')){$GamePath=Join-Path $GamePath 'System'}
 if(!(Test-Path -LiteralPath (Join-Path $GamePath 'advent.exe'))){throw 'advent.exe not found. Select the game System folder.'}
 Write-Host "Game: $GamePath"
 & (Join-Path $PSScriptRoot 'Manage-Mod.ps1') -Action $Action -GameSystem $GamePath
 exit 0
} catch {
 Write-Host $_.Exception.Message -ForegroundColor Red
 Write-Host 'If access is denied: close the game and right-click the .cmd file > Run as administrator.'
 exit 1
}
