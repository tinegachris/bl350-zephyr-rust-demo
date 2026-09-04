#!/usr/bin/env bash
#
# Copyright (c) 2026 Chrispine Tinega <dev@chrispinetinega.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Install the demo on a BL350 and reboot into it.
#
#   ./scripts/stage.sh <ssh-host>
#
# If something on the board already talks to the coprocessor, name its systemd
# unit so this can stop it for the duration:
#
#   COPROC_SERVICE=my-app ./scripts/stage.sh <ssh-host>
#
# This reboots the board. See "Swapping the firmware costs a reboot" in
# README.md for why there is no way around it. Undo with restore.sh, which puts
# back whatever firmware was installed before.

set -euo pipefail

TARGET="${1:-}"
COPROC_SERVICE="${COPROC_SERVICE:-}"

if [ -z "$TARGET" ]; then
	echo "usage: $0 <ssh-host>   (a host or ~/.ssh/config alias for the board)" >&2
	exit 1
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO_ELF="$HERE/build/ipc-hello/zephyr/zephyr.elf"
DEMO_BIN="$HERE/a53/ipc-hello-rs/out/ipc-hello-rs"
UDEV_RULE="$HERE/udev/99-bl350-demo.rules"

BACKUP=/root/coproc-fw.backup-before-demo

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

for f in "$DEMO_ELF" "$DEMO_BIN" "$UDEV_RULE"; do
	[ -f "$f" ] || { echo "missing: $f"$'\n'"build it first. See README.md" >&2; exit 1; }
done

# Ask the board which remoteproc is the M4F and which firmware file it loads.
# Never hardcode remoteproc0: the index is not stable across boots, and the
# other core answers to the same paths.
say "Finding the coprocessor"
read -r M4F FW < <(ssh "$TARGET" '
	for d in /sys/class/remoteproc/remoteproc*; do
		grep -q m4fss "$d/name" 2>/dev/null && M4F="$(basename "$d")"
	done
	[ -n "$M4F" ] || { echo "no m4fss remoteproc on this board" >&2; exit 1; }
	echo "$M4F $(cat /sys/class/remoteproc/$M4F/firmware)"
')
echo "$M4F loads /lib/firmware/$FW"

say "Backing up the firmware that is there now"
# -n so running this twice cannot overwrite the real backup with the demo image.
ssh "$TARGET" "cp -n /lib/firmware/$FW $BACKUP; md5sum /lib/firmware/$FW $BACKUP"

say "Installing the udev rule, the demo firmware and the Rust binary"
scp -q "$UDEV_RULE" "$TARGET:/etc/udev/rules.d/99-bl350-demo.rules"
scp -q "$DEMO_BIN" "$TARGET:/usr/local/bin/ipc-hello-rs"
scp -q "$DEMO_ELF" "$TARGET:/lib/firmware/$FW"
ssh "$TARGET" "chmod 0755 /usr/local/bin/ipc-hello-rs; md5sum /lib/firmware/$FW"

if [ -n "$COPROC_SERVICE" ]; then
	say "Stopping $COPROC_SERVICE so it does not compete for the coprocessor"
	ssh "$TARGET" "systemctl disable --now '$COPROC_SERVICE' >/dev/null 2>&1 || true"
fi

say "Rebooting"
ssh "$TARGET" "(sleep 1; systemctl reboot) >/dev/null 2>&1 &" || true

say "Waiting for the board"
for _ in $(seq 1 40); do
	sleep 5
	ssh -o ConnectTimeout=5 -o BatchMode=yes "$TARGET" true 2>/dev/null && break
done

say "Checking"
ssh "$TARGET" 'bash -s' <<'REMOTE'
set -e
M4F=""
for d in /sys/class/remoteproc/remoteproc*; do
	grep -q m4fss "$d/name" 2>/dev/null && M4F="$(basename "$d")"
done
[ -n "$M4F" ] || { echo "FAIL: no m4fss remoteproc found"; exit 1; }

echo "coprocessor : $M4F ($(cat /sys/class/remoteproc/$M4F/name)) $(cat /sys/class/remoteproc/$M4F/state)"

echo
echo "--- endpoint ---"
ls -l /dev/rpmsg_bl350_demo 2>/dev/null \
	|| { echo "FAIL: /dev/rpmsg_bl350_demo missing. Is the udev rule installed?"; exit 1; }

echo
echo "--- what the coprocessor says for itself ---"
cat "/sys/kernel/debug/remoteproc/$M4F/trace0"
REMOTE

say "Ready. Run the demo with:  ssh $TARGET ipc-hello-rs"
