# Advent Rising VR

Experimental PC VR mod for the 32-bit Steam version of Advent Rising. Developed with Meta Quest 3S through Steam Link and SteamVR/OpenXR.

**Pre-release 0.8.30-dev.** This is a community testing build, not a fully validated release. The latest shooting-range focus fix still needs in-game confirmation. [Русская инструкция](README_RU.md).

## Install (players)

1. Download **AdventRisingVR-0.8.30-dev-Windows.zip** from this repository's Releases section. Extract the entire ZIP before running anything.
2. Install and run the original game once. Close it and any VR bridge. Windows 10/11 x64, DirectX June 2010 x86 runtime (`d3dx9_43.dll`), and Microsoft Visual C++ 2015–2022 runtimes **x86 and x64** are required. Use Microsoft's installers for these prerequisites.
3. Connect the headset with Steam Link; start SteamVR and select SteamVR as the active OpenXR runtime.
4. Double-click **Check.cmd**, then **Install.cmd**. The launcher searches Steam libraries; enter the game folder if it cannot find it. If access is denied, run Install.cmd as administrator.
5. Launch Advent Rising normally and load a level. The VR bridge starts automatically. F8 recenters your view.

The installer accepts only the Engine/Core/EonEngine DLL hashes in `binary-check.json`. Other game builds, executable mods and translation packs replacing these DLLs are not supported. Do not bypass this check.

## Remove or update

Close the game and bridge. Run **Uninstall.cmd** from the old package before installing a new version. Original files are restored from `System/AdventRisingVR-backup-*`. Keep that backup and `AdventRisingVR-install.json`.

If uninstall reports changed files, it stops rather than overwriting your edits. Back up those files; restore their installed versions from the matching package where applicable, or compare with the backup and restore manually. Do not delete the backup or manifest until restoration is complete.

## Features and controls

Head tracking and stereo view, controller-driven hands, room-scale body follow, left-stick movement, right-stick turning, compact health/ammo HUD, controller menu input and target focus. See [controls](CONTROLS.md) and [troubleshooting](TROUBLESHOOTING.md).

The installer backs up four mod files and two game configuration files, then installs the mod and sets 2560×1440 windowed rendering with VSync disabled. It does not edit saves. Performance varies; rendering uses two game draws plus CPU readback. Other headsets/runtimes and a full campaign playthrough have not been validated. Grenade gestures invoke the game's normal throw, not a physically simulated controller trajectory.

## Developers and publishing

[Build instructions](BUILDING.md) · [Release notes](RELEASE_NOTES.md) · [Publishing guide](PUBLISH_RU.md) · [Third-party notices](THIRD_PARTY_NOTICES.md).

Game binaries, assets and extracted game scripts are not included. This is an unofficial mod, not affiliated with the game's publisher or Meta/Valve.

## License

Project code: [MIT](LICENSE). Third-party components retain their own licenses.
