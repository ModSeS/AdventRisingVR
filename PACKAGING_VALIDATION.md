# Packaging validation — 2026-10-05

Runtime payload is unchanged: proxy 0.8.30-dev and bridge 0.8.21-dev. No gameplay fix or new headset validation is claimed by this packaging release.

Windows PowerShell 5.1 installer tests passed in a generated temporary fixture:
- Read-only preflight and Install -WhatIf.
- Tampered package checksum and unsupported game fingerprint rejection.
- Installation with pre-existing DLL backup and unrelated INI key preservation.
- Duplicate installation rejection.
- Changed-file uninstall refusal without mutation.
- Uninstall after game-binary update and removal of package payload.
- Byte-exact original DLL/INI restoration and removal of new files.
- Injected mid-copy error: rollback restores original files and leaves no installed manifest.

Launcher Check also passed against the installed game, without writing to it. Source and player packages include MIT and third-party license notices. Original game files, extracted scripts, private paths and diagnostic logs are excluded from the release archives.
