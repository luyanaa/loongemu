#!/usr/bin/env python3
"""Unified LATR benchmark report (roadmap item 7).

Reads the logs the existing targets already produce and prints one table, so a
change can be compared against a baseline without hunting through four logs.
Deliberately **report-only**: it never re-runs a target, because silently
re-running would make the numbers depend on this script's idea of what is fresh
rather than on what the caller actually ran. Each section states the log it came
from and that log's age.

Two kinds of number are kept apart, because they mean different things:

  * deterministic counters (checks, blocks, writebacks, chain links, diff
    hashes) - equal across machines for the same source, and the only ones in
    the fingerprint;
  * host nanoseconds from the dispatch harness - wall-clock on the machine that
    ran it, useful for ratios, never for a gate.

`guest instructions/sec` and `host instructions per guest instruction` are not
reported: the first needs a LoongArch wall-clock the images cannot produce, and
the second needs host instruction counters. Saying so is better than printing a
number that measures QEMU.
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(".")
BUILD = ROOT / "build"

SECTIONS = []


def read_log(name):
    path = BUILD / name
    if not path.exists():
        return None
    return path.read_text(errors="replace")


def age(name):
    path = BUILD / name
    if not path.exists():
        return "missing"
    delta = max(0, int(time.time() - path.stat().st_mtime))
    if delta < 90:
        return f"{delta}s old"
    if delta < 5400:
        return f"{delta // 60}m old"
    return f"{delta // 3600}h old"


def grab(text, pattern, default="-"):
    if not text:
        return default
    match = re.search(pattern, text)
    return match.group(1) if match else default


def table(title, source, rows):
    SECTIONS.append((title, source, rows))


def isa_section():
    text = read_log("latr_isa.log")
    rows = [
        ("checks", grab(text, r"latr-isa: (?:PASS|FAIL) checks=(\d+)")),
        ("diff hash (first 16)", grab(text, r"latr-diff=([0-9a-f]{16})")),
        ("regcache flushes", grab(text, r"flushes=(\d+)")),
        ("regcache elided", grab(text, r"elided=(\d+)")),
        ("regcache writebacks", grab(text, r"writebacks=(\d+)")),
        ("regcache reloads", grab(text, r"reloads=(\d+)")),
        ("code-sym sites", grab(text, r"total=(\d+)")),
        ("borrow leaks (translation invariant)", grab(text, r"borrow-leaks=(\d+)")),
        ("random diff", grab(text, r"latr-rand-diff=([0-9a-f]+)")),
    ]
    table("ISA image", f"build/latr_isa.log ({age('latr_isa.log')})", rows)


def smp_section():
    text = read_log("latr_smp.log")
    rows = [
        ("checks", grab(text, r"latr-smp: (?:PASS|FAIL) checks=(\d+)")),
        ("harts dispatched", grab(text, r"tbs0=(\d+)" ) + "/" +
         grab(text, r"tbs1=(\d+)")),
        ("wake events", grab(text, r"wake=(\d+)")),
        ("h0 chain links", grab(text, r"h0 epoch=\d+ gen=\d+ links=(\d+)")),
        ("h1 chain links", grab(text, r"h1 epoch=\d+ gen=\d+ links=(\d+)")),
        ("h1 chain misses", grab(text, r"h1 .* miss=(\d+)")),
        ("h1 wfi breaks", grab(text, r"h1 .* wfi=(\d+)")),
        ("fault diff", grab(read_log("latr_fault.log"),
                            r"latr-fault-diff=([0-9a-f]+)")),
    ]
    table("SMP image", f"build/latr_smp.log ({age('latr_smp.log')})", rows)


def sys_section():
    text = read_log("latr_sys_diff.log")
    perf = read_log("latr_sys_diff_perf.log")
    rows = [
        ("guest boot", "PASS" if text and "latr-sys: PASS" in text else "-"),
        ("regcache flushes", grab(text, r"flushes=(\d+)")),
        ("regcache writebacks", grab(text, r"writebacks=(\d+)")),
        ("regcache reloads", grab(text, r"reloads=(\d+)")),
        ("guest uart non-empty", "yes" if text and
         ("SYS:" in text or "guest=" in text) else "no"),
    ]
    table("Guest boot", f"build/latr_sys_diff.log ({age('latr_sys_diff.log')})",
          rows)
    if perf:
        table("Guest boot (PERF=1)",
              f"build/latr_sys_diff_perf.log ({age('latr_sys_diff_perf.log')})",
              [("perf line", grab(perf, r"(latr-perf:[^\n]*)", "present"))])


def host_section():
    binary = ROOT / "latr" / "build-host" / "latr-host-smp"
    if not binary.exists():
        table("Dispatch cost (host)", "latr/build-host/latr-host-smp (missing)",
              [("note", "build with: make -C latr test-host-machine")])
        return
    out = subprocess.run([str(binary)], capture_output=True, text=True, timeout=600)
    rows = [
        ("cache-hit lookup (ns)", grab(out.stdout, r"lookup=(\d+)ns")),
        ("full dispatch (ns)", grab(out.stdout, r"/dispatch=(\d+)ns")),
        ("  clint tick (ns)", grab(out.stdout, r"clint-tick=(\d+)ns")),
        ("  interrupt refresh (ns)", grab(out.stdout, r"interrupts=(\d+)ns")),
        ("  safepoint (ns)", grab(out.stdout, r"safepoint=(\d+)ns")),
        ("  context bind (ns)", grab(out.stdout, r"bind=(\d+)ns")),
        ("chain/link checks", grab(out.stdout, r"latr-host-smp: (\d+) checks")),
    ]
    table("Dispatch cost (host, wall clock)", "latr/build-host/latr-host-smp",
          rows)


def fingerprint():
    """Deterministic numbers only: the same source must produce the same value."""
    text = read_log("latr_isa.log") or ""
    smp = read_log("latr_smp.log") or ""
    fault = read_log("latr_fault.log") or ""
    parts = [
        grab(text, r"latr-isa: (?:PASS|FAIL) checks=(\d+)"),
        grab(text, r"latr-diff=([0-9a-f]{8})"),
        grab(text, r"latr-rand-diff=([0-9a-f]+)"),
        grab(text, r"flushes=(\d+)"),
        grab(text, r"writebacks=(\d+)"),
        grab(text, r"reloads=(\d+)"),
        grab(smp, r"latr-smp: (?:PASS|FAIL) checks=(\d+)"),
        grab(smp, r"h0 epoch=\d+ gen=\d+ links=(\d+)"),
        grab(smp, r"h1 epoch=\d+ gen=\d+ links=(\d+)"),
        grab(smp, r"tbs0=(\d+)"),
        grab(smp, r"tbs1=(\d+)"),
        grab(fault, r"latr-fault-diff=([0-9a-f]+)"),
    ]
    blob = "|".join(parts).encode()
    return hashlib.sha256(blob).hexdigest()[:16], parts


def head_commit():
    """Short HEAD, tolerating a container without git (CI images often lack it):
    fall back to reading .git directly, and to "unknown" rather than crashing."""
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    ref = ROOT / ".git" / "HEAD"
    if ref.exists():
        text = ref.read_text().strip()
        if text.startswith("ref:"):
            target = ROOT / ".git" / text.split(" ", 1)[1].strip()
            if target.exists():
                return target.read_text().strip()[:7]
        return text[:7]
    return "unknown"


def main():
    head = head_commit()
    isa_section()
    smp_section()
    sys_section()
    host_section()

    lines = [f"# LATR benchmark (HEAD {head})", ""]
    for title, source, rows in SECTIONS:
        lines.append(f"## {title}")
        lines.append("")
        lines.append(f"source: {source}")
        lines.append("")
        lines.append("| metric | value |")
        lines.append("|---|---|")
        for name, value in rows:
            lines.append(f"| {name} | {value} |")
        lines.append("")
    digest, parts = fingerprint()
    lines.append("## Fingerprint")
    lines.append("")
    lines.append(f"`{digest}` over: {', '.join(parts)}")
    lines.append("")
    report = "\n".join(lines)
    (BUILD / "latr_bench.md").write_text(report)
    sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
