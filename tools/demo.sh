#!/usr/bin/env bash
# demo.sh — end-to-end demo + regression assertions of usbmon (mock sysfs).
#
# The sandbox is a container with no real USB devices, so we build a fake
# /sys/block tree (matching real sysfs layout) and point usbmon at it via
# --sys-root.  We then simulate plugging and unplugging a USB stick between
# rounds and watch the JSONL events + hooks fire.
#
# Exits non-zero when any assertion fails (CI-safe: set -u + FAIL counter).
#
# Usage: bash tools/demo.sh

set -u
cd "$(dirname "$0")/.."

SYS=/tmp/usbmon-demo-sys
LOG=/tmp/usbmon-demo-events.jsonl
HOOKS=/tmp/usbmon-demo-hooks.json
MARK=/tmp/usbmon-demo-hook-fired
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  >>> PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "  >>> FAIL: $1"; }

rm -rf "$SYS" "$LOG" "$MARK" ~/.local/state/usbmon/last-snapshot.txt
mkdir -p "$SYS/block"

make_device() {  # $1=name  $2=vendor  $3=model  $4=serial  $5=sectors  $6=usb?
    local d="$SYS/block/$1"
    # unique usb bus path per device (sdb->3-2, sdc->3-3, ...)
    local port="3-$((0x$(echo "$1" | tail -c 2) - 0x61 + 2))"
    local tgt="$SYS/devices/pci0000:00/0000:00:14.0/usb3/$port/$port:1.0/host6/target6:0:0/6:0:0:0"
    mkdir -p "$tgt" "$d/${1}1"
    # block attributes live in /sys/block/<dev>
    echo "$5"     > "$d/size"
    echo 1        > "$d/removable"
    echo 0        > "$d/ro"
    echo "$(( $5 - 4096 ))" > "$d/${1}1/size"
    # SCSI descriptors live at the device symlink target
    echo "$3" > "$tgt/model"
    echo "$2" > "$tgt/vendor"
    echo "$4" > "$tgt/serial"
    # /sys/block/<dev>/device -> ../../devices/.../usbX/X-Y/.../block
    ln -sfn "$tgt" "$d/device"
}

# hooks: fire a marker command when any device is added
cat > "$HOOKS" <<'EOF'
{
  "hooks": [
    {
      "name": "demo-marker",
      "match_keys": ["sd*"],
      "command": ["/usr/bin/touch", "/tmp/usbmon-demo-hook-fired"],
      "debounce_seconds": 1,
      "enabled": true
    }
  ]
}
EOF

echo "== 1. initial state: one USB stick present =="
make_device sdb "SanDisk " "Cruzer Glide 3.0 " "0101834A2B341234" 59105800 usb
./usbmon --list --sys-root "$SYS"
grep -q 'sdb' <<<"$(./usbmon --list --sys-root "$SYS")" \
    && ok "--list shows sdb" || bad "--list missing sdb"

echo
echo "== 2. --once: baseline round, add-event logged + hook fires =="
./usbmon --once --sys-root "$SYS" --log "$LOG" --hooks "$HOOKS" --verbose
sleep 0.5
[ -f "$MARK" ] && ok "hook fired: marker file created" \
                || bad "hook did NOT fire for baseline add"
grep -q '"ev":"add","baseline":1,"key":"sdb"' "$LOG" \
    && ok "baseline add logged" || bad "baseline add event missing"

echo
echo "== 3. simulate plug of a second stick (sdc) =="
rm -f "$MARK"
make_device sdc "Kingston " "DataTraveler 3.0 " "60A44C4C1234567890" 121395200 usb
./usbmon --once --sys-root "$SYS" --log "$LOG" --hooks "$HOOKS" --verbose
sleep 0.5
[ -f "$MARK" ] && ok "hook fired again (sdc)" || bad "hook did NOT fire for sdc"
grep -q '"ev":"add","key":"sdc"' "$LOG" \
    && ok "sdc add logged" || bad "sdc add event missing"

echo
echo "== 4. simulate unplug of sdb + stale mount entry check =="
rm -rf "$SYS/block/sdb"
./usbmon --once --sys-root "$SYS" --log "$LOG" --verbose
grep -q '"ev":"remove","key":"sdb"' "$LOG" \
    && ok "sdb remove logged" || bad "sdb remove event missing"

echo
echo "== 5. events.jsonl content + JSON validity =="
cat "$LOG"
if python3 - "$LOG" <<'PYEOF'
import json, sys
bad = 0
for n, line in enumerate(open(sys.argv[1], encoding="utf-8"), 1):
    line = line.strip()
    if not line:
        continue
    try:
        json.loads(line)
    except Exception as exc:
        print(f"line {n} not valid JSON: {exc}")
        bad += 1
sys.exit(1 if bad else 0)
PYEOF
then ok "every JSONL line is valid JSON"; else bad "invalid JSONL lines"; fi

echo
echo "== 6. daemon mode: 2s rounds, live plug during run =="
rm -rf "$SYS/block/sdc"
./usbmon --sys-root "$SYS" --log "$LOG" --interval 2 --verbose &
DPID=$!
sleep 1
make_device sdd "Generic " "Flash Disk " "070B77A08123" 15634432 usb
sleep 2.5
kill -TERM $DPID
wait $DPID 2>/dev/null
RC=$?
echo "(daemon stopped cleanly, exit=$RC)"
[ "$RC" -eq 0 ] && ok "daemon clean exit on SIGTERM" \
               || bad "daemon exit code $RC on SIGTERM"
grep -q '"ev":"add","key":"sdd"' "$LOG" \
    && ok "live plug add (hot wake) logged" || bad "sdd add missing"
grep -q '"ev":"stop","detail":"signal"' "$LOG" \
    && ok "stop event logged" || bad "stop event missing"
ADDS=$(grep -c '"ev":"add"' "$LOG")
[ "$ADDS" -eq 3 ] && ok "exactly 3 add events total (no duplicates)" \
                  || bad "expected 3 adds, got $ADDS"

echo
echo "== final log tail =="
tail -6 "$LOG"

echo
echo "result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
