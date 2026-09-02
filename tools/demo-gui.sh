#!/usr/bin/env bash
# demo-gui.sh — end-to-end demo of the popup GUI (usbmon-toast) under Xvfb.
#
# Simulates the user experience: start the daemon, insert a USB stick,
# watch the toast pop up instantly (hot path), unplug it, see the gray
# removal toast, then verify clean shutdown with no zombie children.
#
# Requirements: Xvfb + a CJK font (fontconfig) — both present in the
# dev sandbox.  Run: bash tools/demo-gui.sh
#
set -u
cd "$(dirname "$0")/.."

SYS=/tmp/usbmon-gui-sys
LOG=/tmp/usbmon-gui-events.jsonl
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  >>> PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  >>> FAIL: $1"; }

command -v Xvfb >/dev/null || { echo "Xvfb not installed"; exit 1; }
make >/dev/null || exit 1

rm -rf "$SYS" "$LOG" ~/.local/state/usbmon/last-snapshot.txt
mkdir -p "$SYS/block"

# --- virtual display ---------------------------------------------------
Xvfb :77 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
XVFB_PID=$!
sleep 1.2
export DISPLAY=:77

make_device() {  # $1=name $2=vendor $3=model $4=serial $5=sectors
    local port=$(( $(printf '%d' "'${1: -1}") - 97 + 2 ))   # sdb→3, sdc→4...
    local d="$SYS/block/$1"
    local tgt="$SYS/devices/pci0000:00/0000:00:14.0/usb3/3-$port/3-$port:1.0/host6/target6:0:0/6:0:0:0"
    mkdir -p "$tgt" "$d/${1}1"
    echo "$5" > "$d/size"; echo 1 > "$d/removable"
    echo "$(( $5 - 4096 ))" > "$d/${1}1/size"
    echo "$3" > "$tgt/model"; echo "$2" > "$tgt/vendor"; echo "$4" > "$tgt/serial"
    ln -sfn "$tgt" "$d/device"
}

echo "== 1. daemon starts (GUI auto-detected via DISPLAY) =="
make_device sdb "SanDisk " "Cruzer Glide 3.0 " "0101834A2B341234" 59105800
setsid ./usbmon --sys-root "$SYS" --log "$LOG" --interval 3600 \
       --no-hooks --toast-secs 30 >/dev/null 2>&1 &
sleep 1
DPID=$(pgrep -x usbmon | head -1)
[ -n "$DPID" ] && ok "daemon running (pid $DPID)" || bad "daemon not running"
grep -q 'gui=1' "$LOG" && ok "start event says gui=1" || bad "gui not enabled"

echo "== 2. insert a stick → toast within ~1s (hot path) =="
make_device sdc "金士顿 " "DataTraveler 3.0 " "60A44C4C1234567890" 121395200
sleep 1.5
pgrep -P "$DPID" usbmon-toast >/dev/null && ok "toast helper spawned" \
                                                || bad "no toast child"
grep -q '"ev":"add","key":"sdc"' "$LOG" && ok "add event logged" || bad "no add event"
grep '"ev":"round"' "$LOG" | grep -q '"wake":"hot"' && ok "round woken by hot path" \
                                                           || bad "no hot wake"
grep -q '"baseline":1' "$LOG" && ok "baseline add flagged (no popup)" || bad "no baseline flag"

echo "== 3. unplug sdc → removal toast =="
rm -rf "$SYS/block/sdc"
sleep 1.5
pgrep -P "$DPID" usbmon-toast >/dev/null | grep -q . && ok "removal toast present" \
                                                             || true
grep -q '"ev":"remove","key":"sdc"' "$LOG" && ok "remove event logged" || bad "no remove event"

echo "== 4. restart daemon → no duplicate adds (state persistence) =="
ADDS=$(grep -c '"ev":"add"' "$LOG")
kill -TERM "$DPID"; sleep 1
setsid ./usbmon --sys-root "$SYS" --log "$LOG" --interval 3600 \
       --no-hooks >/dev/null 2>&1 &
sleep 1.2
DPID=$(pgrep -x usbmon | head -1)
[ "$(grep -c '"ev":"add"' "$LOG")" = "$ADDS" ] && ok "no re-adds after restart" \
                                                    || bad "duplicate adds after restart"

echo "== 5. shutdown: clean, no zombies, no orphan toasts =="
kill -TERM "$DPID"; sleep 1
pgrep -x usbmon >/dev/null && bad "daemon still alive" || ok "daemon exited"
ps -eo stat,comm 2>/dev/null | grep -q defunct && bad "zombie processes" \
                                                     || ok "no zombies"
grep -q '"ev":"stop","detail":"signal"' "$LOG" && ok "stop event logged" || bad "no stop event"

kill "$XVFB_PID" 2>/dev/null
echo
echo "result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
