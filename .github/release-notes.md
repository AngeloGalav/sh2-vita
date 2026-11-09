A native PS Vita port of Silent Hill 2 (PC Director's Cut). This file contains no game data: you convert that from your own copy of the PC game, with the Enhanced Edition installed.

**Install**

1. Install `sh2vita.vpk` with VitaShell.
2. Make sure `ur0:data/libshacccg.suprx` exists (the shader compiler, extracted with ShaRKBR33D).
3. Convert your game with the packer image as the [README](https://github.com/AngeloGalav/sh2-vita#install) says (Docker, from your own copy of the game with the Enhanced Edition), and copy the contents of `out/sh2-vita/` to `ux0:data/sh2-vita/` (about 2.6 GB).

The first time each shader is needed it is compiled and kept on the card, so the first run has some short pauses. The checksum is in `sh2vita.sha256`.
