#!/usr/bin/env python3
"""Copy your sound banks into Vault's Content/Banks folder.

Usage (from the SDK folder):
    python plugins/Vault/tools/setup_sounds.py "path/to/emu_sounds.zip"

It accepts the big zip that holds one "<Bank>_WAV.zip" per bank, a folder of those
bank zips, or a folder of already-unzipped bank folders. Each bank ends up in
plugins/Vault/Content/Banks/<Bank>_WAV/ with its WAV files, which is where
Content/vault.map expects them. Only .wav files are copied. Nothing is deleted.
"""
import io
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
BANKS = os.path.normpath(os.path.join(HERE, "..", "Content", "Banks"))


def bank_name(path):
    return os.path.splitext(os.path.basename(path.rstrip("/\\")))[0]


def copy_wavs_from_zip(zf, dest):
    n = 0
    os.makedirs(dest, exist_ok=True)
    for info in zf.infolist():
        name = os.path.basename(info.filename)   # flatten; never write outside dest
        if info.is_dir() or not name.lower().endswith(".wav") or name.startswith("."):
            continue
        with zf.open(info) as src, open(os.path.join(dest, name), "wb") as out:
            shutil.copyfileobj(src, out)
        n += 1
    return n


def from_bank_zip(zf, label):
    dest = os.path.join(BANKS, bank_name(label))
    n = copy_wavs_from_zip(zf, dest)
    print("  %-40s %5d WAVs" % (bank_name(label), n))
    return n


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    os.makedirs(BANKS, exist_ok=True)
    total = 0
    print("Copying sounds into", BANKS)
    if os.path.isfile(src) and src.lower().endswith(".zip"):
        with zipfile.ZipFile(src) as outer:
            inner = [i for i in outer.infolist() if i.filename.lower().endswith(".zip")]
            if inner:
                for i in inner:
                    with zipfile.ZipFile(io.BytesIO(outer.read(i))) as zf:
                        total += from_bank_zip(zf, i.filename)
            else:   # a single bank zip
                total += from_bank_zip(outer, src)
    elif os.path.isdir(src):
        for entry in sorted(os.listdir(src)):
            p = os.path.join(src, entry)
            if entry.lower().endswith(".zip"):
                with zipfile.ZipFile(p) as zf:
                    total += from_bank_zip(zf, entry)
            elif os.path.isdir(p):
                dest = os.path.join(BANKS, entry)
                os.makedirs(dest, exist_ok=True)
                n = 0
                for f in os.listdir(p):
                    if f.lower().endswith(".wav"):
                        shutil.copy2(os.path.join(p, f), os.path.join(dest, f))
                        n += 1
                print("  %-40s %5d WAVs" % (entry, n))
                total += n
    else:
        print("Not a zip file or folder:", src)
        return 1
    print("Done: %d WAV files. Now build:  python tools/build.py plugins/Vault" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
