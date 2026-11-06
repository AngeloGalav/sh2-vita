#!/usr/bin/env python3
"""Convert the user's title art into the 3DS pack.

  ui.py <game data dir> <pack dir>

Writes pack/ui/title.t3x: the Director's Cut logo (data/pic/etc/sh2dc.tex, 512x512 DXT3) cropped to
a 512x128 strip around the lettering, RGB565 without mipmaps.

Also writes the save screen's backgrounds (data/menu/mc/savebg.tbn2, James, and savebg2.tbn2,
Maria; 512x512 DXT3 squeezed to 512x256 RGB8: pack/ui/savebg.t3x, savebg2.t3x)
and its texts (data/etc/message/m_card_x_msg_e.mes as pack/events/mcard.msg), and the options
screens' texts (option_msg_e.mes as pack/events/option.msg) and the blood effects' texture
(pack/effect/blood.t3x, see write_blood), the hyper spray's mist (pack/effect/mist.t3x, see
write_mist), the room effects' sprites (pack/effect/fire.t3x, smoke.t3x, flare.t3x, see
write_room_fx), the lens flare's (pack/effect/lensflare.t3x, see write_lens_flare), and the
title background, the game over text and its sound (see write_screens).

Also writes the PC's text font (pack/ui/font.t3x and font.bin, see write_font), which the exe carries
compressed; pass --exe to read another sh2pc.exe than the default one.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ee  # noqa: E402
import sh2map  # noqa: E402
from convert import encode_textures  # noqa: E402


def load_tex(path):
    """A single-sprite .tex or .tbn2 file, the Enhanced Edition's version when the player has it
    (ee.py; its images are the same files at up to 4x the size). See decode_tex."""
    return ee.load(path, decode_tex)


def decode_tex(path):
    """Single-sprite .tex / .tbn2 file: 0x30-byte header, then the sprite header (u16 width and
    height at 0x38, u32 format at 0x3c, u32 length at 0x40) and the pixels at 0x50: DXT1..5
    (0x100..0x104) or 32-bit BGRA (0x20: menu/mc/mu_alpha.tbn2, the Image Enhancement Pack's
    start01*.tex)."""
    d = open(path, 'rb').read()
    if struct.unpack_from('<I', d, 0)[0] != 0x19990901:
        raise ValueError(f'{path}: not a .tex file')
    _, _, _, w, h, fmt, length, _, _, _ = struct.unpack_from('<IHHHHIIIII', d, 0x30)
    if fmt == 0x20:
        bgra = np.frombuffer(d, np.uint8, w * h * 4, 0x50).reshape(h, w, 4)
        return Image.fromarray(np.ascontiguousarray(bgra[:, :, [2, 1, 0, 3]]), 'RGBA')
    kind = sh2map.MAP_TEX_KIND[fmt]
    # The Image Enhancement Pack's larger files keep the original's length in the header (262144 at
    # 2732x2048): what the size asks for, when the file holds it
    need = (w + 3) // 4 * ((h + 3) // 4) * (8 if kind == 1 else 16)
    if length < need <= len(d) - 0x50:
        length = need
    return Image.fromarray(sh2map.decode_dxt(d[0x50:0x50 + length], w, h, kind))


# The PC's two text fonts (0x480ea0 draws a glyph into its glyph cache): font 0 is 20x30, font 1
# 16x24 (the u16 pairs at 0x806480), the font data pointers are at 0x806488 and 0x806490 holds
# the eight alpha levels of the 3-bit pixel codes. Each font's data starts with u16 bounds (ended
# by 0) where the glyph offsets wrap past 64K dwords, the widths of codes below 0xe0 at +0x10 and
# the u16 dword offsets of every code's bitstream at +0xf0. Codes below 0xe0 are the text
# characters (Windows-1252 minus 0x20) and are as wide as their width byte; the codes from 0xe0
# on are the full-width set (Japanese, symbols, the 0x138 star and the 0x172.. Latin letters; 0xe7
# and 0x11b are in the save screen's and the German Game Options' titles).
FONT_SIZES = 0x806480
FONT_DATA = 0x806488
FONT_LEVELS = 0x806490
FONT_CODES = list(range(0xe0)) + [0xe7, 0x11b, 0x138] + list(range(0x172, 0x1b0))


def font_glyph(exe, f, code):
    """Decodes one glyph: 3-bit codes read LSB first from dwords (0x480e00); 7 starts a run of
    transparent pixels whose length follows in up to five more codes."""
    def u16(a):
        return struct.unpack('<H', exe.read(a, 2))[0]
    fw, fh = u16(FONT_SIZES + 4 * f), u16(FONT_SIZES + 4 * f + 2)
    fa = exe.u32(FONT_DATA + 4 * f)
    levels = exe.read(FONT_LEVELS, 8)
    bounds = []
    while u16(fa + 2 * len(bounds)):
        bounds.append(u16(fa + 2 * len(bounds)))
    w = exe.read(fa + 0x10 + code, 1)[0] if code < 0xe0 else fw
    off = u16(fa + 0xf0 + 2 * code)
    px = np.zeros((fh, max(w, 1)), np.uint8)
    if not off or not w:
        return w, px
    p = fa + (sum(1 for b in bounds if b < code) * 0x10000 + off) * 4
    bits = nb = run = 0

    def sym():
        nonlocal bits, nb, p
        if nb < 3:
            bits |= exe.u32(p) << nb
            p += 4
            nb += 32
        s = bits & 7
        bits >>= 3
        nb -= 3
        return s
    for y in range(fh):
        for x in range(w):
            if run:
                run -= 1
                continue
            s = sym()
            if s == 7:
                run = sym()
                if run == 0:
                    run = sym() + 7
                    if run == 7:
                        run = sym() + 14
                        if run == 14:
                            run = sym() + 0x15 + sym() * 8
                            if run == 0x15:
                                run = sym() + 0x54 + sym() * 8 + sym() * 64
                continue
            px[y, x] = levels[s]
    return w, px


# The 3DS draws the PC's 512x448 text space as 320x210 pixels (see port/source/font.h), so the
# glyphs are stored at that size and drawn one texel per pixel.
FONT_SX, FONT_SY = 0.625, 0.46875
FONT_ATLAS = 512
VITA = bool(os.environ.get('SH2_VITA_TEX'))
if VITA:
    # The Vita's 960x544 draws the same text space 2.4 and 2.27 times larger: the glyphs are stored
    # at that size and drawn one texel per pixel (font.c GLYPH_SX, GLYPH_SY)
    FONT_SX, FONT_SY = 1.5, 1.0625
    FONT_ATLAS = 1024


def vita_size(img, most=512):
    """A picture's size on the Vita: up to 1024 wide and most tall on powers of two, not the 3DS's
    512x256; 4:3 pictures are squeezed only as the texture's shape needs, the draw undoes it."""
    w, h = img.size
    return min(1024, 1 << (w.bit_length() - 1)), min(most, 1 << (h.bit_length() - 1))


def ee_font(data):
    """The Enhanced Edition's font sheet as alpha, when the game has it (sh2e/font/font000.png,
    read as its Patches/Fonts.cpp does: 100 columns of 44x64 cells by code, the second font in the
    lower half; each cell holds its font at twice the PC's size). None without it."""
    root = ee.mod_root(os.path.dirname(os.path.abspath(data)))
    p = os.path.join(root, 'font', 'font000.png') if root else None
    if not p or not ee.enabled() or not os.path.exists(p):
        return None
    return np.asarray(Image.open(p).convert('RGBA'))[:, :, 3]


FONT_CELL = 44, 64, 100        # the sheet's cell and columns
# Where a cell's glyph box starts, at twice the PC's pixels: one PC pixel left of where its ink lines
# up with the PC's glyph (measured over the Latin letters), since the sheet's ink is a little wider.
# font.c leaves out the PC's one pixel in for it (GLYPH_PEN).
FONT_INSET = 3, 2


def sheet_glyph(sheet, f, code, w, fh):
    """The sheet's glyph for font f: the PC's w x fh box and a PC pixel more on each side."""
    cw, ch, cols = FONT_CELL
    y = f * sheet.shape[0] // 2 + code // cols * ch + FONT_INSET[1]
    x = code % cols * cw + FONT_INSET[0]
    return sheet[y:y + fh * 2, x:x + min((w + 2) * 2, cw - FONT_INSET[0])]


def glyph_sharp(a):
    """An enlarged PC glyph's edge narrowed, which would otherwise stay soft."""
    return np.clip((a.astype(np.float32) - 48) * 255.0 / 159.0, 0, 255).astype(np.uint8)


def write_font(pack, exe_path, data=None):
    """pack/ui/font.t3x: both fonts' glyphs, shrunk to the 3DS size, packed into one LA4 atlas
    (white, the glyph as alpha). pack/ui/font.bin: "FNT1", u16 font count, then per font u16 cell
    width, cell height and glyph count and per glyph u16 code, u16 x, u16 y (the atlas rectangle's
    corner), u8 width, u8 height (the rectangle, in 3DS pixels), u8 advance (PC units), u8 0. The
    advance is the PC's: the width plus 2, 7 for the space and the cell width plus 2 for the
    full-width codes (0x47f400)."""
    from pcexe import Exe
    exe = Exe(exe_path)
    atlas = np.zeros((FONT_ATLAS, FONT_ATLAS), np.uint8)
    sheet = ee_font(data) if VITA and data else None
    print('font: the Enhanced Edition\'s sheet' if sheet is not None else 'font: the exe\'s glyphs')
    out = [b'FNT1', struct.pack('<H', 2)]
    x = y = 0
    pad = 1
    for f in (0, 1):
        fw, fh = struct.unpack('<HH', exe.read(FONT_SIZES + 4 * f, 4))
        sh = int(round(fh * FONT_SY))
        recs = []
        for code in FONT_CODES:
            w, px = font_glyph(exe, f, code)
            adv = 7 if code == 0 else w + 2 if code < 0xe0 else fw + 2
            if code == 0 or not w or not px.any():
                recs.append(struct.pack('<HHHBBBB', code, 0, 0, 0, 0, adv, 0))
                continue
            if sheet is not None:
                hi = sheet_glyph(sheet, f, code, w, fh)
                sw = max(1, int(round(hi.shape[1] / 2 * FONT_SX)))
                small = np.asarray(Image.fromarray(hi).resize((sw, sh), Image.LANCZOS))
            else:
                sw = max(1, int(round(w * FONT_SX)))
                small = np.asarray(Image.fromarray(px[:, :w]).resize((sw, sh), Image.LANCZOS))
                if VITA:
                    small = glyph_sharp(small)
            if x + sw + pad > FONT_ATLAS:
                x, y = 0, y + sh + pad
            if y + sh + pad > FONT_ATLAS:
                raise ValueError('font atlas full')
            atlas[y:y + sh, x:x + sw] = small
            recs.append(struct.pack('<HHHBBBB', code, x, y, sw, sh, adv, 0))
            x += sw + pad
        x, y = 0, y + sh + pad
        out.append(struct.pack('<HHH', fw, fh, len(recs)))
        out += recs
    height = 64
    while height < y:
        height *= 2
    os.makedirs(os.path.join(pack, 'ui'), exist_ok=True)
    with open(os.path.join(pack, 'ui', 'font.bin'), 'wb') as fp:
        fp.write(b''.join(out))
    rgba = np.zeros((height, FONT_ATLAS, 4), np.uint8)
    rgba[:, :, :3] = 255
    rgba[:, :, 3] = atlas[:height]
    Image.fromarray(rgba, 'RGBA').save(os.path.join(pack, 'ui', 'font.png'))
    t3x = 'ui/font.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('ui/font.png', t3x, True)], pack, fmt='la4', mipmaps=False)
    os.remove(os.path.join(pack, 'ui', 'font.png'))
    print(f'wrote {t3x} ({FONT_ATLAS}x{height}) and ui/font.bin')


def write_map_arrows(data, pack):
    """The arrows of the map screen on the Vita (0x49ce40, 0x49d9d0), with their alpha: map/carsol.t3x, where
    map.c looks, from data/pic/etc/carsol.tex. pics.py's pic/etc/carsol.t3x has none."""
    img = load_tex(os.path.join(data, 'pic', 'etc', 'carsol.tex')).convert('RGBA')
    os.makedirs(os.path.join(pack, 'map'), exist_ok=True)
    img.save(os.path.join(pack, 'map', 'carsol.png'))
    t3x = os.path.join(pack, 'map', 'carsol.t3x')
    if os.path.exists(t3x):
        os.remove(t3x)
    encode_textures([('map/carsol.png', 'map/carsol.t3x', True)], pack, fmt='rgba8', mipmaps=False)
    os.remove(os.path.join(pack, 'map', 'carsol.png'))
    print('wrote map/carsol.t3x')


def write_item_menu(data, pack, exe_path):
    """The item screen's sprites on the Vita (0x475db0, 0x475d20): the PC's sheets of item pictures
    as ui/itemmenu.t3x (data/pic/etc/itemmenu2.tex) and ui/itemmenux.t3x (Born From a Wish,
    data/pic/add/itemmenux.tex), and ui/itemmenu.dat with their tables from the exe, in the original
    sheets' texels: 79 item ids (0x7f65c0), 79 scales (0x7f6160) and 79 u0, v, u1 (0x7f63e0), then 12 ids
    (0x7f6748) and 12 u0, v, u1 (0x7f6700) for Born From a Wish."""
    from pcexe import Exe
    exe = Exe(exe_path)
    jobs = []
    for rel, name, size in (('etc/itemmenu2', 'itemmenu', 1024), ('add/itemmenux', 'itemmenux', 512)):
        img = load_tex(os.path.join(data, 'pic', rel + '.tex')).convert('RGBA').resize((size, size), Image.LANCZOS)
        img.save(os.path.join(pack, 'ui', name + '.png'))
        jobs.append(('ui/%s.png' % name, 'ui/%s.t3x' % name, True))
    for _, t3x, _ in jobs:
        if os.path.exists(os.path.join(pack, t3x)):
            os.remove(os.path.join(pack, t3x))
    encode_textures(jobs, pack, fmt='rgba8', mipmaps=False)
    for png, _, _ in jobs:
        os.remove(os.path.join(pack, png))
    blob = exe.read(0x7f65c0, 79 * 4) + exe.read(0x7f6160, 79 * 4) + exe.read(0x7f63e0, 79 * 6)
    blob += exe.read(0x7f6748, 12 * 4) + exe.read(0x7f6700, 12 * 6)
    with open(os.path.join(pack, 'ui', 'itemmenu.dat'), 'wb') as f:
        f.write(blob)
    print('wrote ui/itemmenu.t3x, ui/itemmenux.t3x and ui/itemmenu.dat')


def write_save_screen(data, pack):
    """The save screen's pictures and texts (0x453fc0 / 0x4537b0, the PC's file list)."""
    from events_pack import write_msg_langs
    jobs = []
    for name in ('savebg', 'savebg2'):
        # 512x512 DXT3; the Image Enhancement Pack's are 2732x2048 (4:3 at square pixels), which
        # squeeze to the same 512x256 since both fill the PC's 4:3 screen. RGB8: the pictures are
        # nearly all one hue (James' red), and RGB565 left 17 of its 137 reds, in blotches
        img = load_tex(os.path.join(data, 'menu', 'mc', name + '.tbn2')).convert('RGB')
        png = 'ui/%s.png' % name
        img.resize(vita_size(img) if VITA else (512, 256), Image.LANCZOS).save(os.path.join(pack, png))
        t3x = 'ui/%s.t3x' % name
        if os.path.exists(os.path.join(pack, t3x)):
            os.remove(os.path.join(pack, t3x))
        jobs.append((png, t3x, False))
    encode_textures(jobs, pack, fmt='rgb8', mipmaps=False)
    for png, _, _ in jobs:
        os.remove(os.path.join(pack, png))
    write_msg_langs(pack, 'mcard.msg', os.path.join(data, 'etc', 'message', 'm_card_x_msg'))
    print('wrote ui/savebg.t3x, ui/savebg2.t3x and events/mcard.msg')


def write_blood(data, pack):
    """The blood effects' texture (data/pic/effect/bloodtex0.tbn2, 512x512 DXT3: white with the
    splashes, spots, footprints and pool clouds in the alpha; the Blood Color tints it): A4 at
    256x256 as pack/effect/blood.t3x."""
    img = load_tex(os.path.join(data, 'pic', 'effect', 'bloodtex0.tbn2')).convert('RGBA')
    os.makedirs(os.path.join(pack, 'effect'), exist_ok=True)
    png, t3x = 'effect/blood.png', 'effect/blood.t3x'
    img.resize((256, 256), Image.LANCZOS).save(os.path.join(pack, png))
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([(png, t3x, True)], pack, fmt='a4', mipmaps=False)
    os.remove(os.path.join(pack, png))
    print('wrote', t3x)


def write_mist(data, pack):
    """The hyper spray's mist texture: effect texture 2 (0x4a4c00 -> 0x4c9b20(2) -> file id 0x2c1a,
    data/pic/effect/poison.tbn2, 256x256 with a cloud in the alpha). The PC tints it with the
    vertex colour and samples a random quarter of it per puff, so only the alpha matters: A8 at
    128x128 as pack/effect/mist.t3x (blood.c wraps it)."""
    img = load_tex(os.path.join(data, 'pic', 'effect', 'poison.tbn2')).convert('RGBA')
    alpha = img.split()[3].resize((128, 128), Image.LANCZOS)
    out = Image.new('RGBA', (128, 128), (255, 255, 255, 0))
    out.putalpha(alpha)
    os.makedirs(os.path.join(pack, 'effect'), exist_ok=True)
    png, t3x = 'effect/mist.png', 'effect/mist.t3x'
    out.save(os.path.join(pack, png))
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([(png, t3x, True)], pack, fmt='a8', mipmaps=False)
    os.remove(os.path.join(pack, png))
    print('wrote', t3x)


def write_room_fx(data, pack):
    """The sprites of the effects a room posts on entry (port/source/roomfx.c). The PC finds each by
    its texture id through the effect texture table at 0x6bc0b8 (0x4c9b20): index 3 (id 0x52e4,
    data/pic/effect/fire01.tbn2) for the burning stairs' flames (classes 47..49, 0x4ec920), index 0
    (0x52e3, playable0.tbn2) for their smoke (class 52, 0x4ecad0) and index 22 (0x10df, flear.tbn2)
    for the lake's guiding light (class 56, 0x4db440). All are 512x512 sprite sheets with alpha:
    fire01 a 4x4 grid of flames, playable0 smoke puffs in its lower half, flear one glow. Written
    as ETC1A4 without mipmaps, fire and smoke at 256x256, the glow at 128x128."""
    os.makedirs(os.path.join(pack, 'effect'), exist_ok=True)
    jobs = []
    for src, name, size in (('fire01', 'fire', 256), ('playable0', 'smoke', 256), ('flear', 'flare', 128)):
        img = load_tex(os.path.join(data, 'pic', 'effect', src + '.tbn2')).convert('RGBA')
        png, t3x = 'effect/%s.png' % name, 'effect/%s.t3x' % name
        img.resize((size, size), Image.LANCZOS).save(os.path.join(pack, png))
        if os.path.exists(os.path.join(pack, t3x)):
            os.remove(os.path.join(pack, t3x))
        jobs.append((png, t3x, True))
    encode_textures(jobs, pack, fmt='etc1a4', mipmaps=False)
    for png, t3x, _ in jobs:
        os.remove(os.path.join(pack, png))
        print('wrote', t3x)


def write_lens_flare(data, pack):
    """The lens flare's sprites (port/source/lensflare.c): data/etc/effect/lens_flare.tbn2, which
    0x511ab0 loads as texture 0x9e, a 128x128 sheet with alpha (a round glow and a ring in its top
    half, the streaks' corner, an oval and a ghost in its lower half; the vertex colours tint them).
    Written as RGBA8 at 128x128 without mipmaps, so the soft edges keep their gradient."""
    os.makedirs(os.path.join(pack, 'effect'), exist_ok=True)
    img = load_tex(os.path.join(data, 'etc', 'effect', 'lens_flare.tbn2')).convert('RGBA')
    png, t3x = 'effect/lensflare.png', 'effect/lensflare.t3x'
    img.resize((128, 128), Image.LANCZOS).save(os.path.join(pack, png))
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([(png, t3x, True)], pack, fmt='rgba8', mipmaps=False)
    os.remove(os.path.join(pack, png))
    print('wrote', t3x)


def write_screens(data, pack, out_voice=True):
    """The PC's title background (data/pic/etc/start00.tex, the menus are text over it:
    pack/ui/start00.t3x, 512x512 DXT3 squeezed to 512x256 RGB565 like the stills) and its pulsing
    white logo (start01.tex's top 512x96, ui/titleglow.t3x), the start-up screens
    (konami2048x2048.tex and splash_logo.tex as ui/boot0.t3x and ui/boot1.t3x), the game over
    text (gameover1.tex: four "GAME OVER" of 160x48 in its top left quarter, pack/ui/gameover.t3x,
    LA4 256x256) and the game over sound (voice.afs 143, SE 60143 in 0x498780)."""
    os.makedirs(os.path.join(pack, 'ui'), exist_ok=True)
    jobs = []
    img = load_tex(os.path.join(data, 'pic', 'etc', 'start00.tex')).convert('RGB')
    img.resize(vita_size(img) if VITA else (512, 256), Image.LANCZOS).save(os.path.join(pack, 'ui', 'start00.png'))
    jobs.append(('ui/start00.png', 'ui/start00.t3x', False, 'rgb8' if VITA else 'rgb565'))
    img = load_tex(os.path.join(data, 'pic', 'etc', 'gameover1.tex')).convert('RGBA').crop((0, 0, 256, 256))
    img.save(os.path.join(pack, 'ui', 'gameover.png'))
    jobs.append(('ui/gameover.png', 'ui/gameover.t3x', True, 'la4'))
    # the title's white logo that pulses over start00's red one (start01.tex's first item, 0x496dd0):
    # the top 512x96 of the PC's 512x512. It must outline start00's letters, so it is the original's
    # as start00 is: the Enhanced Edition's start00 doesn't decode here (the original is packed),
    # and its start01 (2048x2048) lays a smaller logo out over "ENHANCED EDITION"; its top-left
    # 512x96 was a corner of it, a white blob pulsing over the title. Larger ones are scaled down.
    img = decode_tex(os.path.join(data, 'pic', 'etc', 'start01.tex')).convert('RGBA')
    w, h = img.size
    img = img.crop((0, 0, w, h * 96 // 512))
    if img.size != (512, 96):
        img = img.resize((512, 96), Image.LANCZOS)
    glow = Image.new('RGBA', (512, 128), (255, 255, 255, 0))
    glow.paste(img, (0, 0))
    bg_path = ee.file(os.path.join(data, 'pic', 'etc', 'start00.tex'), note=False)
    if VITA and ee.is_ee(bg_path) and bg_path not in ee._rejected:
        # The Enhanced Edition's start00, which the Vita takes whole, has its logo elsewhere: its own
        # start01's logo (rows 29..354 of 2048) on a canvas of the whole picture, where the two line up
        # (measured against start00's letters); titlemenu.c lays it over the picture across the whole
        # screen. The picture is widened to 16:9, not cut: its logo is 6.4:1 at 4:3 where the original's
        # stands 8.9:1, and the Enhanced Edition on PC shows it widened too.
        sheet = decode_tex(ee.file(os.path.join(data, 'pic', 'etc', 'start01.tex'), note=False)).convert('RGBA')
        glow = Image.new('RGBA', (1024, 512), (255, 255, 255, 0))
        glow.paste(sheet.crop((82, 29, 1973, 354)).resize((711, 119), Image.LANCZOS), (157, 147))
    glow.save(os.path.join(pack, 'ui', 'titleglow.png'))
    jobs.append(('ui/titleglow.png', 'ui/titleglow.t3x', True, 'la4'))
    # the start-up screens (menu events 4 and 5, 0x405e90): the Konami logo and the splash screen
    for name, out in (('konami2048x2048', 'boot0'), ('splash_logo', 'boot1')):
        img = load_tex(os.path.join(data, 'pic', 'etc', name + '.tex')).convert('RGB')
        img.resize(vita_size(img) if VITA else (512, 256), Image.LANCZOS).save(os.path.join(pack, 'ui', out + '.png'))
        jobs.append(('ui/%s.png' % out, 'ui/%s.t3x' % out, False, 'rgb8' if VITA else 'rgb565'))
    if VITA:
        # start01's menu words as pictures (titlemenu.c s_cell has where each one is), DXT5
        img = load_tex(os.path.join(data, 'pic', 'etc', 'start01.tex')).convert('RGBA')
        img.resize((1024, 1024), Image.LANCZOS).save(os.path.join(pack, 'ui', 'titleitems.png'))
        jobs.append(('ui/titleitems.png', 'ui/titleitems.t3x', True, 'etc1a4'))
    for png, t3x, alpha, fmt in jobs:
        if os.path.exists(os.path.join(pack, t3x)):
            os.remove(os.path.join(pack, t3x))
        encode_textures([(png, t3x, alpha)], pack, fmt=fmt, mipmaps=False)
        os.remove(os.path.join(pack, png))
    if out_voice:
        from demos import copy_voice
        copy_voice(None, data, pack, 143, False)
    print('wrote ui/start00.t3x, ui/gameover.t3x, ui/titleglow.t3x, ui/boot0.t3x, ui/boot1.t3x and '
          'sound/voice/143.adx')


def write_options(data, pack):
    """The options screens' texts (0x461600 / 0x464eb0 draw option_msg_?.mes by number)."""
    from events_pack import write_msg_langs
    write_msg_langs(pack, 'option.msg', os.path.join(data, 'etc', 'message', 'option_msg'))
    print('wrote events/option.msg')


# The pause menu's strings are in the exe, nine per language (0x407310 picks the table by the
# language 0x1dbec06): the title, Resume, Save Game, Load Game, Options, Quit, the quit question
# (0x407620), Yes and No.
PAUSE_TABLES = (('e', 0x7abd84), ('f', 0x7abda8), ('g', 0x7abdcc), ('i', 0x7abe14), ('s', 0x7abdf0))


def exe_text(s):
    """A string the exe hands to 0x482ea0, as the pack's text: after \\h a byte b is the code
    b - 0x20, which is the pack's character b; \\n starts a line and the other escapes (\\c
    centre, \\h, ...) are dropped."""
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            if s[i + 1] == 'n':
                out.append('\n')
            i += 2
            continue
        out.append(s[i])
        i += 1
    return ''.join(out)


def write_pause(pack, exe_path):
    """pack/events/pause.msg and events/<f, g, i, s>/pause.msg: the pause menu's strings."""
    from pcexe import Exe
    from events_pack import lang_dir, write_msg
    exe = Exe(exe_path)
    for lang, table in PAUSE_TABLES:
        write_msg(os.path.join(lang_dir(pack, lang), 'pause.msg'),
                  [exe_text(exe.cstr(exe.u32(table + 4 * i), 96)) for i in range(9)])
    print('wrote events/pause.msg')


def main():
    args = sys.argv[1:]
    exe_path = None
    if '--exe' in args:
        i = args.index('--exe')
        exe_path = args[i + 1]
        del args[i:i + 2]
    if exe_path is None:
        from events import DEFAULT_EXE
        exe_path = DEFAULT_EXE
    data, pack = args[0], args[1]
    write_font(pack, exe_path, data)
    if VITA:
        write_item_menu(data, pack, exe_path)
        write_map_arrows(data, pack)
    write_save_screen(data, pack)
    write_options(data, pack)
    write_pause(pack, exe_path)
    write_blood(data, pack)
    write_mist(data, pack)
    write_room_fx(data, pack)
    write_lens_flare(data, pack)
    write_screens(data, pack)
    os.makedirs(os.path.join(pack, 'ui'), exist_ok=True)
    logo = os.path.join(data, 'pic', 'etc', 'sh2dc.tex')
    img = ee.like_original(load_tex(logo), logo).convert('RGB')
    img.crop((0, 136, 512, 264)).save(os.path.join(pack, 'ui', 'title.png'))
    t3x = 'ui/title.t3x'
    if os.path.exists(os.path.join(pack, t3x)):
        os.remove(os.path.join(pack, t3x))
    encode_textures([('ui/title.png', t3x, False)], pack, fmt='rgb565', mipmaps=False)
    print('wrote', t3x)


if __name__ == '__main__':
    main()
