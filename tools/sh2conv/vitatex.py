"""Rewrites a pack's .t3x textures in the form the PS Vita build uploads as they are (port/vita).

    python3 tools/sh2conv/vitatex.py <pack dir>      every .t3x under it, in place

The 3DS keeps textures in 8x8 tiles in Morton order, and its ETC1A4 has no counterpart on the
Vita's GPU, so the Vita build used to untile every texture as it came in and decode ETC1A4 to
RGBA4444: 30 to 160 ms of the CPU each time new textures arrived (vita_log.txt, "untile"). Here,
once, each level is laid out in GL's row order (the same rows port/vita/compat/c3d_tex.c's untile
gives) and the format byte gets 0x80:

  ETC1      the 4x4 blocks in row order, each 64-bit word big-endian, as GL_ETC1_RGB8_OES wants
  ETC1A4    decoded and encoded again as DXT5 (BC3): the same 8 bits a texel, so every level keeps
            its size and place and the engine's level arithmetic (t3xcap.c, room.c tex_halve) holds
  RGBA8, RGB8, RGB565, RGBA5551, RGBA4, LA8, HILO8, L8, A8
            untiled, with the byte order c3d_tex.c's untile gives
  LA4, L4, A4 left as they are (GL takes them a byte a texel; the font and a few small ones)

A pack can mix converted and unconverted files: the Vita build reads both. Converted files are
skipped, so running it twice changes nothing. The 3DS build cannot read the converted files.
"""
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HEAD = 17
VITA = 0x80
ETC1, ETC1A4 = 0x0c, 0x0d
BPP = {0x00: 32, 0x01: 24, 0x02: 16, 0x03: 16, 0x04: 16, 0x05: 16, 0x06: 16, 0x07: 8, 0x08: 8,
       0x0c: 4, 0x0d: 8}
RAW_SWAP = {0x00: [3, 2, 1, 0], 0x01: [2, 1, 0], 0x05: [1, 0], 0x06: [1, 0]}   # c3d_tex.c untile

ETC_MOD = np.array([[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42],
                    [18, 60, -18, -60], [24, 80, -24, -80], [33, 106, -33, -106],
                    [47, 183, -47, -183]], np.int32)


def _morton_xy():
    mx = np.zeros(64, np.int64)
    my = np.zeros(64, np.int64)
    for y in range(8):
        for x in range(8):
            m = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3
            mx[m], my[m] = x, y
    return mx, my


MX, MY = _morton_xy()


def untile_raw(src, w, h, nbytes):
    """The PICA's tiled texels (nbytes each) in GL row order."""
    px = np.frombuffer(src, np.uint8).reshape(h // 8, w // 8, 64, nbytes)
    out = np.empty((h, w, nbytes), np.uint8)
    ty = np.arange(h // 8)[:, None, None]
    tx = np.arange(w // 8)[None, :, None]
    out[ty * 8 + MY[None, None, :], tx * 8 + MX[None, None, :]] = px
    return out


def etc_blocks_in_rows(src, w, h, blk):
    """The ETC blocks (blk bytes each) of one level, from the PICA's order (four to an 8x8 tile,
    sub-block (bx & 1) + 2 (by & 1)) to rows of blocks."""
    b = np.frombuffer(src, np.uint8).reshape(h // 8, w // 8, 2, 2, blk)   # tile y, tile x, sub y, sub x
    return b.transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, blk)


def etc1_decode(words):
    """ETC1 colour words (uint64, as read little-endian from the PICA) to RGB, (n, 4, 4, 3) with
    [row][column], as c3d_tex.c's etc1_block."""
    hi = (words >> np.uint64(32)).astype(np.int64)
    lo = (words & np.uint64(0xffffffff)).astype(np.int64)
    diff = (hi & 2) != 0
    flip = (hi & 1) != 0
    base = np.empty((len(words), 2, 3), np.int32)
    for c in range(3):
        b = (hi >> (27 - c * 8)) & 31
        d = (hi >> (24 - c * 8)) & 7
        d = np.where(d >= 4, d - 8, d)
        b2 = (b + d) & 31
        dif0, dif1 = (b << 3) | (b >> 2), (b2 << 3) | (b2 >> 2)
        ind0, ind1 = ((hi >> (28 - c * 8)) & 15) * 17, ((hi >> (24 - c * 8)) & 15) * 17
        base[:, 0, c] = np.where(diff, dif0, ind0)
        base[:, 1, c] = np.where(diff, dif1, ind1)
    table = np.stack([(hi >> 5) & 7, (hi >> 2) & 7], 1)
    out = np.empty((len(words), 4, 4, 3), np.int32)
    for x in range(4):
        for y in range(4):
            i = x * 4 + y
            sub = np.where(flip, int(y >= 2), int(x >= 2))
            idx = (((lo >> (i + 16)) & 1) << 1) | ((lo >> i) & 1)
            m = ETC_MOD[table[np.arange(len(words)), sub], idx]
            out[:, y, x, :] = base[np.arange(len(words)), sub] + m[:, None]
    return np.clip(out, 0, 255).astype(np.uint8)


def a4_decode(words):
    """ETC1A4's alpha words to (n, 4, 4) bytes, [row][column]: texel x * 4 + y in bits 4i.."""
    out = np.empty((len(words), 4, 4), np.uint8)
    for x in range(4):
        for y in range(4):
            out[:, y, x] = ((words >> np.uint64((x * 4 + y) * 4)) & np.uint64(15)).astype(np.uint8) * 17
    return out


def _to565(c):
    c = np.clip(np.rint(c), 0, 255).astype(np.int32)
    return ((c[..., 0] * 31 + 127) // 255) << 11 | ((c[..., 1] * 63 + 127) // 255) << 5 | ((c[..., 2] * 31 + 127) // 255)


def _from565(v):
    r, g, b = v >> 11 & 31, v >> 5 & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def _colour_indices(px, c0, c1):
    """The nearest of the four colours of endpoints c0, c1 (565) for each texel: (n, 16), and the
    squared error of each block."""
    e0, e1 = _from565(c0), _from565(c1)
    pal = np.stack([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)      # (n, 4, 3)
    d = ((px[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)            # (n, 16, 4)
    idx = d.argmin(-1)
    return idx, np.take_along_axis(d, idx[..., None], -1)[..., 0].sum(-1)


W4 = np.array([1.0, 0.0, 2 / 3, 1 / 3], np.float32)   # each index's share of endpoint 0


def bc1_colour(px):
    """DXT colour blocks for px (n, 16, 3) float, texel t = row * 4 + column: (c0, c1, indices),
    c0 > c1 as BC1's four-colour mode has it. Endpoints along the block's principal axis, then one
    least-squares refit from the indices, kept where it is better."""
    n = len(px)
    mean = px.mean(1)
    d = px - mean[:, None, :]
    cov = np.einsum('nti,ntj->nij', d, d)
    axis = px.max(1) - px.min(1) + 1e-3
    for _ in range(6):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.linalg.norm(axis, axis=1, keepdims=True) + 1e-12
    t = np.einsum('nti,ni->nt', d, axis)
    lo, hi = t.min(1), t.max(1)
    inset = (hi - lo) / 32
    c0 = _to565(mean + axis * (hi - inset)[:, None])
    c1 = _to565(mean + axis * (lo + inset)[:, None])
    idx, err = _colour_indices(px, c0, c1)
    # refit: px ~ w e0 + (1 - w) e1 with w from the indices
    w = W4[idx]
    a, b = w, 1 - w
    aa, bb, ab = (a * a).sum(1), (b * b).sum(1), (a * b).sum(1)
    det = aa * bb - ab * ab
    ok = det > 1e-6
    det = np.where(ok, det, 1)
    ax = np.einsum('nt,nti->ni', a, px)
    bx = np.einsum('nt,nti->ni', b, px)
    r0 = (bb[:, None] * ax - ab[:, None] * bx) / det[:, None]
    r1 = (aa[:, None] * bx - ab[:, None] * ax) / det[:, None]
    f0, f1 = _to565(r0), _to565(r1)
    fidx, ferr = _colour_indices(px, f0, f1)
    better = ok & (ferr < err)
    c0 = np.where(better, f0, c0)
    c1 = np.where(better, f1, c1)
    idx = np.where(better[:, None], fidx, idx)
    # four-colour mode wants c0 > c1: swap (indices 0 <-> 1, 2 <-> 3); equal ones take index 0
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    idx = np.where(swap[:, None], idx ^ 1, idx)
    idx = np.where((c0 == c1)[:, None], 0, idx)
    return c0, c1, idx


def bc3_alpha(a):
    """DXT5 alpha blocks for a (n, 16) bytes: (a0, a1, 3-bit indices), each block in the better of
    the two modes: eight levels from its largest (a0) to its smallest (a1), or (a0 < a1) six levels
    between its smallest and largest other than 0 and 255, with 0 and 255 exact."""
    a = a.astype(np.int32)
    n = len(a)
    hi, lo = a.max(1), a.min(1)
    j = np.array([0, 7, 1, 2, 3, 4, 5, 6])                 # index -> step from a0 towards a1
    lev8 = ((7 - j)[None, :] * hi[:, None] + j[None, :] * lo[:, None]) / 7.0
    mid = np.where((a > 0) & (a < 255), a, -1)
    m_hi = mid.max(1)
    m_lo = np.where(mid >= 0, mid, 256).min(1)
    none = m_hi < 0
    m_lo, m_hi = np.where(none, 0, m_lo), np.where(none, 0, m_hi)
    k = np.array([0, 5, 1, 2, 3, 4])
    lev6 = np.concatenate([((5 - k)[None, :] * m_lo[:, None] + k[None, :] * m_hi[:, None]) / 5.0,
                           np.zeros((n, 1)), np.full((n, 1), 255.0)], 1)
    d8 = np.abs(a[:, :, None] - lev8[:, None, :])
    d6 = np.abs(a[:, :, None] - lev6[:, None, :])
    i8, i6 = d8.argmin(-1), d6.argmin(-1)
    e8 = (np.take_along_axis(d8, i8[..., None], -1) ** 2).sum((1, 2))
    e6 = (np.take_along_axis(d6, i6[..., None], -1) ** 2).sum((1, 2))
    six = (e6 < e8) & (m_lo < m_hi)                       # six levels need a0 < a1
    a0 = np.where(six, m_lo, hi)
    a1 = np.where(six, m_hi, lo)
    idx = np.where(six[:, None], i6, i8)
    idx = np.where(((a0 == a1) & ~six)[:, None], 0, idx)   # one value: a0 == a1 reads as eight levels
    return a0, a1, idx


def bc3_encode(rgba):
    """(n, 16, 4) RGBA bytes, t = row * 4 + column, to n DXT5 blocks of 16 bytes."""
    n = len(rgba)
    a0, a1, ai = bc3_alpha(rgba[:, :, 3])
    c0, c1, ci = bc1_colour(rgba[:, :, :3].astype(np.float32))
    out = np.zeros((n, 16), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    abits = np.zeros(n, np.uint64)
    for t in range(16):
        abits |= ai[:, t].astype(np.uint64) << np.uint64(3 * t)
    for k in range(6):
        out[:, 2 + k] = ((abits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    out[:, 8], out[:, 9] = c0 & 255, c0 >> 8
    out[:, 10], out[:, 11] = c1 & 255, c1 >> 8
    cbits = np.zeros(n, np.uint64)
    for t in range(16):
        cbits |= ci[:, t].astype(np.uint64) << np.uint64(2 * t)
    for k in range(4):
        out[:, 12 + k] = ((cbits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    return out


def level(fmt, src, w, h):
    """One level in the Vita's form."""
    if fmt == ETC1:
        b = etc_blocks_in_rows(src, w, h, 8)
        return b[..., ::-1].tobytes()                      # GL wants the word big-endian
    if fmt == ETC1A4:
        b = etc_blocks_in_rows(src, w, h, 16).reshape(-1, 16)
        alpha = b[:, :8].copy().view('<u8')[:, 0]
        colour = b[:, 8:].copy().view('<u8')[:, 0]
        rgba = np.concatenate([etc1_decode(colour), a4_decode(alpha)[..., None]], -1)   # (n, 4, 4, 4)
        return bc3_encode(rgba.reshape(-1, 16, 4)).tobytes()
    nb = BPP[fmt] // 8
    px = untile_raw(src, w, h, nb)
    if fmt in RAW_SWAP:
        px = px[..., RAW_SWAP[fmt]]
    return px.tobytes()


def convert(path):
    """One .t3x in place. Returns 'converted', 'done' (already) or 'kept' (a format left alone)."""
    with open(path, 'rb') as f:
        d = bytearray(f.read())
    count = d[0] | d[1] << 8
    fmt = d[3]
    if fmt & VITA:
        return 'done'
    if fmt not in BPP:
        return 'kept'
    hdr = 5 + count * 12
    body = hdr + (4 if d[hdr] == 0 else 8)
    if d[hdr] not in (0x00, 0x80):
        return 'kept'                                          # compressed: none in the packs
    w, h, mips = 1 << ((d[2] & 7) + 3), 1 << ((d[2] >> 3 & 7) + 3), d[4]
    out = bytearray()
    pos = body
    for k in range(mips + 1):
        lw, lh = w >> k, h >> k
        n = lw * lh * BPP[fmt] // 8
        if pos + n > len(d):
            break
        out += level(fmt, bytes(d[pos:pos + n]), lw, lh)
        pos += n
    d[body:body + len(out)] = out
    d[3] = fmt | VITA
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(d)
    os.replace(tmp, path)
    return 'converted'


def convert_pack(root, jobs=None):
    paths = [os.path.join(r, n) for r, _, fs in os.walk(root) for n in fs if n.endswith('.t3x')]
    counts = {}
    with ProcessPoolExecutor(jobs) as ex:
        for i, res in enumerate(ex.map(convert, paths, chunksize=4)):
            counts[res] = counts.get(res, 0) + 1
            if (i + 1) % 200 == 0 or i + 1 == len(paths):
                print(f'  {i + 1}/{len(paths)} textures', flush=True)
    return counts


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    c = convert_pack(sys.argv[1])
    print(', '.join(f'{v} {k}' for k, v in sorted(c.items())))
