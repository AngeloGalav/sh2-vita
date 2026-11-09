<h1 align="center">sh2-vita: Silent Hill 2 for PlayStation Vita</h1>

<p align="center">
  A native PS Vita port of the PC Director's Cut, with menus that follow the original.<br>
  Bring your own copy of the game.
</p>

<p align="center">
  <img alt="Platform: PS Vita" src="https://img.shields.io/badge/platform-PS%20Vita-2f5da8?style=flat-square">
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-5b7083?style=flat-square"></a>
</p>

sh2-vita plays Silent Hill 2 from the PC version's own data, as native code on the Vita's CPU and GPU. It is based heavily on [sh23ds](https://github.com/ankushKun/sh23ds), the New 3DS port by ankushKun: the game logic, camera, lighting and events are that project's work, rebuilt from the PC executable. Here they run on a citro3d and libctru compatibility layer over [vitaGL](https://github.com/Rinnegatamante/vitaGL), with the Vita's own rendering, menus and controls on top.

**sh2-vita contains no game data.** You need your own copy of the PC game, which you convert on your computer.

> [!NOTE]
> This is a work in progress, tested on a console only in parts. A full playthrough on the Vita hasn't been done. See [what is open](#status).

## What it does

- Runs at the display's 960 x 544, at a steady 30 fps in most of the game
- Single-screen menus: title, pause, items, options, save and load, map, drawn after the original game's
- Textures in DXT straight from the PC's pixels, with their mipmaps
- The fog drawn on a smaller target, with a choice of detail in the options
- The Enhanced Edition's high resolution font, art and maps

## Install

You need:

- a PS Vita with custom firmware (HENkaku or Ensō), and enough room on `ux0:` for about 2.6 GB of game data
- `ur0:data/libshacccg.suprx`, the system's shader compiler, which the game compiles its shaders with
- the PC release of *Silent Hill 2: Director's Cut*, with the [Enhanced Edition](https://github.com/elishacloud/Silent-Hill-2-Enhancements) installed: it is required, and the executable has to be the Director's Cut, whose tables the converters read
- on your computer: Docker (or, without it, Python 3 with numpy, Pillow and pefile, ffmpeg and a C++ compiler)

1. Install `sh2vita.vpk` from the [latest release](https://github.com/AngeloGalav/sh2-vita/releases) with VitaShell, or build it yourself with `./port/vita/build.sh`, which writes `port/vita/sh2vita.vpk`.
2. Convert your game with the packer image, which you build from this repository (it holds only the converters, never game data):
   ```sh
   git clone https://github.com/AngeloGalav/sh2-vita && cd sh2-vita
   docker build -f tools/packer/Dockerfile -t sh2-vita-packer .
   mkdir -p out
   docker run --rm --user "$(id -u):$(id -g)" -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/out:/out" sh2-vita-packer
   ```
   The game folder is the one with `sh2pc.exe`, `data/` and the Enhanced Edition's `sh2e/`. On Windows leave out `--user ...` and write the paths as `"C:\Games\Silent Hill 2:/game:ro"` and `"${PWD}\out:/out"`. It takes an hour or more; if it stops, the same command picks up where it stopped (`-e FRESH=1` starts over).
3. Copy the contents of `out/sh2-vita/` to `ux0:data/sh2-vita/` on the Vita (about 2.6 GB).

Without Docker, the same steps by hand:

```sh
python3 -m venv .venv && .venv/bin/pip install numpy pillow pefile
tools/texenc/build.sh native
SH2_VITA_TEX=1 SH2_TEXENC=native tools/make_pack.sh "/path/to/Silent Hill 2" out/sh2-vita --strip-extras
.venv/bin/python tools/sh2conv/vitatex.py out/sh2-vita
```

The first time each shader is needed it is compiled and kept on the card, so the first run has some short pauses.

## Controls

| | Button |
| --- | --- |
| Move James | Left stick |
| Search camera, back when let go | Right stick |
| Check, open, pick up (attack while aiming) | Cross |
| Run (hold) | Square |
| Aim (hold) | R |
| Quick turn | L |
| Map | Triangle or D-Pad right |
| Flashlight | Circle |
| Items | START |
| Pause | SELECT |
| Search view | D-Pad up |
| Next weapon | D-Pad down |
| Quick heal | D-Pad left |
| Strafe left / right | Rear touch pad's lower corners |

In menus, Cross confirms and Circle goes back, whatever the console's own setting. Actions can be rebound in *Options*, *Controls*, *Buttons*. More in [CONTROLS.md](port/CONTROLS.md).

## Options for this port

Under *Options*, *System*:

| Option | Choices |
| --- | --- |
| Resolution | 960 x 544, or 720 x 416 for a steadier frame rate |
| FPS Cap | 30 or 60 |
| Texture Filter | Bilinear or trilinear |
| Fog | Off or on |
| Fog Detail | The fog's target at a half, a third or a quarter of the view each way |
| Filters | The blur some scenes use, off or on |

## Status

Open at the moment:

- **Item screen:** close to the original, but the layout is about 13% wider than 4:3 and Adjust volume shows a number in place of the blocks
- **Options:** the look follows the original's screens, the tabs and some settings are the port's own
- **Other menus:** the memo list, stage panels and first run use the same style without being ported from the game's own screens, and the Movie Menu takes the options' look
- **Title:** the menu's words are the game's pictures, which exist only in English
- **Sound:** crackling can be heard
- **Not wired:** subtitles on the movies and New Game+'s yellow NEW GAME

More in [port/vita/README.md](port/vita/README.md) and [port/STATUS.md](port/STATUS.md).

## Building and working on it

The toolchain runs in Docker. `./port/vita/build.sh` fetches vitaGL at a pinned commit and builds the package.

The game is ported by reading the PC executable in Ghidra and translating its functions to C, with the original address in a comment. Start with [AGENTS.md](AGENTS.md): it says where everything is and how the original game is used.

| Doc | What's in it |
| --- | --- |
| [port/vita/README.md](port/vita/README.md) | How the Vita layer works |
| [port/STATUS.md](port/STATUS.md) | What works and what differs from the PC |
| [port/PERFORMANCE.md](port/PERFORMANCE.md) | Frame budget and streaming, from the 3DS work |
| [docs/RESEARCH.md](docs/RESEARCH.md) | How the executable was mapped, and key addresses |

To test on a console, these combinations work while holding SELECT: L changes the fog's mode, R the GPU cost test, Triangle the fog's precision and Square the ETC1 levels. `vita_log.txt` and `perf.txt` are written next to the game data.

## Thanks

- [Silent Hill 2: Enhanced Edition](https://github.com/elishacloud/Silent-Hill-2-Enhancements) by Elisha Riedlinger and contributors, for its fixes, its research into the PC game and its high resolution packs
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) and vitaShaRK by Rinnegatamante, and the [VitaSDK](https://vitasdk.org/) team
- [sh23ds](https://github.com/ankushKun/sh23ds) by ankushKun
- The Silent Hill Database for its file format notes, and the PS2 beta's debug information, which names much of the PC code
- [pl_mpeg](https://github.com/phoboslab/pl_mpeg) by Dominic Szablewski, which plays the movies
- [Ghidra](https://ghidra-sre.org/)

## License

sh2-vita is released under the [GNU General Public License v3.0](LICENSE). Bundled third-party code keeps its own license.

This is a fan project, not affiliated with or endorsed by Konami or Sony. *Silent Hill* is a trademark of Konami Digital Entertainment and PlayStation Vita is a trademark of Sony Interactive Entertainment.

## AI disclosure

Much of this port's code, documentation and commit messages was written with AI coding agents, including Claude Code. A person directs the work, decides what goes in and tests the builds on a real console. Expect mistakes, and check the code against the original game before relying on it.
