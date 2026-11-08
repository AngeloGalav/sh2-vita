#!/usr/bin/env python3
"""Converts the PC's save files (sh2pcsave??.dat) into the port's (save<N>.dat).

    python3 tools/pcsave.py <dir with the .dat files> [--out dir] [--first-slot 10]

A PC file is 0x2000 bytes: two check words, the game data scrambled with a key that sits inside it, and
more checks (0x44d780 writes and reads it, 0x44d2f0 makes the key, 0x4553a0 checks what comes out). The
game data is what 0x455220 gathers, 0x1fc0 bytes from 0x1f5f300. Files whose checks fail are left out.

The port's files are numbered from the first slot given, so slots 0..9 (save.dat, save2.dat ..) are not
touched: slot N is save<N+1>.dat. The Vita build lists 75 slots, the 3DS build 10. What neither the port's
format nor its game state holds is dropped: the room comes from the stage and the position (apply_warp,
empty map) unless --pack is given, then from the rooms whose bounds hold the position (bg/<area>/<room>.s3m),
Maria's place and the radio are not carried over.
"""
import argparse
import glob
import math
import os
import struct
import sys

GAME_SIZE = 0x1fc0
SAVE_SIZE = 2488                 # sizeof(SaveFile), port/source/main.c, version 7
N_FLAGS, N_ITEMS = 2048, 256     # EV_STATE_SIZE: the flags, then a count per item


def rol(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & 0xffffffff if n else v


def decode(d):
    """0x44d780, its second mode. The game data, or None when a check fails."""
    if len(d) != 0x2000:
        return None
    body = d[8:8 + 0x3fd * 8]
    total = sum(body)
    low0 = sum(1 for b in body if b & 15 == 0)
    w0, w1 = struct.unpack_from('<II', d, 0)
    if w0 != rol(total, low0):
        return None
    at = 0x938dd0 - 0x936e10 - (low0 % 0x3f8) * 8    # the key's place: 0x938dd0, less a part of the count
    key = body[at:at + 40]
    ksum = sum(key)
    if key[39] != ((ksum - key[39]) ^ 0x21) & 0xff:
        return None
    if w1 != rol((low0 - total - ksum) & 0xffffffff, low0):
        return None
    rest = low0 % 0x3f8
    data = bytearray(body)
    data[at:at + rest * 8] = data[at + 40:at + 40 + rest * 8]    # the key out of the way
    k = struct.unpack('<6I', key[:24])
    out, i, j = [], 0, 0
    words = struct.unpack('<%dI' % (0x3f8 * 2), bytes(data[:0x3f8 * 8]))
    for n in range(0x3f8):
        m = key[24 + j] * 0x01010101
        out += [words[n * 2] ^ k[i * 2] ^ m, words[n * 2 + 1] ^ k[i * 2 + 1] ^ m]
        i += 1
        if i > 2:
            i, j = 0, (j + 1) % 16
    game = struct.pack('<%dI' % len(out), *out)
    return game if struct.unpack_from('<I', game, 0)[0] == 5 else None


def room_finder(pack):
    """(stage, x, y, z) -> the room the position is in, from the pack's rooms of that stage's areas and their
    bounds (an .s3m's six floats at 8: min x, y, z, max x, y, z). The smallest that holds the position wins, so
    a floor of a building beats the block round it. None when nothing holds it: the game then picks."""
    if not pack:
        return lambda *a: None
    rooms = [l.split() for l in open(os.path.join(pack, 'events', 'rooms.txt')) if len(l.split()) == 2]
    areas = sorted(os.listdir(os.path.join(pack, 'bg')))
    area_of = lambda m: max((a for a in areas if m.startswith(a)), key=len, default=None)
    bounds = {}
    for a in areas:
        d = os.path.join(pack, 'bg', a)
        for f in os.listdir(d) if os.path.isdir(d) else []:
            if f.endswith('.s3m'):
                with open(os.path.join(d, f), 'rb') as h:
                    b = h.read(32)
                if b[:4] == b'S3M1':
                    bounds[f[:-4]] = struct.unpack_from('<6f', b, 8)
    stage_areas = {}
    for m, st in rooms:
        a = area_of(m)
        if a:
            stage_areas.setdefault(int(st), set()).add(a)

    def find(stage, x, y, z):
        best = None
        for m, b in bounds.items():
            if area_of(m) not in stage_areas.get(stage, ()):
                continue
            if b[0] <= x <= b[3] and b[2] <= z <= b[5] and b[1] - 50 <= y <= b[4] + 50:
                vol = (b[3] - b[0]) * (b[5] - b[2]) * max(b[4] - b[1], 1.0)
                if best is None or vol < best[0]:
                    best = (vol, m)
        return best[1] if best else None
    return find


def convert(g, find_room=lambda *a: None):
    """0x455220's layout into port/source/main.c's SaveFile: the block from 0x1dbeb80 at 0x2c, gGameFlag
    at 0xc0, the items from 0x1f7d3e0 at 0x560, James' state (0x53aa10) at 0x59c."""
    sysb = lambda addr: 0x2c + addr - 0x1dbeb80
    f32 = lambda off: struct.unpack_from('<f', g, off)[0]
    u16 = lambda off: struct.unpack_from('<H', g, off)[0]
    stage = g[sysb(0x1dbeb80)]
    x, y, z, yaw = struct.unpack_from('<4f', g, 0x1c)
    place = g[6] or g[4]         # a save point's number, or 157 + the stage from the pause menu
    # flags up to the puzzle bytes, which the port keeps in the program state (0x1f7da60)
    flags = bytearray(N_FLAGS)
    flags[:0x480] = g[0xc0:0xc0 + 0x480]
    # 0x49e310: a bit an item, and a count for the first twelve (0x1f7d3ec + item * 2); a count here is a byte
    items = bytearray(N_ITEMS)
    bits = int.from_bytes(g[0x560:0x56c], 'little')
    for it in range(1, 96):
        if bits >> it & 1:
            items[it] = 1
    for it in range(1, 13):
        n = u16(0x56c + it * 2)
        if n or it not in (4, 6, 8):
            items[it] = min(n, 255)
    for it, marker in ((4, 250), (6, 251), (8, 252)):   # combat.c WEAPONS: an empty gun is still owned
        if bits >> it & 1:
            items[marker] = 1
    time = f32(sysb(0x1dbeb94))
    if not (time >= 0 and time < 1e7):
        time = 0.0
    prog = bytearray(64)
    prog[:32] = g[0xc0 + 0x480:0xc0 + 0x4a0]
    prog[32] = g[sysb(0x1dbebf5)]    # riddle
    prog[33] = g[sysb(0x1dbebf4)]    # action
    prog[34] = 1 if g[sysb(0x1dbec0c)] == 1 else 0
    prog[35] = g[sysb(0x1dbeb88)]
    struct.pack_into('<3f', prog, 36, f32(sysb(0x1dbebb0)), f32(sysb(0x1dbebac)), f32(sysb(0x1dbeba8)))
    walk = min(int(max(f32(sysb(0x1dbeb98)), 0.0) / 1000.0), 0xffff)
    run = min(int(max(f32(sysb(0x1dbeb9c)), 0.0) / 1000.0), 0xffff)
    struct.pack_into('<f6H', prog, 48, time, walk, run, u16(sysb(0x1dbeb8a)), u16(sysb(0x1dbeb8e)),
                     u16(sysb(0x1dbeb90)), u16(sysb(0x1dbeb92)))
    hp = f32(0x59c)
    hp = hp if 0 < hp <= 100 else 100.0
    clear = g[sysb(0x1dbeb87)] == 0xff
    weapon = g[0x560 + 0x29] or 0xff     # 0x1f7d409
    room = find_room(stage, x, y, z)                              # else apply_warp picks one
    out = b'SH3S' + struct.pack('<II', 7, stage) + (room or '').encode()[:7].ljust(8, b'\0')
    out += struct.pack('<4f', x, y, z, 0.0 if math.isnan(yaw) else yaw)
    out += bytes(flags) + bytes(items) + bytes(prog)
    out += g[0xc0 + 0x400:0xc0 + 0x440]                           # dead enemies, 0x1f7d9e0
    out += struct.pack('<f', hp) + bytes([1 if clear else 0])
    out += bytes([g[sysb(0x1dbeb82)], g[sysb(0x1dbeb84)], g[sysb(0x1dbeb85)]])
    out += bytes([0xff if clear else place, weapon, 0, 0])
    out += struct.pack('<2f', f32(sysb(0x1dbeba0)), f32(sysb(0x1dbeba4)))
    assert len(out) == SAVE_SIZE
    return out, stage, place, time, clear


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--out', help='where to write (default the same folder)')
    ap.add_argument('--first-slot', type=int, default=10)
    ap.add_argument('--pack', help='the pack (the folder with bg/ and events/), to find each save\'s room; default the input folder when it is one')
    a = ap.parse_args()
    out = a.out or a.dir
    os.makedirs(out, exist_ok=True)
    slot, bad = a.first_slot, 0
    pack = a.pack or (a.dir if os.path.exists(os.path.join(a.dir, 'events', 'rooms.txt')) else None)
    find_room = room_finder(pack)
    unplaced = 0
    for path in sorted(glob.glob(os.path.join(a.dir, 'sh2pcsave*.dat'))):
        with open(path, 'rb') as f:
            g = decode(f.read())
        if g is None:
            print('%s: left out, its checks fail' % os.path.basename(path), file=sys.stderr)
            bad += 1
            continue
        data, stage, place, time, clear = convert(g, find_room)
        room = data[12:20].split(b'\0')[0].decode()
        unplaced += not room and not clear
        name = 'save.dat' if slot == 0 else 'save%d.dat' % (slot + 1)
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
        print('%s -> %-11s stage %2d, room %-6s place %3d, %s' % (os.path.basename(path), name, stage, room or '?', place,
              'clear data' if clear else '%d:%02d:%02d' % (time // 3600, time % 3600 // 60, time % 60)))
        slot += 1
    print('%d converted, %d left out, %d with no room found; next free slot %d' % (slot - a.first_slot, bad, unplaced, slot))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
