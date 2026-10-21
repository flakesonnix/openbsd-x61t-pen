#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
umask 077
PATH=/usr/X11R6/bin:/usr/bin:/bin:/usr/sbin:/sbin
export PATH

test "$(uname -s)" = OpenBSD
for tool in Xvfb xauth xdpyinfo openssl; do
    command -v "$tool" >/dev/null
done
for fragment in examples/GiveConsole.fragment examples/TakeConsole.fragment; do
    sh -n "$fragment"
done

work=$(mktemp -d "${TMPDIR:-/tmp}/x61-pen-test.XXXXXXXX")
server=
cleanup() {
    if [ -n "$server" ]; then
        kill "$server" 2>/dev/null || :
        wait "$server" 2>/dev/null || :
    fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

number=
for candidate in 81 82 83 84 85 86 87 88 89; do
    if [ ! -e "/tmp/.X11-unix/X$candidate" ] &&
        [ ! -e "/tmp/.X$candidate-lock" ]; then
        number=$candidate
        break
    fi
done
if [ -z "$number" ]; then
    printf '%s\n' 'No free test display; refusing to use an existing display.' >&2
    exit 1
fi

DISPLAY=:$number
XAUTHORITY=$work/Xauthority
X61_PEN_TEST_DISPLAY=$DISPLAY
export DISPLAY XAUTHORITY X61_PEN_TEST_DISPLAY
touch "$XAUTHORITY"
xauth -f "$XAUTHORITY" add "$DISPLAY" MIT-MAGIC-COOKIE-1 "$(openssl rand -hex 16)"
Xvfb "$DISPLAY" -screen 0 1024x768x24 -noreset -nolisten tcp \
    -auth "$XAUTHORITY" >"$work/Xvfb.log" 2>&1 &
server=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if ! kill -0 "$server" 2>/dev/null; then
        cat "$work/Xvfb.log" >&2
        exit 1
    fi
    if xdpyinfo >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 1
done
if [ "$ready" -ne 1 ]; then
    cat "$work/Xvfb.log" >&2
    printf '%s\n' 'Private Xvfb did not become ready.' >&2
    exit 1
fi

./tests/integration
