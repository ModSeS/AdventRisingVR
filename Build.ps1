$ErrorActionPreference='Stop'
Push-Location $PSScriptRoot
try {
 foreach($arch in @('Win32','x64')){
  $proxy=if($arch -eq 'Win32'){'ON'}else{'OFF'}
  $bridge=if($arch -eq 'x64'){'ON'}else{'OFF'}
  cmake -S . -B "build-$arch" -G 'Visual Studio 17 2022' -A $arch "-DARVR_BUILD_PROXY=$proxy" "-DARVR_BUILD_BRIDGE=$bridge"
  if($LASTEXITCODE){throw "Configure failed: $arch"}
  cmake --build "build-$arch" --config Release --parallel
  if($LASTEXITCODE){throw "Build failed: $arch"}
  ctest --test-dir "build-$arch" -C Release -E d3d9_capture --output-on-failure
  if($LASTEXITCODE){throw "Tests failed: $arch"}
 }
 New-Item -ItemType Directory -Force dist | Out-Null
 Copy-Item 'build-Win32/Release/dinput8.dll','build-x64/Release/AdventRisingVRBridge.exe','AdventRisingVR.ini' dist -Force
 Copy-Item 'build-Win32/_deps/d3d8to9-build/Release/d3d8.dll' dist -Force
 Get-ChildItem dist -File | ForEach-Object {@{File=$_.Name;Hash=(Get-FileHash -LiteralPath $_.FullName).Hash}} | ConvertTo-Json | Set-Content package-sha256.json -Encoding UTF8
} finally {Pop-Location}
