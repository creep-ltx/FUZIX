#!/usr/bin/env bash
# Boot Fuzix in FS-UAE as a stock A1200 (68EC020, 2MB chip, no fast RAM),
# headless inside cage. Prints the serial console; saves screenshots.
#
# Usage: uae/run.sh [seconds] [keyscript]
#   KICKSTART=<A1200 3.1 ROM>   required: Kickstart 3.1 r40.068 for the A1200
#   SHOTS="10 20"               seconds after start to take screenshots
#   UAE_EXTRA="fast_memory = 8192"   extra config lines (\n separated)
#   ADF=fuzix-floppy.adf        boot floppy in Images/amiga (default
#                               fuzix.adf; fuzix-floppy.adf = everything
#                               on the floppy)
#   HD=none                     no hard disk
#   FLOPPY_SPEED=100            real floppy speed (default 0 = turbo)
#   KEEP_DISK=1                 keep the disks the last run left
#   KEYS=<file>                 typed on the Amiga keyboard (uae/vkey):
#                               lines "sleep N", "type TEXT", "key NAME",
#                               "ctrl X"
#
# The keyscript (serial port) lines are "sleep N" and "send TEXT"
# (see serial-tap.py).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
IMG="$ROOT/Images/amiga"
SECS="${1:-30}"
SCRIPT="${2:-}"
if [ -z "${KICKSTART:-}" ]; then echo "set KICKSTART to an A1200 Kickstart 3.1 ROM" >&2; exit 1; fi
SHOTS="${SHOTS:-}"
OUT="${OUT:-$IMG/run}"
mkdir -p "$OUT"
rm -f "$OUT"/*.png "$OUT"/serial.log

# The hard disk is written to: run from a copy (KEEP_DISK=1 reuses the
# last run's copy, to see what a previous run changed)
ADF="${ADF:-fuzix.adf}"
[ -n "${KEEP_DISK:-}" ] && [ -f "$OUT/disk.img" ] || cp "$IMG/disk.img" "$OUT/disk.img"
[ -n "${KEEP_DISK:-}" ] && [ -f "$OUT/$ADF" ] || cp "$IMG/$ADF" "$OUT/$ADF"
if [ "${HD:-}" = none ]; then
	HDCONF=""
else
	HDCONF="hard_drive_0 = $OUT/disk.img
hard_drive_0_controller = ide0
hard_drive_0_type = hdf"
fi

TTY="$OUT/serial-pty"
cat > "$OUT/run.fs-uae" <<EOF
[fs-uae]
amiga_model = A1200
kickstart_file = $KICKSTART
floppy_drive_0 = $OUT/$ADF
floppy_drive_speed = ${FLOPPY_SPEED:-0}
writable_floppy_images = 1
$HDCONF
serial_port = $TTY
uae_serial_direct = true
fullscreen = 1
fullscreen_mode = window
zoom = full
keep_aspect = 0
automatic_input_grab = 0
$(printf '%b' "${UAE_EXTRA:-}")
EOF

python3 "$HERE/serial-tap.py" "$TTY" "$OUT/serial.log" $SCRIPT >"$OUT/tap.out" 2>&1 &
TAP=$!
sleep 1

before=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$' | sort || true)
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1 \
	timeout "$SECS" cage -- fs-uae "$OUT/run.fs-uae" >"$OUT/uae.log" 2>&1 </dev/null &
EMU=$!

# Find cage's display for grim
DISP=""
for i in $(seq 1 50); do
	after=$(ls "$XDG_RUNTIME_DIR" | grep '^wayland-[0-9]*$' | sort || true)
	DISP=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
	[ -n "$DISP" ] && break
	sleep 0.1
done

[ -n "${KEYS:-}" ] && [ ! -x "$HERE/vkey/vkey" ] && make -s -C "$HERE/vkey"
if [ -n "${KEYS:-}" ] && [ -n "$DISP" ]; then
	(while read -r cmd arg; do
		case "$cmd" in
		sleep) sleep "$arg" ;;
		type) WAYLAND_DISPLAY=$DISP "$HERE/vkey/vkey" "$arg" ;;
		key) WAYLAND_DISPLAY=$DISP "$HERE/vkey/vkey" -k "$arg" ;;
		ctrl) WAYLAND_DISPLAY=$DISP "$HERE/vkey/vkey" -c "$arg" ;;
		esac
	done < "$KEYS") >/dev/null 2>&1 &
fi

t=0
for s in $SHOTS; do
	sleep $((s - t)); t=$s
	[ -n "$DISP" ] && WAYLAND_DISPLAY=$DISP grim "$OUT/shot-$s.png" 2>/dev/null || true
done

wait $EMU 2>/dev/null || true
kill $TAP 2>/dev/null || true
echo "=== serial console (${SECS}s):"
tr -d '\r' < "$OUT/serial.log"
ls "$OUT"/*.png 2>/dev/null || true
