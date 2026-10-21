#!/usr/bin/env bash
# The whole install, unattended: an empty hard disk, the install floppy,
# then a boot from the hard disk with each Extras disk in the drive, then
# a check. Screenshots end up in Images/amiga/run-*.png.
# Needs KICKSTART (see ../run.sh).
set -e
cd "$(dirname "$0")/.."
IMG=../../../../Images/amiga
OUT=$IMG/run
n_extras=$(ls $IMG/fuzix-extras*.adf | wc -l)
ADF=fuzix-floppy.adf HD=blank KEYS=tests/install-1.keys ./run.sh 900 >/dev/null 2>&1
cp $OUT/installed.png $IMG/run-installed.png
for n in $(seq 1 $n_extras); do
	KEEP_DISK=1 ADF=fuzix-extras$n.adf FLOPPY_SPEED=100 KEYS=tests/extras.keys \
		./run.sh 900 >/dev/null 2>&1
	cp $OUT/extras.png $IMG/run-extras$n.png
	cat $OUT/keys.log 2>/dev/null || true
done
KEEP_DISK=1 ADF=none KEYS=tests/check.keys ./run.sh 300 >/dev/null 2>&1
cp $OUT/check.png $IMG/run-check.png
grep -i panic $OUT/serial.log || echo "no panics"
