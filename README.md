# Pluckomp

**Pluckomp** is a work-in-progress static recompilation of *Pikmin 4* (Nintendo Switch, 2023) into a native
x86-64 application for Windows and Linux, with a Vulkan renderer ([docs/architecture.md](docs/architecture.md)).

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of *Pikmin 4*, dumped from your own
> console. Instructions for obtaining those files will not be provided.
>
> The game is built from your files on your machine, so the result contains the game. It is for your own use. Do not
> share it.

## Status

Pre-alpha: the repository layout, content guard and build plan are in place; the recompiler and runtime are not
working yet. [docs/roadmap.md](docs/roadmap.md) lists what comes next.

Report problems as issues, following [CONTRIBUTING.md](CONTRIBUTING.md).

## Planned features

- Native code, not emulation
- NSP and XCI input (plus compressed NSZ / XCZ)
- Unlocked frame rate, custom resolutions and upscaling
- Keyboard, mouse and gamepad with remapping, rumble and gyro
- In-game settings menu and performance overlay
- Mods in the formats emulator mods use (RomFS replacements, ExeFS patches)

## Installing

There is no release yet. When there is: download the setup from [Releases](../../releases/latest), point it at your
own game files, and it builds the game once on your machine.

## Building from source

[docs/building.md](docs/building.md) lists the toolchain and the steps. Everything the build writes stays under
`build/` and `generated/`, which git ignores; run `python tools/check_content.py` before a commit.

## Credits

The project layout and content-guard approach follow [TotK Recomp](https://github.com/valentderah/totk-pc-recomp)
(MIT). What was taken from each project, with its license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp)
- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [hactool](https://github.com/SciresM/hactool)
- [Switchbrew](https://switchbrew.org) and the Switch homebrew community

## License

The project's own code is under the [MIT License](LICENSE); third-party code keeps its own license
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). The license grants nothing for Nintendo's game.

Pluckomp is an independent, non-commercial fan project, not affiliated with or endorsed by Nintendo.
*Pikmin* and *Nintendo Switch* are trademarks of Nintendo.
