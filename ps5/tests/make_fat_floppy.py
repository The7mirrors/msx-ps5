# Test floppy for MSX PS5's "games on a floppy": a 1.44 MB FAT12 image made by mkfs.fat, with files added the way
# Windows writes them (8.3 entry + VFAT long-name entries, a subfolder, a deleted entry, a file spanning many
# non-contiguous clusters). Usage: make_fat_floppy.py out.img  -> prints the expected listing + sha of each file.
import hashlib, os, struct, subprocess, sys

img = sys.argv[1]
if os.path.exists(img):
    os.remove(img)
subprocess.check_call(['/usr/sbin/mkfs.fat', '-C', '-F', '12', '-n', 'MSXGAMES', img, '1440'], stdout=subprocess.DEVNULL)
d = bytearray(open(img, 'rb').read())
bps, spc, res, nfats, nroot = struct.unpack_from('<HBHBH', d, 11)
spf = struct.unpack_from('<H', d, 22)[0]
root = (res + nfats * spf) * bps
data = root + nroot * 32
cbytes = spc * bps

def fat_set(c, v):
    for f in range(nfats):
        base = (res + f * spf) * bps
        off = base + c + c // 2
        cur = d[off] | d[off + 1] << 8
        cur = (cur & 0x000F) | (v << 4) if c & 1 else (cur & 0xF000) | v
        d[off], d[off + 1] = cur & 0xFF, cur >> 8

next_free = [2]
def alloc(n, gap=0):
    cl = []
    for _ in range(n):
        cl.append(next_free[0])
        next_free[0] += 1 + gap  # gap: leave clusters free in between (a fragmented file)
    for a, b in zip(cl, cl[1:] + [0xFFF]):
        fat_set(a, b)
    return cl

def write_data(clusters, blob):
    for i, c in enumerate(clusters):
        chunk = blob[i * cbytes:(i + 1) * cbytes]
        o = data + (c - 2) * cbytes
        d[o:o + len(chunk)] = chunk

def short_name(name, n):
    base, _, ext = name.upper().rpartition('.')
    base = ''.join(ch for ch in base if ch.isalnum())[:6] + '~%d' % n
    return (base.ljust(8) + ext[:3].ljust(3)).encode('ascii')

def lfn_entries(long, sn):
    s = 0
    for b in sn:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    u = [ord(ch) for ch in long] + [0]
    while len(u) % 13:
        u.append(0xFFFF)
    parts = [u[i:i + 13] for i in range(0, len(u), 13)]
    out = []
    for seq, p in enumerate(parts, 1):
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == len(parts) else 0)
        for k, off in enumerate([1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]):
            struct.pack_into('<H', e, off, p[k])
        e[11], e[13] = 0x0F, s
        out.append(bytes(e))
    return list(reversed(out))  # the last piece first, as on disk

def entry(sn, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = sn
    e[11] = attr
    struct.pack_into('<HI', e, 26, cluster, size)
    return bytes(e)

def add(dir_off, slot, long, attr, cluster, size, n):
    sn = short_name(long, n)
    ents = lfn_entries(long, sn) + [entry(sn, attr, cluster, size)]
    for e in ents:
        d[dir_off + slot * 32:dir_off + slot * 32 + 32] = e
        slot += 1
    return slot

def blob(seed, size):
    out = bytearray()
    x = seed
    while len(out) < size:
        x = (x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        out += x.to_bytes(8, 'little')
    return bytes(out[:size])

expect = []
slot = 1  # (slot 0: the volume label mkfs wrote)
files = [('Knightmare (Japan).rom', 32768, 0), ('Zanac (Japan).rom', 131072, 3), ('Metal Gear (Japan) (En).zip', 70000, 0),
         ('readme.txt', 900, 0)]
for i, (name, size, gap) in enumerate(files, 1):
    b = blob(i, size)
    cl = alloc((size + cbytes - 1) // cbytes, gap)
    write_data(cl, b)
    slot = add(root, slot, name, 0x20, cl[0], size, i)
    expect.append((name, size, hashlib.sha256(b).hexdigest()[:16]))
# a deleted file's entries (must not show)
d[root + slot * 32] = 0xE5
d[root + slot * 32 + 11] = 0x20
slot += 1
# a folder with one game
dcl = alloc(1)
slot = add(root, slot, 'More games', 0x10, dcl[0], 0, 9)
sub = data + (dcl[0] - 2) * cbytes
d[sub:sub + 32] = entry(b'.          ', 0x10, dcl[0], 0)
d[sub + 32:sub + 64] = entry(b'..         ', 0x10, 0, 0)
b = blob(42, 49152)
cl = alloc(49152 // cbytes)
write_data(cl, b)
add(sub, 2, 'Penguin Adventure (Japan).rom', 0x20, cl[0], 49152, 1)
expect.append(('More games/Penguin Adventure (Japan).rom', 49152, hashlib.sha256(b).hexdigest()[:16]))
open(img, 'wb').write(d)
for e in expect:
    print('%s|%d|%s' % e)
