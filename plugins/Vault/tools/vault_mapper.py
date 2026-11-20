#!/usr/bin/env python3
"""Make Content/vault.map from the WAV folders in Content/Banks (needs numpy).

Usage (from the SDK folder):
    python plugins/Vault/tools/vault_mapper.py

You only need this for NEW sounds: Vault ships with a map for the banks it was set up with.
Each sub-folder of Content/Banks becomes a Bank. Files are taken in name order and grouped
into Sounds automatically:
  * pitched samples (detected with the YIN pitch detector) that sit next to each other and
    do not repeat a pitch become one multisampled instrument, spread across the keyboard;
  * unpitched one-shots become drum kits, one sample per key from C1 (note 36) up.
Loop points are read from each WAV's 'smpl' chunk. Grouping is a best guess: open
vault.map in a text editor to rename sounds (the 'I' lines) or fix key ranges.
"""
import json, os, re, struct, sys
from concurrent.futures import ProcessPoolExecutor
try:
    import numpy as np
except ImportError:
    sys.exit("vault_mapper needs numpy:  pip install numpy")

HERE = os.path.dirname(os.path.abspath(__file__))
CONTENT = os.path.normpath(os.path.join(HERE, "..", "Content"))
BANKS = os.path.join(CONTENT, "Banks")
KIT_START, KIT_MAX, MAX_ZONES = 36, 61, 32


def read_wav(path):
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        return None
    p, info, x = 12, {"loop": None}, None
    while p + 8 <= len(d):
        cid, sz = d[p:p + 4], struct.unpack("<I", d[p + 4:p + 8])[0]
        body = d[p + 8:p + 8 + sz]
        if cid == b"fmt ":
            fmt, ch, sr, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            info.update(ch=ch, sr=sr, bits=bits, fmt=fmt)
        elif cid == b"smpl" and len(body) >= 36:
            n = struct.unpack("<I", body[28:32])[0]
            if n and len(body) >= 60:
                l = struct.unpack("<6I", body[36:60])
                info["loop"] = (l[2], l[3])
        elif cid == b"data" and "bits" in info:
            ch, bits = info["ch"], info["bits"]
            if bits == 16:
                x = np.frombuffer(body[:len(body) // 2 * 2], "<i2").astype(np.float32) / 32768
            elif bits == 24:
                b = np.frombuffer(body[:len(body) // 3 * 3], np.uint8).reshape(-1, 3).astype(np.int32)
                x = ((b[:, 0] << 8 | b[:, 1] << 16 | b[:, 2] << 24) >> 8).astype(np.float32) / 8388608
            elif bits == 32 and info["fmt"] == 3:
                x = np.frombuffer(body[:len(body) // 4 * 4], "<f4").astype(np.float32)
            elif bits == 8:
                x = (np.frombuffer(body, np.uint8).astype(np.float32) - 128) / 128
            else:
                return None
            if ch > 1:
                x = x[:len(x) // ch * ch].reshape(-1, ch).mean(1)
        p += 8 + sz + (sz & 1)
    if x is None or len(x) == 0:
        return None
    info["x"] = x
    return info


def yin(x, sr, fmin=27.5, fmax=4200):
    W = min(len(x) // 2, 4096)
    tmax, tmin = min(int(sr / fmin), W - 1), max(2, int(sr / fmax))
    if W < 64 or tmax <= tmin + 2 or len(x) < W + tmax:
        return None, 0.0
    d = np.array([np.sum((x[:W] - x[t:t + W]) ** 2) for t in range(tmax + 1)])
    cm = np.concatenate([[1], d[1:] * np.arange(1, tmax + 1) / np.maximum(np.cumsum(d[1:]), 1e-12)])
    cand = np.where(cm[tmin:] < 0.15)[0]
    t = tmin + (cand[0] if len(cand) else int(np.argmin(cm[tmin:])))
    while len(cand) and t + 1 < len(cm) and cm[t + 1] < cm[t]:
        t += 1
    a, b, c = cm[t - 1], cm[t], cm[min(t + 1, len(cm) - 1)]
    den = a - 2 * b + c
    off = 0.5 * (a - c) / den if den else 0.0
    return sr / (t + off), float(1 - cm[t])


def analyze(path):
    w = read_wav(path)
    if w is None:
        return None
    x, sr, loop = w["x"].astype(np.float64), w["sr"], w["loop"]
    n = len(x)
    if loop and loop[1] < n and loop[1] - loop[0] > 2048:
        seg = x[max(0, loop[0] - 2048):loop[1]]
    elif loop and loop[1] < n:
        seg = x[int(n * 0.2):]
    else:
        loop = None if loop else None
        seg = x[int(min(n * 0.15, sr * 0.08)):]
    seg = seg[:sr // 2]
    f, conf = yin(seg, sr) if len(seg) > 256 else (None, 0.0)
    midi = 69 + 12 * np.log2(f / 440) if f else None
    m = re.findall(r"\d+", os.path.basename(path))
    return dict(file=os.path.basename(path), num=int(m[-1]) if m else 0, dur=n / sr,
                loop=loop if (loop and loop[1] < n) else None, midi=float(midi) if midi else None, conf=conf)


def pitched(x):
    return x["midi"] is not None and x["conf"] >= 0.85 and 20 <= x["midi"] <= 110 and (x["loop"] or x["dur"] > 0.05)


def fits(z, x):
    if len(z) >= MAX_ZONES or any(abs(y["midi"] - x["midi"]) < 0.6 for y in z):
        return False
    if min(abs(y["midi"] - x["midi"]) for y in z) > 13 or bool(z[-1]["loop"]) != bool(x["loop"]):
        return False
    d = [y["dur"] for y in z]
    return min(d) / 8 <= x["dur"] <= max(d) * 8


def group(samples):
    groups = []
    for x in sorted(samples, key=lambda s: (s["num"], s["file"])):
        g = groups[-1] if groups else None
        if pitched(x):
            if g and g["type"] == "multi" and fits(g["z"], x): g["z"].append(x)
            else: groups.append({"type": "multi", "z": [x]})
        elif g and g["type"] == "kit" and len(g["z"]) < KIT_MAX: g["z"].append(x)
        else: groups.append({"type": "kit", "z": [x]})
    merged = []
    for g in groups:
        if g["type"] == "multi" and len(g["z"]) == 1 and not g["z"][0]["loop"]:
            g["type"] = "kit"   # a lone tuned hit (tom, 808) belongs with the drums
        if merged and g["type"] == "kit" and merged[-1]["type"] == "kit" and len(merged[-1]["z"]) + len(g["z"]) <= KIT_MAX:
            merged[-1]["z"] += g["z"]
        else:
            merged.append(g)
    return merged


def zone_line(x, root, lo, hi):
    ls, le = x["loop"] if x["loop"] else (-1, -1)
    return "Z\t%s\t%.2f\t%d\t%d\t%d\t%d" % (x["file"], root, lo, hi, ls, le)


def main():
    if not os.path.isdir(BANKS):
        sys.exit("No Content/Banks folder. Run setup_sounds.py first.")
    folders = sorted([f for f in os.listdir(BANKS) if os.path.isdir(os.path.join(BANKS, f))], key=str.lower)
    lines = ["# Vault sound map. B=bank (name, folder under Content/Banks), I=sound (name, kit 0/1), "
             "Z=zone (file, root note, low key, high key, loop start, loop end; -1 = no loop)"]
    total = 0
    with ProcessPoolExecutor() as ex:
        for folder in folders:
            files = [os.path.join(BANKS, folder, f) for f in os.listdir(os.path.join(BANKS, folder)) if f.lower().endswith(".wav")]
            samples = [s for s in ex.map(analyze, files, chunksize=32) if s]
            if not samples:
                continue
            name = re.sub(r"_WAV$", "", folder).replace("_", " ")
            lines.append("B\t%s\t%s" % (name, folder))
            for i, g in enumerate(group(samples), 1):
                z = g["z"]
                if g["type"] == "multi":
                    z = sorted(z, key=lambda s: s["midi"])
                    looped = sum(bool(s["loop"]) for s in z) > len(z) / 2
                    if len(z) == 1: kind = ("Wave" if z[0]["dur"] < 0.25 else "Loop") if looped else "Hit"
                    else: kind = ("Pad" if sum(s["dur"] for s in z) / len(z) > 1.0 else "Loop") if looped else "Multi"
                    lines.append("I\t%03d %s\t0" % (i, kind))
                    roots = [s["midi"] for s in z]
                    for j, s in enumerate(z):
                        lo = 0 if j == 0 else int(round((roots[j - 1] + roots[j]) / 2 + 0.001))
                        hi = 127 if j == len(z) - 1 else int(round((roots[j] + roots[j + 1]) / 2 + 0.001)) - 1
                        lines.append(zone_line(s, s["midi"], lo, max(lo, hi)))
                else:
                    lines.append("I\t%03d %s\t1" % (i, "Kit" if len(z) > 1 else "Hit"))
                    for j, s in enumerate(z):
                        lines.append(zone_line(s, KIT_START + j, KIT_START + j, KIT_START + j))
                total += 1
            print("  %-30s %4d sounds" % (name, sum(1 for l in lines if l.startswith("I\t")) - (total - len(group(samples)))))
    out = os.path.join(CONTENT, "vault.map")
    if os.path.exists(out):
        os.replace(out, out + ".bak")
        print("Previous map saved as vault.map.bak")
    open(out, "w").write("\n".join(lines) + "\n")
    print("Wrote %s: %d banks, %d sounds" % (out, sum(1 for l in lines if l.startswith("B\t")), total))


if __name__ == "__main__":
    main()
