#!/usr/bin/env bash
# Run a bare-metal LATA image under QEMU, killing it as soon as the guest says
# it is done or the watchdog fires.  A bare QEMU launch is what leaves orphan
# processes behind: the image ends its test and spins forever, so the child
# outlives the shell that started it.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="${QEMU_BIN:-$ROOT_DIR/qemu-system-loongarch64}"
KERNEL="${QEMU_KERNEL:-$ROOT_DIR/build/bare_lata.elf}"
LOG_FILE="${QEMU_LOG:-$ROOT_DIR/build/lata_smoke.log}"
DONE_TOKEN="${QEMU_DONE:-lata-isa: done}"
TIMEOUT_SEC="${QEMU_TIMEOUT:-120}"
CPU="${QEMU_CPU:-la464}"

[[ -x "$QEMU" ]] || { echo "no qemu at $QEMU" >&2; exit 1; }
[[ -f "$KERNEL" ]] || { echo "no image at $KERNEL" >&2; exit 1; }

rm -f "$LOG_FILE"
"$QEMU" -M virt -cpu "$CPU" -m 512M -kernel "$KERNEL" \
    -display none -monitor none -nic none \
    -L "$(dirname "$QEMU")/../share/qemu" \
    -serial "file:$LOG_FILE" -no-reboot >/dev/null 2>&1 &
QEMU_PID=$!

# Watchdog: nothing here may leave a QEMU behind.
( sleep "$TIMEOUT_SEC"; kill -KILL "$QEMU_PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCHDOG=$!

for _ in $(seq 1 $((TIMEOUT_SEC * 5))); do
    grep -qa "$DONE_TOKEN" "$LOG_FILE" 2>/dev/null && break
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.2
done

kill -KILL "$QEMU_PID" 2>/dev/null
kill -KILL "$WATCHDOG" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null
exit 0
