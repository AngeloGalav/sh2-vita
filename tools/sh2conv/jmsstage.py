#!/usr/bin/env python3
"""James' per-stage damage motions.

jmsstage.py <game data dir> <pack dir> <sh2pc.exe>

Each stage loads a jms_stg_*.anm over the weapon's set, its frames counted from 1024 (0x491e60, file table
0x88e138 by stage id), and names its clips in a list at Stage_Data +0x2c: {id, length, speed, first, last,
flags} x n, ending at id 0. 0x534700 picks the ids. Writes chr/jms/<anm>.s3a and chr/jms/stgmot.txt, a line
for each stage ("s <stage> <anm>") followed by its clips ("<id> <speed> <first> <last> <flags>").
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chr as chrconv
import sh2mdl
from pcexe import Exe

STAGES = os.path.join(HERE, '..', '..', 'sh2-3ds-port-kit', 'data', 'dc', 'stage_table.json')
ANM_FILES = 0x88e138
MOTIONS = 0x2c


def main():
    data, pack, exe_path = sys.argv[1:4]
    exe = Exe(exe_path)
    src = os.path.join(data, 'chr', 'jms')
    out = os.path.join(pack, 'chr', 'jms')
    os.makedirs(out, exist_ok=True)
    model = sh2mdl.load_mdl(os.path.join(src, 'hll_jms.mdl'))
    done, lines = {}, []
    for stage in json.load(open(STAGES)):
        sid = stage['id']
        name_ptr = exe.u32(ANM_FILES + 4 * sid)
        table = exe.u32(int(stage['addr'], 16) + MOTIONS)
        if not name_ptr or not table:
            continue
        rel = exe.read(exe.u32(name_ptr), 48).split(b'\0')[0].decode()
        anm = os.path.basename(rel)
        path = os.path.join(src, anm)
        if not os.path.isfile(path):
            continue
        base = os.path.splitext(anm)[0]
        if base not in done:
            done[base] = chrconv.convert_anm(model, path, os.path.join(out, base + '.s3a'))
        lines.append('s %d %s' % (sid, base))
        while True:
            mid, _, speed, first, last, flags = struct.unpack('<6H', exe.read(table, 12))
            if not mid:
                break
            lines.append('%d %d %d %d %d' % (mid, speed, first, last, flags))
            table += 12
    with open(os.path.join(out, 'stgmot.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('jmsstage: %d files, %d frames, %d lines' % (len(done), sum(done.values()), len(lines)))


if __name__ == '__main__':
    main()
