#!/usr/bin/env python3
"""One character motion the stage code sets by id, as an .s3a the port's chr.c plays.

  motion.py [--data DIR] [--exe FILE] [--out DIR]

The stages place some characters themselves (CharaCreate 0x48fcc0, then a motion setter such as
0x540cf0 with an id), outside any scene, from a file list of their own: the motion's record (id,
frames, speed, start, end, loop) is in a table, and the motions lie back to back in the .anm. Writes
pack/chr/<folder>/<name>.s3a with the frames from the motion's start to its end (S3A1, chr.py
convert_anm's layout).

  Maria lying in the day room S3 (stage 20, room 0x41, 0x58ef90): kind 0x106 (hhh Maria) with
  the file list 0x8db88c (data/demo/mar_hagureru/hhh_mar.anm) and motion 0xd49, which 0x540cf0 finds
  in the table at 0x6e7728 (ids from 3400; the motions lie back to back in the .anm) ->
  pack/chr/mar/hhh_mar_s3.s3a

Whole files, played by the port's own code:
  The boat on the lake (stage 38, character 0x10b, 0x53fe40): data/chr/boat/boat.anm, 474 frames.
  The motions 51, 52 and 53 (table 0x6e2298: both oars, the left, the right) are frames 0..149,
  150..299 and 300..449; 54, the boat lying at the pier, is 450..473 -> pack/chr/boat/boat.s3a
  James rowing: data/chr/jms/jms_boat.anm (file record 0x7d87b8), 225 frames of hll_jms' 41 bones,
  three loops of 75 frames that go with the boat's three (James' motions 0x4e24..0x4e26 in the lake's
  table 0x8cf3d8 are 150 frames long, so they play at half the boat's frame rate)
  -> pack/chr/jms/jms_boat.s3a
"""
import argparse
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ee  # noqa: E402
import sh2mdl  # noqa: E402
from demos import DEFAULT_DATA, DEFAULT_EXE, DEFAULT_OUT, anm_frames  # noqa: E402
from sound import Exe  # noqa: E402

# (motion id, record table, the table's first id, the first motion in the .anm, model, anm, output folder, output name)
MOTIONS = [
    (0xd49, 0x6e7728, 3400, 0xd49, 'chr/mar/hhh_mar.mdl', 'demo/mar_hagureru/hhh_mar.anm', 'mar', 'hhh_mar_s3'),
    # Angela at the grave: 0x59fd30 with the file list 0x8ea4a4, 0x5407c0 (first id 0xf3c: the records' first is empty)
    (0xf3d, 0x6e5c20, 0xf3c, 0xf3d, 'chr/agl/agl.mdl', 'demo/haka_agl/agl.anm', 'agl', 'agl_grave_1'),
    (0xf3e, 0x6e5c20, 0xf3c, 0xf3d, 'chr/agl/agl.mdl', 'demo/haka_agl/agl.anm', 'agl', 'agl_grave_2'),
    # Maria in the labyrinth cell: 0x582bd0 (stage 34, room 0x89) with the file list 0x8d4380, 0x540cf0; the
    # file starts at 0xd4a, the scene's own motion
    (0xd4b, 0x6e7728, 3400, 0xd4a, 'chr/mar/hhh_mar.mdl', 'demo/mar_ikiteruka/hhh_mar.anm', 'mar', 'hhh_mar_cell'),
    # Maria waiting in the park: EvProgMariaMeetingStandby 0x592830 with the file list 0x8e0938, 0x540cf0
    (0xd4d, 0x6e7728, 3400, 0xd4d, 'chr/mar/hhh_mar.mdl', 'demo/mar_deai/hhh_mar.anm', 'mar', 'hhh_mar_park'),
    # Laura on the wall with the letter: 0x593d70 (stage 14) with the file list 0x8e0ff8, 0x540ab0 and 0x53b8a0
    (0xb55, 0x6e696c, 0xb54, 0xb55, 'chr/lau/lau.mdl', 'demo/hei_lau/lau.anm', 'lau', 'lau_wall'),
    (0x282b, 0x6db230, 0x282a, 0x282b, 'chr/item/i_letter.mdl', 'demo/hei_lau/i_letter.anm', 'item', 'i_letter_wall'),
    # Eddie, 0x540650: by the toilet (0x59d120, stage 6 room 0x1e, list 0x8e674c), with the pizza in the bowling
    # alley (0x591dd0, stage 15 room 0xb, list 0x8ddb58, the pizza by 0x53b8a0), and dead (0x57f480, stage 37
    # room 0x90, list 0x8d0128)
    (0x12c1, 0x6e4f58, 0x12c0, 0x12c1, 'chr/edi/hhh_edi.mdl', 'demo/gero_edi/hhh_edi.anm', 'edi', 'hhh_edi_toilet'),
    (0x12c2, 0x6e4f58, 0x12c0, 0x12c2, 'chr/edi/hhh_edi.mdl', 'demo/bp_edi_2/hhh_edi.anm', 'edi', 'hhh_edi_bowl'),
    (0x282d, 0x6db230, 0x282a, 0x282d, 'chr/item/b_piz.mdl', 'demo/bp_edi_2/b_piz.anm', 'item', 'b_piz_bowl'),
    (0x12c3, 0x6e4f58, 0x12c0, 0x12c3, 'chr/edi/hhh_edi.mdl', 'demo/killer_edi_e/hhh_edi.anm', 'edi', 'hhh_edi_dead'),
]

# (model, anm, output folder, output name): the whole .anm
WHOLE = [
    ('chr/boat/boat.mdl', 'chr/boat/boat.anm', 'boat', 'boat'),
    ('chr/jms/hll_jms.mdl', 'chr/jms/jms_boat.anm', 'jms', 'jms_boat'),
]


def write_s3a(m, frames, path):
    out = [struct.pack('<4sIII', b'S3A1', 1, m.bone_count, len(frames))]
    for fr in frames:
        for e in fr:
            rot, trans = (e[0], e[1]) if e else (None, None)
            flags = (1 if rot is not None else 0) | (2 if trans is not None else 0)
            r = [int(round(x * 4096)) for x in rot] if rot is not None else [0, 0, 0]
            t = trans if trans is not None else (0.0, 0.0, 0.0)
            out.append(struct.pack('<3hH3f', *r, flags, *t))
    with open(path, 'wb') as f:
        f.write(b''.join(out))


def convert(exe, data, pack, anim, table, base, first, mdl, anm, folder, name):
    m = sh2mdl.load_mdl(os.path.join(data, mdl))
    d = ee.read(os.path.join(data, anm))
    roots = list(m.parents == 255)
    off, rec = 0, None
    for i in range(first - base, anim - base + 1):   # the file's motions before it, back to back from 0
        rec = struct.unpack('<6h', exe.read(table + 12 * i, 12))
        if rec[0] != base + i or rec[1] <= 0:
            print('%s: no motion record for %#x' % (name, base + i))
            return False
        offs = anm_frames(d, off, rec[1], m.bone_count, roots)
        if offs is None:
            print('%s: motion %#x runs off the end of %s' % (name, base + i, anm))
            return False
        if i < anim - base:
            off = offs[-1] + (offs[-1] & 1)
    # the motion's bytes on their own, decoded by sh2mdl from their first frame
    with tempfile.NamedTemporaryFile(suffix='.anm', delete=False) as t:
        t.write(d[offs[0]:offs[-1]])
    try:
        frames = sh2mdl.load_anm(t.name, m)
    finally:
        os.unlink(t.name)
    # a record's start and end can count from the first motion of the file: 0xf3e's are 64 and 155 of its 92
    shift = rec[3] if rec[4] >= len(frames) else 0
    first, last = max(0, rec[3] - shift), max(0, min(rec[4] - shift, len(frames) - 1))
    frames = frames[first:last + 1] or frames[:1]
    out = os.path.join(pack, 'chr', folder, name + '.s3a')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_s3a(m, frames, out)
    print('%s: motion %#x, %d frames (loop %d) -> %s' % (name, anim, len(frames), rec[5], out))
    return True


def convert_whole(data, pack, mdl, anm, folder, name):
    m = sh2mdl.load_mdl(os.path.join(data, mdl))
    frames = sh2mdl.load_anm(os.path.join(data, anm), m)
    out = os.path.join(pack, 'chr', folder, name + '.s3a')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    write_s3a(m, frames, out)
    print('%s: %d frames -> %s' % (anm, len(frames), out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--out', default=DEFAULT_OUT)
    a = ap.parse_args()
    exe = Exe(a.exe)
    for anim, table, base, first, mdl, anm, folder, name in MOTIONS:
        convert(exe, a.data, a.out, anim, table, base, first, mdl, anm, folder, name)
    for mdl, anm, folder, name in WHOLE:
        convert_whole(a.data, a.out, mdl, anm, folder, name)


if __name__ == '__main__':
    main()
