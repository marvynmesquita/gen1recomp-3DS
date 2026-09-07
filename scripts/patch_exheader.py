import struct
import sys

if len(sys.argv) < 2:
    print("Usage: patch_exheader.py <cia_file>")
    sys.exit(1)

cia_path = sys.argv[1]

with open(cia_path, 'rb') as f:
    cia = bytearray(f.read())

# CIA header 0x2020 + cert 0xa00 + ticket 0x350 + tmd 0xb34 + ncch 0x200 = exheader offset
exheader_off = 0x2020 + 0xa00 + 0x350 + 0xb34 + 0x200
fs_off = exheader_off + 0x1d8

val = struct.unpack('<I', cia[fs_off:fs_off+4])[0]
cia[fs_off:fs_off+4] = struct.pack('<I', val | 0x01)

with open(cia_path, 'wb') as f:
    f.write(cia)

print(f'Patched {cia_path}: DirectSdmc enabled (fs_access_info=0x01)')
