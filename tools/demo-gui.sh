#!/usr/bin/env bash
# Run the Linux GUI regression on an isolated X display and state tree.
set -u
cd "$(dirname "$0")/.."
RUN=$(mktemp -d "${TMPDIR:-.}/usbmon-gui.XXXXXX") || exit 1
RUN=$(cd "$RUN" && pwd) || exit 1
SYS=$RUN/sys
LOG=$RUN/events.jsonl
export XDG_STATE_HOME=$RUN/state XDG_CONFIG_HOME=$RUN/config
mkdir -p "$SYS/block"
xvfb= daemon=
cleanup() {
    if [ -n "$daemon" ]; then kill -TERM "$daemon" 2>/dev/null; wait "$daemon" 2>/dev/null; fi
    if [ -n "$xvfb" ]; then kill "$xvfb" 2>/dev/null; wait "$xvfb" 2>/dev/null; fi
    rm -rf -- "$RUN"
}
trap cleanup EXIT

if [ -n "${USBMON_TEST_DISPLAY:-}" ]; then
    export DISPLAY=$USBMON_TEST_DISPLAY
else
    command -v Xvfb >/dev/null || { echo "Xvfb not installed"; exit 1; }
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$RUN/display" >"$RUN/xvfb.log" 2>&1 &
    xvfb=$!
    for _ in {1..40}; do
        [ -s "$RUN/display" ] && break
        kill -0 "$xvfb" 2>/dev/null || { cat "$RUN/xvfb.log"; exit 1; }
        sleep 0.1
    done
    [ -s "$RUN/display" ] || { cat "$RUN/xvfb.log"; exit 1; }
    export DISPLAY=:$(cat "$RUN/display")
fi
command -v xwininfo >/dev/null || { echo "xwininfo not installed"; exit 1; }
[ -x ./usbmon ] || make usbmon || exit 1
[ -x ./usbmon-toast ] || make usbmon-toast || exit 1

PASS=0 FAIL=0
ok()  { PASS=$((PASS+1)); echo "  >>> PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  >>> FAIL: $1"; }
check() { local label=$1; shift; if "$@"; then ok "$label"; else bad "$label"; fi; }
count_windows() {
    local tree
    tree=$(xwininfo -root -tree) || return 1
    awk '/"usbmon-toast"/ { n++ } END { print n+0 }' <<<"$tree"
}
make_device() {
    local key=$1 port="$SYS/devices/usb/port-$1" node="$SYS/block/$1"
    mkdir -p "$port" "$node/${key}1"
    echo 1 > "$node/removable"
    echo 59105800 > "$node/size"
    echo "$2" > "$port/model"
    ln -s ../../bus/usb "$port/subsystem"
    ln -s "$port" "$node/device"
}
start() {
    ./usbmon --sys-root "$SYS" --log "$LOG" --interval 3600 --no-hooks --toast-secs 30 &
    daemon=$!
    for _ in {1..40}; do
        grep -q '"ev":"start"' "$LOG" 2>/dev/null && break
        sleep 0.1
    done
}
stop() {
    kill -TERM "$daemon"
    wait "$daemon"
    local rc=$?
    daemon=
    return "$rc"
}
wait_windows() {
    local expected=$1
    for _ in {1..40}; do
        [ "$(count_windows)" = "$expected" ] && return 0
        sleep 0.1
    done
    return 1
}

make_device sdb "SanDisk Cruzer 3.0"
start
check "daemon running" kill -0 "$daemon"
check "GUI enabled" grep -q 'gui=1' "$LOG"
check "baseline add has no popup" wait_windows 0
make_device sdc "金士顿 DataTraveler 3.0"
check "insertion opens a real X11 window" wait_windows 1
check "add recorded" grep -q '"ev":"add","key":"sdc"' "$LOG"
check "hot path woke the monitor" grep -q '"wake":"hot"' "$LOG"
if [ -n "${USBMON_GUI_CAPTURE:-}" ] && command -v import >/dev/null; then
    import -display "$DISPLAY" -window usbmon-toast "$USBMON_GUI_CAPTURE"
fi
rm -rf "$SYS/block/sdc"
check "removal opens a second X11 window" wait_windows 2
check "remove recorded" grep -q '"ev":"remove","key":"sdc"' "$LOG"
check "clean exit with visible notifications" stop
check "both helper windows closed" wait_windows 0
adds=$(grep -c '"ev":"add"' "$LOG")
start
sleep 1
check "restart does not duplicate additions" test "$(grep -c '"ev":"add"' "$LOG")" = "$adds"
check "restart exits cleanly" stop
check "stop recorded" grep -q '"ev":"stop","detail":"signal"' "$LOG"
printf 'result: %s passed, %s failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
