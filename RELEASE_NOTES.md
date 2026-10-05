# 0.8.30-dev — community pre-release

Proxy: 0.8.30-dev. OpenXR bridge: 0.8.21-dev (intentionally unchanged).

Includes head/stereo tracking, controller hands, room-scale body follow, stick locomotion/turning, trigger-gated melee gestures, weapon buttons, compact VR HUD and target marker. Latest fix makes the selected VR target visible to the shooting-range damage script without forcing camera rotation.

## Validation

The development x86 build passed 15 automated tests, including the target-query regression with mocked game calls. This is not proof of in-headset behavior. The latest focus/damage fix has not yet been confirmed by the player. Hardware D3D9 capture testing is excluded in the development sandbox. No full campaign or broad hardware compatibility claim is made.

## Distribution packaging

Adds Windows double-click launchers, Steam library discovery, package checksum verification, concise bilingual documentation, clean source layout and third-party notices. Runtime DLL/EXE files are unchanged from the previously built versions. Installer lifecycle is tested in an isolated fixture, not against the user's game.
