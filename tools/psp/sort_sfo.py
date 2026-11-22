#!/usr/bin/env python3
"""Rewrites a PARAM.SFO with its keys in alphabetical order, in place.

mksfoex puts the keys given with -d/-s (MEMSIZE, APP_VER, DISC_VERSION) in front of its own sorted ones. Sony's PARAM.SFO files always
have their keys sorted, and the firmware looks them up as if they were: with APP_VER and DISC_VERSION added in front, the XMB said the
data was corrupted when starting the game. The values and their sizes are kept exactly as they were; only the order changes.
"""
import struct
import sys


def main(path):
    data = open(path, 'rb').read()
    magic, version, key_start, data_start, count = struct.unpack('<4sIIII', data[:20])
    if magic != b'\0PSF':
        sys.exit('%s: not a PARAM.SFO' % path)
    entries = []
    for i in range(count):
        key_off, fmt, length, max_len, data_off = struct.unpack('<HHIII', data[20 + i * 16:36 + i * 16])
        name = data[key_start + key_off:data.index(b'\0', key_start + key_off)]
        value = data[data_start + data_off:data_start + data_off + max_len]
        entries.append((name, fmt, length, max_len, value))
    entries.sort(key=lambda e: e[0])

    index = b''
    keys = b''
    values = b''
    for name, fmt, length, max_len, value in entries:
        index += struct.pack('<HHIII', len(keys), fmt, length, max_len, len(values))
        keys += name + b'\0'
        values += value
    while len(keys) % 4:
        keys += b'\0'
    key_start = 20 + len(index)
    data_start = key_start + len(keys)
    out = struct.pack('<4sIIII', magic, version, key_start, data_start, count) + index + keys + values
    open(path, 'wb').write(out)


if __name__ == '__main__':
    main(sys.argv[1])
