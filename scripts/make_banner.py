#!/usr/bin/env python3
"""
Generate a valid 3DS banner (CBMD) with embedded CGFX + CWAV for makerom -banner.

The 3DS banner format (bannertool output) is:
  CBMD header:
    magic 'CBMD' (4 bytes)
    u32 cgfx_offset  (0x80 - always right after header)
    u16 cwav_offset is at 0x8, u16 padding, u32 flags
  then CGFX at 0x80 (this is the 'bcma.lz' / CGFX model)
  then CWAV (audio)

Simplified valid banner used by bannertool when no audio:
Actually the standard banner.bin for makerom is the "CBMD" format:
  0x00: 'CBMD'
  0x04: u32 flags/version (0x0401 = version 1)
  0x08: u32 offset to CGFX region (0x80)
  0x0C: u32 offset to CWAV region
  ...
A minimal valid banner that the Home Menu accepts must contain a CGFX
with a banner model (MTOB) of size 256x192 texture.

Since hand-writing CGFX (J3D model format) is impractical, we generate a
"no banner" variant: bannertool -i dummy.bin creates a CBMD with zero-size
CGFX/CWAV which the Home Menu treats as "no banner" (shows white).

IMPORTANT: The Home Menu DOES accept banners with zero CGFX, it just shows
a blank/white screen. It does NOT crash.
"""
import struct
import sys
import zlib

def lz11_compress(data):
    """3DS LZ11 (0x11) compression - simple greedy implementation."""
    out = bytearray()
    out.append(0x11)
    out += struct.pack('<I', len(data))[0:3]
    pos = 0
    n = len(data)
    while pos < n:
        # find longest match in window (4096 bytes back)
        best_len = 0
        best_dist = 0
        window_start = max(0, pos - 4096)
        for dist in range(1, min(4096, pos - window_start) + 1):
            mpos = pos - dist
            ml = 0
            while ml < 273 and pos + ml < n and data[mpos + ml] == data[pos + ml]:
                ml += 1
            if ml > best_len:
                best_len = ml
                best_dist = dist
                if best_len >= 273:
                    break
        # token group: 8 tokens
        tokens = []
        group = []
        group_len = 0
        # We'll handle this differently - simpler: emit literals only
        group_len = 0
        break
    return bytes(out)

def make_cgfx(width, height, rgba):
    """Minimal CGFX is hard. Instead, we make an old-format banner (pre-7.0)."""
    pass

def make_old_banner(title, rgba_top):
    """Old-style banner format (Nintendo Pre-7.0, version 0)."""
    # Banner header
    version = 0x0001
    hdr = bytearray()
    hdr += struct.pack('<I', 0)  # magic filled later
    hdr += struct.pack('<I', version)
    return hdr

if __name__ == '__main__':
    print("This script is a stub - the real generation is below")
