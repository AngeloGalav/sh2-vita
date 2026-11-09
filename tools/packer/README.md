# The packer

The packer reads your `Silent Hill 2` folder (the one with `sh2pc.exe`, `data/` and the Enhanced
Edition's `sh2e/`) and converts every room, texture, character, animation, event, message, picture, map,
sound and movie into the formats the port reads, with the textures in the PS Vita's form. It writes a
`sh2-vita` folder, whose contents go to `ux0:data/sh2-vita/`, and never changes the game folder.

## With Docker

The image holds the converters from this repository and nothing from the game. Build it from a clone:

```sh
docker build -f tools/packer/Dockerfile -t sh2-vita-packer .
mkdir -p out
docker run --rm --user "$(id -u):$(id -g)" -v "/path/to/Silent Hill 2:/game:ro" -v "$PWD/out:/out" sh2-vita-packer
```

```powershell
# Windows (PowerShell)
docker build -f tools/packer/Dockerfile -t sh2-vita-packer .
mkdir out
docker run --rm -v "C:\Games\Silent Hill 2:/game:ro" -v "${PWD}\out:/out" sh2-vita-packer
```

When it finishes, `out/sh2-vita/` holds about 2.6 GB.

- **Linux:** `--user "$(id -u):$(id -g)"` makes the files yours.
- **macOS with Colima:** only your home folder is shared with Docker, so keep the game folder and the
  output folder inside it.
- **If it stops partway,** run the same command again. Finished steps are skipped; `-e FRESH=1` starts
  over.

## Without Docker

Python 3.11 or newer with `numpy`, `Pillow` and `pefile`, ffmpeg with the Bink decoder (most Linux
builds and Homebrew's have it) and a C++ compiler:

```sh
python3 -m venv .venv && .venv/bin/pip install numpy pillow pefile
tools/texenc/build.sh native
SH2_VITA_TEX=1 SH2_TEXENC=native tools/make_pack.sh "/path/to/Silent Hill 2" out/sh2-vita --strip-extras
.venv/bin/python tools/sh2conv/vitatex.py out/sh2-vita
```

`make_pack.sh` uses `.venv` when it is there; the system's `python3` often has no numpy.
