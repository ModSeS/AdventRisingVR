# Building

Use the complete repository/source archive. The Windows player ZIP does not contain source files or build tools.

Windows x64, Visual Studio 2022 with Desktop development with C++, Windows SDK, CMake 3.24+, Git and network access for FetchContent dependencies.

```powershell
./Build.ps1
```

Builds the proxy and d3d8to9 as Win32, OpenXR bridge as x64, runs automatic tests, then populates dist and refreshes package-sha256.json. Dependencies are pinned in CMakeLists.txt. The optional hardware d3d9_capture test is excluded by default; run it separately on a suitable Windows desktop if desired. No game files are required to compile.

```powershell
./Package.ps1
```

Creates the player ZIP and SHA256SUMS.txt in releases. Run Build.ps1 first after changing runtime code. Installer-only tests: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/InstallerTests.ps1`.
