#!/usr/bin/env python3
"""Byte-level helpers for qxltool Q40 tests. Stdlib only."""
import sys

def rd16(b, off): return (b[off] << 8) | b[off+1]
def rd32(b, off): return (rd16(b, off) << 16) | rd16(b, off+2)

def swab16(buf):
    """Transpose the two bytes of every 16-bit word (involution)."""
    if len(buf) % 2: raise ValueError("odd length")
    out = bytearray(buf)
    for i in range(0, len(out), 2):
        out[i], out[i+1] = out[i+1], out[i]
    return bytes(out)

def read_sector(path, lba, n=512):
    with open(path, "rb") as f:
        f.seek(lba * 512); return f.read(n)

def read_partition(path, start, size):
    """Return the full big-endian (un-swapped) .WIN content of a partition."""
    with open(path, "rb") as f:
        f.seek(start * 512)
        return swab16(f.read(size * 512))

def checksum_sector(b):
    return (sum(b[0:0x1FE]) + (~sum(b[0:0x1FE]) & 0xFFFF)) & 0xFFFF

if __name__ == "__main__":
    # tiny self-test: swap twice == identity
    data = bytes(range(0, 512))
    assert swab16(swab16(data)) == data
    print("self-test ok")
