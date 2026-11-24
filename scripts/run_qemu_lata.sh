#!/usr/bin/env bash
# Run a bare-metal LATA image under QEMU, killing it as soon as the guest says
# it is done or the watchdog fires.  A bare QEMU launch is what leaves orphan
# processes behind: the image ends its test and spins forever, so the child
# outlives the shell that started it.
#
# Exit status is the verification result, not "qemu was launched":
#   0  the expected DONE_TOKEN appeared in the serial log
#   2  the watchdog fired before the token appeared
#   3  QEMU exited on its own before the token appeared
# Without this, a missing token, an early exit or a hang all looked like
# success to CI.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU="${QEMU_BIN:-$ROOT_DIR/qemu-system-loongarch64}"
KERNEL="${QEMU_KERNEL:-$ROOT_DIR/build/bare_lata.elf}"
LOG_FILE="${QEMU_LOG:-$ROOT_DIR/build/lata_smoke.log}"
DONE_TOKEN="${QEMU_DONE:-lata-isa: done}"
TIMEOUT_SEC="${QEMU_TIMEOUT:-120}"
CPU="${QEMU_CPU:-la464}"
# qemu-system-loongarch64 rejects ram_size <= 1G for the la464 virt machine in
# some distro builds, so ask for the smallest size that also works there.
MEM="${QEMU_MEM:-1G}"

[[ -x "$QEMU" ]] || { echo "no qemu at $QEMU" >&2; exit 1; }
[[ -f "$KERNEL" ]] || { echo "no image at $KERNEL" >&2; exit 1; }

ERR_FILE="${LOG_FILE%.log}.err"
rm -f "$LOG_FILE" "$ERR_FILE"
# QEMU_DEBUG=1 adds the executor trace (-d int) to <log>.trace. It is how a
# distro qemu that dies without a serial byte can be told apart from one whose
# guest never got scheduled at all.
TRACE_FILE="${LOG_FILE%.log}.trace"
QEMU_ARGS=()
if [[ "${QEMU_DEBUG:-0}" != "0" ]]; then
    rm -f "$TRACE_FILE"
    QEMU_ARGS+=(-d int,guest_errors -D "$TRACE_FILE")
fi
"$QEMU" -M virt -cpu "$CPU" -m "$MEM" -kernel "$KERNEL" \
    -display none -monitor none -nic none \
    -L "$(dirname "$QEMU")/../share/qemu" \
    -serial "file:$LOG_FILE" -no-reboot ${QEMU_ARGS[@]+"${QEMU_ARGS[@]}"} \
    >/dev/null 2>>"$ERR_FILE" &
QEMU_PID=$!

# Watchdog: nothing here may leave a QEMU behind.  It records that it fired so a
# watchdog kill is not reported as "qemu exited on its own".
WATCHDOG_FLAG="$(mktemp)"
( sleep "$TIMEOUT_SEC"; : > "$WATCHDOG_FLAG"; kill -KILL "$QEMU_PID" 2>/dev/null ) >/dev/null 2>&1 &
WATCHDOG=$!

token_seen=0
qemu_exited=0
for _ in $(seq 1 $((TIMEOUT_SEC * 5))); do
    if grep -qa -- "$DONE_TOKEN" "$LOG_FILE" 2>/dev/null; then
        token_seen=1
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        qemu_exited=1
        break
    fi
    sleep 0.2
done

kill -KILL "$QEMU_PID" 2>/dev/null
kill -KILL "$WATCHDOG" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

if [[ "$token_seen" -eq 1 ]]; then
    exit 0
fi

if [[ -s "$WATCHDOG_FLAG" ]]; then
    qemu_exited=0
fi
rm -f "$WATCHDOG_FLAG"

if [[ "$qemu_exited" -eq 1 ]]; then
    echo "run_qemu_lata: qemu exited before '$DONE_TOKEN' appeared" >&2
    status=3
else
    echo "run_qemu_lata: timed out after ${TIMEOUT_SEC}s waiting for '$DONE_TOKEN'" >&2
    status=2
fi
echo "run_qemu_lata: log tail ($LOG_FILE):" >&2
tail -n 20 "$LOG_FILE" 2>/dev/null >&2
echo "run_qemu_lata: log bytes: $(wc -c <"$LOG_FILE" 2>/dev/null || echo 0)" >&2
if [[ -s "$ERR_FILE" ]]; then
    echo "run_qemu_lata: qemu stderr ($ERR_FILE):" >&2
    tail -n 20 "$ERR_FILE" >&2
fi
if [[ -s "$TRACE_FILE" ]]; then
    echo "run_qemu_lata: qemu trace head ($TRACE_FILE):" >&2
    head -n 12 "$TRACE_FILE" >&2
    echo "run_qemu_lata: qemu trace tail ($TRACE_FILE):" >&2
    tail -n 12 "$TRACE_FILE" >&2
fi
exit "$status"
