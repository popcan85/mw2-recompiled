#!/usr/bin/env python3
"""Packs mw2-setup (installer/main.cpp) with the files it installs.

    tools/make_setup.py <mw2-setup.exe> <out.exe> <file>[=<name>]...

Each file is carried under its own name, or the name after '='. A file named
.env is merged into the .env of the folder setup runs in rather than written
over it. The payload follows the program: per file its name's length (4
bytes), its name (UTF-8), its size (8 bytes) and its bytes; then where the
first file starts (8 bytes) and b"MW2SETUP", all little-endian.
"""
import os
import shutil
import struct
import sys


def main(argv):
    if len(argv) < 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    setup, out = argv[1], argv[2]
    shutil.copyfile(setup, out)
    with open(out, "ab") as f:
        start = f.tell()
        for spec in argv[3:]:
            path, _, name = spec.partition("=")
            name = name or os.path.basename(path)
            with open(path, "rb") as src:
                data = src.read()
            encoded = name.encode("utf-8")
            f.write(struct.pack("<I", len(encoded)))
            f.write(encoded)
            f.write(struct.pack("<Q", len(data)))
            f.write(data)
            print(f"  {name}: {len(data):,} bytes")
        f.write(struct.pack("<Q", start))
        f.write(b"MW2SETUP")
    print(f"{out}: {os.path.getsize(out):,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
