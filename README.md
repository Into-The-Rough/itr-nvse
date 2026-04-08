# itr-nvse

`itr-nvse` is an xNVSE plugin for Fallout: New Vegas that bundles engine
fixes, gameplay tweaks, small UI and quality-of-life features, and a scripting
API for modders.

No ESP is required.

## Highlights

- Engine fixes such as `SlowMotionPhysicsFix`, `VATSProjectileFix`,
  `VATSLimbFix`, `ArmorDTDRFix`, and crash fixes
- Gameplay tweaks such as `OwnedBeds`, `OwnedCorpses`, `FriendlyFire`,
  `NoDoorFade`, and `AshPileNames`
- Features such as `QuickReadNote`, `LocationVisitPopup`, `VATSExtender`,
  `OwnerNameInfo`, `SaveFileSize`, `QuickDrop`, and `Quick180`
- Script commands and event handlers for dialogue, radio, combat, input, and
  utility workflows

## Requirements

- Fallout: New Vegas
- xNVSE

## Installation

Install the release files into the normal Fallout New Vegas data paths:

- `Data/NVSE/Plugins/`
- `Data/config/itr-nvse.ini`
- `Data/MCM/` for the included MCM assets

## Configuration

Most end-user settings live in `Data/config/itr-nvse.ini`.

Reload config at runtime with:

```text
ReloadPluginConfig itr-nvse
```


## Documentation

- [FEATURES.md](FEATURES.md): features, commands, and event handlers
- [THIRD_PARTY.md](THIRD_PARTY.md): bundled third-party code notes
- [LICENSE](LICENSE): project license

## Building

Open [itr-nvse.sln](itr-nvse.sln) in Visual Studio 2022 and build `Win32`
`Release` or `Debug`.

The project file contains local post-build copy steps. If you are building in a
different environment, update or remove those paths first.

## License

`itr-nvse` is licensed under the MIT license. See [LICENSE](LICENSE).
