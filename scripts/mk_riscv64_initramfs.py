#!/usr/bin/env python3
"""Build a Linux initramfs containing a riscv64 static busybox and a tiny /init.

Why this exists: firmware/initramfs.cpio.gz is only asserted to be a *valid*
archive (see mk_initramfs.py), and firmware/busybox is an x86-64 binary used by
the host-side smoke tests. A RISC-V guest that reaches `Run /init as init
process` therefore gets ENOEXEC and panics, which makes "booted to userspace"
impossible to demonstrate.

    python3 scripts/mk_riscv64_initramfs.py <busybox-riscv64> <out.cpio.gz>

The busybox must be a *static riscv64* build (Alpine's busybox-static .apk
contains one at bin/busybox.static). The archive carries /init, /bin/busybox,
the applet symlinks init uses, and a /dev/console node, because the kernel opens
the console before it runs init and a missing node silently swallows the
output that tells you the boot worked.
"""
import gzip
import os
import stat
import sys

INIT = b"""#!/bin/busybox sh
/bin/busybox echo "LATR-INITRAMFS-OK"
/bin/busybox mount -t proc none /proc 2>/dev/null
/bin/busybox mount -t sysfs none /sys 2>/dev/null
/bin/busybox echo "LATR-INITRAMFS-PROCFS $(/bin/busybox cat /proc/version)"
/bin/busybox free
/bin/busybox echo "LATR-INITRAMFS-DONE"
/bin/busybox poweroff -f
/bin/busybox sleep 1
"""

APPLETS = [
    "sh", "echo", "mount", "cat", "free", "poweroff", "sleep", "ls", "dmesg",
]

# Filesystem timestamps are fixed so two builds of the same busybox produce the
# same archive; otherwise every rebuild rewrites the embedded blob.
MTIME = 0


def pad4(data):
    return data + b"\0" * ((4 - len(data) % 4) % 4)


def entry(name, data, mode, ino, rdev=(0, 0)):
    fields = [ino, mode, 0, 0, 1, MTIME, len(data), 0, 0, rdev[0], rdev[1],
              len(name) + 1, 0]
    header = b"070701" + b"".join(b"%08X" % f for f in fields)
    name_field = name.encode() + b"\0"
    # Pad to a 4-byte boundary *including* the 110-byte header, not just the
    # name: 110 is 2 mod 4, so padding the name by (4 - len) % 4 misaligns every
    # entry whose namesize is not 2 mod 4. The kernel then reports "Initramfs
    # unpacking failed: broken padding", which points at the payload instead of
    # at the arithmetic.
    name_pad = (4 - (110 + len(name_field)) % 4) % 4
    return header + name_field + b"\0" * name_pad + pad4(data)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: mk_riscv64_initramfs.py <busybox> <out.cpio.gz>\n")
        return 2
    busybox_path, out = argv[1], argv[2]
    with open(busybox_path, "rb") as f:
        busybox = f.read()
    if busybox[:4] != b"\x7fELF" or busybox[18:20] != b"\xf3\x00":
        sys.stderr.write("mk_riscv64_initramfs: %s is not a riscv64 ELF\n"
                         % busybox_path)
        return 1

    ino = 1
    archive = b""

    def add(path, data, mode, rdev=(0, 0)):
        nonlocal archive, ino
        archive += entry(path, data, mode, ino, rdev)
        ino += 1

    # The "." entry is not optional. Without it the kernel's unpacker fails to
    # resolve the first top-level name and reports "Initramfs unpacking failed:
    # no cpio magic", which reads like a format error and sends you looking at
    # the fields. Measured: an archive whose first entry is "bin" fails, the
    # same archive with "." in front of it unpacks.
    add(".", b"", stat.S_IFDIR | 0o755)
    for d in ("bin", "dev", "proc", "sys", "tmp"):
        add(d, b"", stat.S_IFDIR | 0o755)
    add("init", INIT, stat.S_IFREG | 0o755)
    add("bin/busybox", busybox, stat.S_IFREG | 0o755)
    for applet in APPLETS:
        add("bin/" + applet, b"busybox", stat.S_IFLNK | 0o777)
    # 5:1 is /dev/console. Without it the kernel's init output goes nowhere.
    add("dev/console", b"", stat.S_IFCHR | 0o600, (5, 1))
    add("dev/null", b"", stat.S_IFCHR | 0o666, (1, 3))
    add("TRAILER!!!", b"", 0)

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "wb") as f:
        f.write(gzip.compress(archive, 9, mtime=0))
    sys.stderr.write("mk_riscv64_initramfs: %s (%d bytes, %d entries)\n"
                     % (out, os.path.getsize(out), ino - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
