#!/usr/bin/env python3
"""Write a minimal initramfs (newc cpio, gzipped) for the boot-smoke images.

`build/vmlinux_segs.o` embeds this file into the LoongArch harness, and
firmware/initramfs.cpio.gz is gitignored because it is a test asset rather than
source. A fresh checkout therefore had no way to build the SMP harness at all.

The archive only needs to be *valid*: the harness asserts translation counters
and reaches its stop marker with a kernel-only boot, so the image carries just
the root directory entry. A real userspace can still be placed at
firmware/initramfs.cpio.gz; this script refuses to overwrite an existing file.
"""
import gzip
import os
import stat
import sys


def _pad4(data):
    return data + b"\0" * ((4 - len(data) % 4) % 4)


def _entry(name, data, mode, ino):
    fields = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0]
    header = b"070701" + b"".join(b"%08X" % f for f in fields)
    return header + _pad4(name.encode() + b"\0") + _pad4(data)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: mk_initramfs.py <output.cpio.gz>\n")
        return 2
    out = argv[1]
    if os.path.exists(out) and os.path.getsize(out) != 0:
        sys.stderr.write("mk_initramfs: %s exists; leaving it alone\n" % out)
        return 0
    archive = _entry(".", b"", stat.S_IFDIR | 0o755, 1)
    archive += _entry("TRAILER!!!", b"", 0, 0)
    if os.path.dirname(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as fh:
        fh.write(gzip.compress(archive, 9, mtime=0))
    sys.stderr.write("mk_initramfs: wrote %s (%d bytes)\n" % (out, len(archive)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
