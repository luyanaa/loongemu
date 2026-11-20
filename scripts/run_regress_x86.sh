#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${QEMU_KERNEL:-$ROOT_DIR/build/regress_x86.elf}"
TIMEOUT_SEC="${QEMU_TIMEOUT:-12}"

run_profile() {
    local name="$1"
    local cpu="$2"
    local expect="$3"
    local log="$ROOT_DIR/build/regress_${name}.log"
    local pidfile="$ROOT_DIR/build/qemu_regress.pid"

    rm -f "$log" "$pidfile"
    (
        QEMU_TIMEOUT="$TIMEOUT_SEC" QEMU_CPU="$cpu" QEMU_KERNEL="$KERNEL" \
            QEMU_LOG="$log" "$ROOT_DIR/scripts/run_qemu_tb_sys.sh"
    ) >/dev/null 2>&1 &
    local runner_pid=$!

    local waited=0
    while [[ "$waited" -lt "$TIMEOUT_SEC" ]]; do
        if grep -qa "regress: done" "$log" 2>/dev/null; then
            break
        fi
        sleep 1
        waited=$((waited + 1))
    done

    if [[ -f "$ROOT_DIR/build/qemu_tb_sys.pid" ]]; then
        local qpid
        qpid="$(cat "$ROOT_DIR/build/qemu_tb_sys.pid" 2>/dev/null || true)"
        [[ -n "$qpid" ]] && kill -KILL "$qpid" 2>/dev/null || true
        rm -f "$ROOT_DIR/build/qemu_tb_sys.pid"
    fi
    kill -KILL "$runner_pid" 2>/dev/null || true
    wait "$runner_pid" 2>/dev/null || true

    echo "=== profile: $name (-cpu $cpu) ==="
    cat "$log"

    if ! grep -qa "regress: done" "$log"; then
        echo "ERROR: $name did not reach 'regress: done' within ${TIMEOUT_SEC}s"
        return 1
    fi
    if [[ "$expect" == "refused" ]]; then
        grep -qa "refusing to start: this host has no LSX" "$log" || {
            echo "ERROR: $name did not refuse startup without LSX"; return 1;
        }
        grep -qa "refused" "$log" || {
            echo "ERROR: $name missing refused marker"; return 1;
        }
    else
        grep -qa "fails=0000000000000000 ok" "$log" || {
            echo "ERROR: $name reported failures"; return 1;
        }
    fi
}

rc=0
run_profile "la464_v3"       "la464"                  "ok"      || rc=1
run_profile "la464_v3_swlbt" "la464,lbt=off"          "ok"      || rc=1
run_profile "la464_v2"       "la464,lasx=off"         "ok"      || rc=1
run_profile "la464_no_lsx"   "la464,lsx=off,lasx=off" "refused" || rc=1

rm -f "$ROOT_DIR/build/qemu_tb_sys.pid" "$ROOT_DIR"/build/regress_*.log
exit "$rc"
