# make_pack.py : the MSX PS5 shelf's sound effects (CC0 sources, see ../CREDITS.txt), cut, levelled and converted to
# 48 kHz 16-bit stereo WAV in ../pack/, plus ../pack/_preview.wav (every sound in a row, to listen to on the PC).
# Runs in WSL (decodes with /tmp/ogg2raw, built from tools/ogg2raw.c + stb_vorbis).
import array, math, os, struct, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "pack")
os.makedirs(OUT, exist_ok=True)
RATE = 48000

# name: (source, start s, end s, peak level, fade-out s)
K = os.path.join(ROOT, "kenney", "Audio")
D = os.path.join(ROOT, "downloads")
SOUNDS = {
    "browse": (os.path.join(K, "tick_002.ogg"), None, None, 0.35, 0.0),          # left / right on the shelf
    "menu_move": (os.path.join(K, "click_002.ogg"), None, None, 0.30, 0.0),      # up / down in a menu
    "menu_ok": (os.path.join(K, "select_002.ogg"), None, None, 0.40, 0.0),       # change a setting / choose
    "menu_back": (os.path.join(K, "back_002.ogg"), None, None, 0.40, 0.0),       # back
    "open": (os.path.join(K, "toggle_001.ogg"), None, None, 0.32, 0.0),            # a menu opens
    "close": (os.path.join(K, "toggle_004.ogg"), None, None, 0.32, 0.0),          # a menu closes
    "favorite": (os.path.join(K, "confirmation_002.ogg"), None, None, 0.40, 0.0),
    "shutter_slide": (os.path.join(K, "scratch_004.ogg"), 0.190, 0.300, 0.32, 0.02, -3),  # (its last third, 3 semitones down: the user's pick)  # the floppy's metal shutter slides
    "shutter_snap": (os.path.join(K, "tick_004.ogg"), None, None, 0.35, 0.0),     # ... and springs shut
    "cart_clack": (os.path.join(D, "fs723363_cartridge.ogg"), 1.52, 2.10, 0.80, 0.08),   # the cartridge goes in
    "cart_clack_b": (os.path.join(D, "fs677386_gba_cartridge.ogg"), 3.20, 3.85, 0.80, 0.08),
    "floppy_insert": (os.path.join(D, "fs636260_floppy_insert.ogg"), 1.30, 2.75, 0.85, 0.15),  # slide in + latch
    "floppy_seek": (os.path.join(D, "fs39699_floppy_read.ogg"), 1.95, 3.40, 0.75, 0.40),       # the head steps
}


def decode(path):
    ch, rate, frames = map(int, subprocess.check_output(["/tmp/ogg2raw", path, "/tmp/pack.raw"]).split())
    a = array.array("h")
    a.frombytes(open("/tmp/pack.raw", "rb").read())
    left = [a[i * ch] / 32768 for i in range(frames)]
    right = [a[i * ch + (1 if ch > 1 else 0)] / 32768 for i in range(frames)]
    return rate, left, right


def resample(x, src, dst):
    if src == dst:
        return x
    n = int(len(x) * dst / src)
    out = []
    for i in range(n):
        p = i * src / dst
        j = int(p)
        f = p - j
        a = x[j] if j < len(x) else 0.0
        b = x[j + 1] if j + 1 < len(x) else a
        out.append(a + (b - a) * f)
    return out


def make(name, spec):
    path, t0, t1, level, fade = spec[:5]
    semis = spec[5] if len(spec) > 5 else 0  # pitch shift (slower = lower)
    rate, l, r = decode(path)
    if t0 is not None:
        l, r = l[int(t0 * rate):int(t1 * rate)], r[int(t0 * rate):int(t1 * rate)]
    src_rate = rate * 2 ** (semis / 12)
    l, r = resample(l, src_rate, RATE), resample(r, src_rate, RATE)
    # trim leading / trailing silence, a 3 ms fade-in, the fade-out, then level the peak
    thr = 0.01 * max(max(abs(v) for v in l), max(abs(v) for v in r))  # 1% of its own peak
    s = next((i for i in range(len(l)) if abs(l[i]) > thr or abs(r[i]) > thr), 0)
    e = next((i for i in range(len(l) - 1, -1, -1) if abs(l[i]) > thr or abs(r[i]) > thr), len(l) - 1) + 1
    l, r = l[max(0, s - 48):e], r[max(0, s - 48):e]
    n = len(l)
    fi, fo = 144, int(fade * RATE)
    for i in range(n):
        g = min(1.0, i / fi) * (min(1.0, (n - i) / fo) if fo else 1.0)
        l[i] *= g
        r[i] *= g
    peak = max(max(abs(v) for v in l), max(abs(v) for v in r)) or 1
    k = level / peak
    return [v * k for v in l], [v * k for v in r]


def write_wav(path, l, r):
    data = array.array("h", [int(max(-1, min(1, v)) * 32767) for pair in zip(l, r) for v in pair])
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data) * 2) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(data) * 2))
        f.write(data.tobytes())


pl, pr = [], []
for name, spec in SOUNDS.items():
    l, r = make(name, spec)
    write_wav(os.path.join(OUT, name + ".wav"), l, r)
    print(f"{name:14s} {len(l) / RATE:5.2f} s  <- {os.path.basename(spec[0])}")
    pl += l + [0.0] * int(0.6 * RATE)
    pr += r + [0.0] * int(0.6 * RATE)
write_wav(os.path.join(OUT, "_preview.wav"), pl, pr)
print(f"_preview.wav {len(pl) / RATE:.1f} s (the sounds in the order above)")
