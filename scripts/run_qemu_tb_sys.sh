#!/usr/bin/env bash
set -euo pipefail

# Resolve through the script's own path.  `dirname` alone is relative, and the
# old form then joined it with "/.." -- giving /Users/yanlu/Documents instead of
# the repo, so the pid file could never be created.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT_DIR/build/qemu_tb_sys.pid"
LOG_FILE="${QEMU_LOG:-$ROOT_DIR/build/tb_sys.log}"
KERNEL="${QEMU_KERNEL:-$ROOT_DIR/build/tb_sys.elf}"
TIMEOUT_SEC="${QEMU_TIMEOUT:-300}"

# Kill any previously tracked instance before launching a new one
if [[ -f "$PID_FILE" ]]; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "$OLD_PID" ]]; then
        kill -KILL "$OLD_PID" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
fi

echo "$$" > "$PID_FILE"

if [[ "$TIMEOUT_SEC" -gt 0 ]]; then
    (
        sleep "$TIMEOUT_SEC"
        kill -KILL "$$" 2>/dev/null || true
        rm -f "$PID_FILE"
    ) >/dev/null 2>&1 &
fi

# Replace shell with QEMU so SIGTERM/SIGKILL on this process kills QEMU directly.
exec "$ROOT_DIR/qemu-system-loongarch64" \
    -machine virt -cpu "${QEMU_CPU:-la464}" -m 512M \
    -nic none -nographic -monitor none \
    -kernel "$KERNEL" > "$LOG_FILE" 2>&1
