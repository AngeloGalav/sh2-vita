#!/usr/bin/env python3
"""Reader for SH2 PC message files (data/etc/message/*.mes).

Layout: u16 count, u16 offset[count] (in 16-bit words from the file start). A message runs to the
next offset and holds one or more pages. Each page starts on a 16-bit boundary with control words
(0x1000, 0x8000, 0x9000: display style, the low byte is 0), then byte-coded text read as 0x47f400
reads it after 0x8000: a byte b < 0xe0 is the font code b, the Windows-1252 character
chr(b + 0x20) (so 0x00 is a space and the accents of the other languages come through); e0 lo hi
is the full-width code hi << 8 | lo (the titles' letters); a byte b > 0xe0 starts the two-byte
code b << 8 | next: ff fd newline, ff ff end of page, ff fe a full-width space, fc nn / fd nn move
the pen back / on by nn (the titles' kerning), ff f9 a newline without a height of its own. Pages are
joined with \f. A full-width code (and the kerning) becomes FULL_WIDTH and the code as four hex
digits, which port/source/font.c draws.

With marks, the display codes of 0x47ff50 stay, as MARK and a letter (port/source/main.c
draw_message_top; font.c skips them): ff fa 'v' the block centred on the screen's middle, ff fb 'h'
each line centred, f8 nn / f9 nn 'y' and three hex digits the first line's height, ff 0n 'c' and a
hex digit the colour of 0x47eca0, ff f7 'b' the ends of a blotted-out part. An ff fb alone, as the
memos' titles have it, is dropped, and so are the other codes.
"""
import re
import struct
import sys

import ee

FULL_WIDTH = '\x06'
MARK = '\x07'


def full_width(code):
    return '%s%04x' % (FULL_WIDTH, code)


def _is_control(d, p):
    return p + 1 < len(d) and d[p] == 0 and d[p + 1] and not d[p + 1] & 0x0f


def load(path, marks=False):
    """A .mes file; the Enhanced Edition's version when the player has it (ee.py)."""
    return ee.load(path, lambda p: _load(p, marks))


def _page(s, head):
    """A page's text; its marks alone are no page."""
    text = ''.join(s).strip()
    if 'h' in head and len(head) == 1 and MARK + 'c' not in text:
        head = ''
    if not re.sub(MARK + '(c.|.)', '', text).strip():
        return ''
    return ''.join(MARK + c for c in head) + text


def _load(path, marks=False):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<H', d, 0)[0]
    offs = list(struct.unpack_from(f'<{n}H', d, 2))
    out = []
    for i, o in enumerate(offs):
        p, end = o * 2, offs[i + 1] * 2 if i + 1 < n else len(d)
        pages, s, head = [], [], []
        while p < end:
            if not s:
                p += p & 1
                while _is_control(d, p):
                    p += 2
                if p >= end:
                    break
            b = d[p]
            if b == 0xe0 and p + 2 < end:
                s.append(full_width(d[p + 1] | d[p + 2] << 8))
                p += 3
                continue
            if b > 0xe0 and p + 1 < end:
                c = d[p + 1]
                p += 2
                if b in (0xfc, 0xfd):
                    s.append(full_width(b << 8 | c))
                if marks and b in (0xf8, 0xf9):
                    head.append('y%03x' % ((b & 1) << 8 | c))
                if b != 0xff:
                    continue
                if c == 0xff:
                    pages.append(_page(s, head))
                    s, head = [], []
                elif c in (0xfd, 0xf9):
                    s.append('\n')
                elif c == 0xfe:
                    s.append(full_width(0xfffe))
                elif 0xe0 <= c <= 0xe3:
                    s.append(chr(1 + c - 0xe0))     # inserted value 0..3 (MesSetInsert 0x47fd70)
                elif c == 0xf6:
                    s.append('\x05')               # yes / no question follows
                elif marks and c in (0xfa, 0xfb):
                    head.append('v' if c == 0xfa else 'h')
                elif marks and c == 0xf7:
                    s.append(MARK + 'b')
                elif marks and c < 0x10:
                    s.append(MARK + 'c%x' % c)
                continue
            s.append(chr(b + 0x20) if b < 0xe0 else '?')
            p += 1
        if s:
            pages.append(_page(s, head))
        out.append('\f'.join(x for x in pages if x))
    return out


if __name__ == '__main__':
    for i, m in enumerate(load(sys.argv[1])):
        print(i, repr(m))
