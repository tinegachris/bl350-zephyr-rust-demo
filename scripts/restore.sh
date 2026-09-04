#!/usr/bin/env bash
#
# Copyright (c) 2026 Chrispine Tinega <dev@chrispinetinega.com>
#
# SPDX-License-Identifier: Apache-2.0
#
# Put the board back the way stage.sh found it.
#
#   ./scripts/restore.sh <ssh-host>
#
# If stage.sh stopped a service for you, name it again so this can re-enable it:
#
#   COPROC_SERVICE=my-app ./scripts/restore.sh <ssh-host>
#
# Reboots, for the same reason stage.sh does. Checks afterwards rather than
# assuming.

set -euo pipefail

TARGET="${1:-}"
COPROC_SERVICE="${COPROC_SERVICE:-}"

if [ -z "$TARGET" ]; then
	echo "usage: $0 <ssh-host>   (a host or ~/.ssh/config alias for the board)" >&2
	exit 1
fi

BACKUP=/root/coproc-fw.backup-before-demo

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

say "Finding the coprocessor"
read -r M4F FW < <(ssh "$TARGET" '
	for d in /sys/class/remoteproc/remoteproc*; do
		grep -q m4fss "$d/name" 2>/dev/null && M4F="$(basename "$d")"
	done
	[ -n "$M4F" ] || { echo "no m4fss remoteproc on this board" >&2; exit 1; }
	echo "$M4F $(cat /sys/class/remoteproc/$M4F/firmware)"
')
echo "$M4F loads /lib/firmware/$FW"

say "Restoring the original firmware and removing the demo"
ssh "$TARGET" "
	test -f $BACKUP || { echo 'no backup at $BACKUP Did stage.sh run?' >&2; exit 1; }
	cp $BACKUP /lib/firmware/$FW
	rm -f /etc/udev/rules.d/99-bl350-demo.rules /usr/local/bin/ipc-hello-rs
	md5sum /lib/firmware/$FW $BACKUP
"

if [ -n "$COPROC_SERVICE" ]; then
	say "Re-enabling $COPROC_SERVICE"
	ssh "$TARGET" "systemctl enable '$COPROC_SERVICE' >/dev/null 2>&1 || true"
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
echo "coprocessor : $M4F ($(cat /sys/class/remoteproc/$M4F/state))"

test ! -e /dev/rpmsg_bl350_demo \
	&& echo "demo endpoint  : gone" \
	|| echo "demo endpoint  : STILL PRESENT"
test ! -e /usr/local/bin/ipc-hello-rs \
	&& echo "demo binary    : gone" \
	|| echo "demo binary    : STILL PRESENT"
REMOTE

say "Restored. The backup is kept at $BACKUP."
