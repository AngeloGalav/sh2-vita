# The PS Vita build

The same game sources as the 3DS build (`port/source/`), compiled against a compatibility layer
that implements the citro3d and libctru calls the game makes on vitaGL and the Sce* libraries.
The game logic, the ported PC code and the renderer stay as they are; only `compat/` is Vita code.

## Build

```sh
port/vita/build.sh          # sh2vita.vpk, in the pinned vitasdk/vitasdk Docker image
port/vita/build.sh clean
port/vita/build.sh check    # syntax-check every game source against the layer
```

## Install

1. Install `sh2vita.vpk` with VitaShell.
2. Copy the pack to `ux0:data/sh23ds/`, so that `ux0:data/sh23ds/ui/font.t3x` exists. Make it with
   the packer and `-e VITA=1`, or convert a 3DS pack with
   `python3 tools/sh2conv/vitatex.py <pack>/3ds/sh23ds` (10 seconds; the 3DS cannot read it after).
   This writes the textures in the Vita's form, so they are not untiled or decoded as they load. A 3DS
   pack still works, at the cost of the conversion every time new textures come in. Saves, `options.txt` and `debug.txt` live there too.
3. The shaders are compiled on the console at run time: `ur0:data/libshacccg.suprx` must be
   installed (the usual step for vitaGL ports; extract it with ShaRKBR33D).

`ux0:data/sh23ds/vita_log.txt` records start-up and any shader that fails to compile.

## How the layer works

| File | Does |
| --- | --- |
| `compat/include/3ds.h`, `citro3d.h`, `tex3ds.h` | The libctru / citro3d / Tex3DS API the game uses, with citro3d's type layouts |
| `compat/ctru.c` | Time (the ARM11 tick from the Vita clock), threads and locks on pthread, memory, input, system language, the `sdmc:` paths (linker `--wrap`), the 3DS framebuffers for `nodata.c` |
| `compat/ndsp.c` | The DSP's 24 channels mixed in software into one 48 kHz SceAudioOut port |
| `compat/y2r.c` | No Y2R: `movie.c` takes its CPU conversion path |
| `compat/c3d_math.c` | citro3d's matrices, PICA depth range and tilted projections kept |
| `compat/c3d_tex.c` | Textures kept in PICA layout (the game reads and writes them), untiled on upload; ETC1 stays compressed, ETC1A4 and 4-bit formats are expanded; `.t3x` import. A converted pack's textures (format `0x80 \|` the PICA's, `tools/sh2conv/vitatex.py`) go up as they are: ETC1 in GL's block order, ETC1A4 as DXT5 with its levels, the raw formats untiled |
| `compat/c3d_shaders.c` | The six `.v.pica` programs translated to Cg, register for register |
| `compat/c3d_gl.c` | State, render targets (FBOs in the PICA's portrait orientation), draws, the fragment programs generated from the TEV, fragment lighting, alpha test and fog state, and presenting |

Input: the 3DS's A is the console's confirm button (Cross or Circle as the system is set), B the
other, X Triangle, Y Square, L / R, START / SELECT, the left stick the Circle Pad, the right stick
the C-Stick (read with `sceCtrlPeekBufferPositive`: the "2" reads report the shoulders as L1 / R1).
ZL / ZR are the rear touch pad's outer lower corners. The game's own default bindings on the Vita (`controls_default`, `SH2_VITA`): Cross acts,
Square runs, Triangle the flashlight, Circle the items, R aims, L turns round, the D-Pad as on the
3DS (look, next weapon, quick heal, map), the right stick changes target while aiming, the rear
corners strafe. Prompts draw the PlayStation shapes; the Buttons page names them.

## Status

Runs on a console: menus, rooms, characters, sound, a new game played through its first rooms.
Vita3K freezes at the first frame of any vitaGL program (even a one-shader test), so it can't be
used to check the picture. What is open, roughly in order:

1. **One screen.** The game screen fills the display (906 x 544, the 3DS's 5:3) and the 3DS touch
   page is laid over its right side, on a dark translucent ground, only while it has something to
   choose (`touchui_overlay`); the play page is not shown, as the PC has no HUD. Drawn on the game
   screen as the PC draws them: the pause menu (`pausemenu.c`, 0x407310 / 0x407620) and the title's
   menu, scenarios and levels (`titlemenu.c`, 0x496280, 0x4968c0, 0x496490, 0x497fc0; the Movie
   Menu in the same style until the PC's is found). Next, from their PC functions: items (the
   state machine is 0x471590; its drawing is still to find), options, save and load, map, memos.
2. Check on a console: textures (the PICA's rows run top-down, so every level is flipped on
   upload; ETC1 blocks are turned in place), the fog and lighting tables, render-to-texture.
3. Mipmaps: only level 0 is uploaded (the suspect for the coloured noise on some textures: vitaGL's
   upload of the levels above 0, not yet confirmed); the
   uncompressed formats get theirs from `glGenerateMipmap`, ETC1 goes without.
4. `C3D_SyncDisplayTransfer` and `C3D_SyncTextureCopy` do nothing yet: the game-over still
   (`gameover.c`) is black and the screenshot (`main.c`) is empty.
5. Resolution: 960 x 544, the display's own, at a locked 30 fps (`main.c`, `C3D_FrameEnd`). The
   2x2 target (always on, `supersample.c`) is made at that size with the game's viewports scaled
   (`C3D_RenderTargetCreate`, `C3D_SetViewport`), and `C3D_AspectRatioTop` is the display's shape,
   so the 3D is 16:9 (the scenes' 4:3 framing kept by the EE's ReduceCutsceneFOV, `demo.c`). The 2D,
   laid out in 400 x 240, is drawn across the whole width, 6.7% wider than on the 3DS.
   Textures at the PC's sizes: profile.c reads the 3DS's heaps, which the layer reports as the New
   3DS's 124 MB mode (`__wrap_main`); left at 0 they had every texture halved.
6. Memory (textures held twice), DXT from the PC's own textures in the packer (vitatex.py starts from the 3DS's ETC1A4), a shader cache.
7. No 3D (stereo.c's right eye is never shown), no surround (`ndspSetOutputMode` mono or stereo).

Shaders are compiled through vitashark directly and handed to vitaGL as programs
(`vglShaderGxpBinary`): vitaGL's own `glCompileShader` fails on every source with this SDK.
`vita_log.txt` logs every failed shader with the compiler's messages, and once a second the frames,
draws and draws skipped for want of a program.
