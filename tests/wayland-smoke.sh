#!/bin/bash
# End-to-end smoke test for XNote on a real Wayland compositor.
#
# Replaces the old Xvfb + openbox + xdotool harness, which died with the X11
# backend in v3.0.0. Everything here is display-server-real: a headless weston
# provides an actual wl_compositor, and XNote is driven through its own
# interfaces (the single-instance server socket) rather than synthetic input.
#
# There is deliberately NO synthetic input. Wayland has no XTEST, and
# ydotool-style uinput injection is seat-GLOBAL rather than confined to the
# test compositor -- it has escaped into a live desktop session before.
# Clicking and typing stay in a by-hand acceptance pass on a real desktop;
# this script covers everything else.
#
# THERE IS NO TRAY HERE. dbus-run-session provides a session bus but no
# StatusNotifier watcher, so xpad_tray_has_indicator() is false for the whole
# run. That is not incidental -- it decides when XNote is allowed to exit, and
# both phases below are built around it. See the comments on each phase.
#
# Usage:  dbus-run-session -- tests/wayland-smoke.sh /path/to/xnote
# Exits non-zero on the first failed assertion.

set -u

XNOTE="${1:-}"
if [ -z "$XNOTE" ] || [ ! -x "$XNOTE" ]; then
	echo "usage: $0 /path/to/xnote   (got: '${XNOTE}')" >&2
	exit 2
fi

WORK="$(mktemp -d)"
export XDG_RUNTIME_DIR="$WORK/run"
export XDG_CONFIG_HOME="$WORK/config"
export XDG_DATA_HOME="$WORK/data"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
chmod 700 "$XDG_RUNTIME_DIR"
SOCKET="$XDG_CONFIG_HOME/xnote/server"

# XNote is native Wayland. Unset DISPLAY so that if anything ever reaches for
# X again the test fails loudly instead of silently falling back to XWayland.
unset DISPLAY
export WAYLAND_DISPLAY=wayland-xnote-ci

XNOTE_PID=""
WESTON_PID=""

cleanup_all () {
	[ -n "$XNOTE_PID" ]  && kill -9 "$XNOTE_PID"  2>/dev/null
	[ -n "$WESTON_PID" ] && kill -9 "$WESTON_PID" 2>/dev/null
	# The document portal and gvfsd, started on the session bus below, outlive
	# this script and keep FUSE mounts in $WORK that rm cannot remove.
	awk -v w="$WORK/" 'index($2, w) == 1 { print $2 }' /proc/self/mounts |
		xargs -r -n1 fusermount3 -u -z
	rm -rf "$WORK"
}
trap cleanup_all EXIT

fail () {
	echo "FAIL: $*" >&2
	# Dump both logs into the job output. The work dir is a mktemp that the EXIT
	# trap removes, so there is nothing left for CI artifacts to collect -- the
	# job log is the only place these diagnostics can survive.
	[ -f "$WORK/xnote.err" ]  && sed 's/^/    xnote| /'  "$WORK/xnote.err"  >&2
	[ -f "$WORK/weston.log" ] && sed 's/^/    weston| /' "$WORK/weston.log" >&2
	exit 1
}
ok () { echo "  ok: $*"; }

wait_for () {  # wait_for <path>
	for _ in $(seq 1 60); do [ -e "$1" ] && return 0; sleep 0.25; done
	return 1
}

wait_gone () {  # wait_gone <pid>
	for _ in $(seq 1 60); do kill -0 "$1" 2>/dev/null || return 0; sleep 0.25; done
	return 1
}

# Start the primary instance. ALWAYS with --new, and that is load-bearing:
# xpad_app_first_idle_check() quits outright when there is no tray AND no
# visible pad (xpad-app.c) -- correct behaviour, since such an instance would
# be unreachable, but it means a bare `xnote` against an empty config exits a
# few seconds after startup. --new gives it a visible pad to stay alive for.
start_xnote () {
	rm -f "$SOCKET"
	"$XNOTE" --new >"$WORK/xnote.out" 2>"$WORK/xnote.err" &
	XNOTE_PID=$!
	wait_for "$SOCKET" || fail "xnote never created its server socket (did it start at all?)"
	# first_idle_check retries the tray query for up to ~3s before deciding.
	# Sleep past it, or we assert against an instance that is about to exit.
	sleep 5
	kill -0 "$XNOTE_PID" 2>/dev/null || fail "xnote exited during startup"
}

# --- the session bus ------------------------------------------------------
# dbus-run-session starts its bus before this script runs, so the bus has the
# caller's environment and passes it to every service it activates. GTK and
# GIO bring up the portals, gvfsd and the accessibility bus, which mount and
# listen under XDG_RUNTIME_DIR: run from a desktop, the live session's, where
# the document portal unmounts the running one to take its place and the
# accessibility bus deletes the live socket when it exits. So give the bus
# this run's directories and compositor before XNote starts; a portal with no
# display drops its Settings interface, and GTK warns. DISPLAY= because the
# bus cannot unset a variable. Refuse a bus with any other name on it: that is
# not dbus-run-session's fresh bus but a desktop's, which this would change.
NAMES=$(dbus-send --session --print-reply --dest=org.freedesktop.DBus \
	/org/freedesktop/DBus org.freedesktop.DBus.ListNames) \
	|| fail "no session bus: run this under dbus-run-session"
if printf '%s\n' "$NAMES" | grep 'string "' | grep -qv -e 'string ":' -e '"org.freedesktop.DBus"'; then
	fail "the session bus is already in use: run this under dbus-run-session, not on a desktop's bus"
fi
dbus-update-activation-environment XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME \
	WAYLAND_DISPLAY DISPLAY= || fail "could not update the session bus's activation environment"
ok "private session bus; the services it starts get this run's directories"

# --- the compositor -------------------------------------------------------
weston --backend=headless --socket="$WAYLAND_DISPLAY" --width=1280 --height=1024 \
	>"$WORK/weston.log" 2>&1 &
WESTON_PID=$!
wait_for "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" || fail "weston did not create its socket"
ok "headless weston up on \$WAYLAND_DISPLAY=$WAYLAND_DISPLAY"

# --- phase 1: a long-lived instance ---------------------------------------
start_xnote
ok "xnote started on Wayland, listening, and alive past the tray check"

# A GTK CRITICAL/WARNING at startup is how several past regressions announced
# themselves (NULL titles, unrealized windows, prefs chain-up). Treat as fatal.
if grep -qE "CRITICAL|WARNING \*\*|assertion .* failed" "$WORK/xnote.err"; then
	fail "xnote logged a GTK CRITICAL/WARNING at startup"
fi
ok "no GTK CRITICAL/WARNING at startup"

# --- drive it through its own CLI (no synthetic input) --------------------
printf 'SMOKE TEST PAD\nsecond line\n' >"$WORK/note.txt"
"$XNOTE" --new-from-file "$WORK/note.txt" || fail "--new-from-file returned non-zero"
CONTENT=""
for _ in $(seq 1 60); do   # the save is debounced; it is not on disk immediately
	CONTENT=$(grep -l "SMOKE TEST PAD" "$XDG_CONFIG_HOME"/xnote/content-* 2>/dev/null | head -1)
	[ -n "$CONTENT" ] && break
	sleep 0.25
done
[ -n "$CONTENT" ] || fail "--new-from-file did not persist a note to disk"
CONTENT_BASE="$(basename "$CONTENT")"
INFO_FILE="$(grep -l "content ${CONTENT_BASE}" "$XDG_CONFIG_HOME"/xnote/info-* 2>/dev/null | head -1)"
[ -n "$INFO_FILE" ] || fail "--new-from-file did not write matching info-* file"
grep -q "^hidden 0$" "$INFO_FILE" || fail "--new-from-file info file did not record hidden 0"
ok "created and persisted a note ($CONTENT_BASE / $(basename "$INFO_FILE"), hidden 0)"

"$XNOTE" --show || fail "--show returned non-zero"
kill -0 "$XNOTE_PID" 2>/dev/null || fail "--show killed the running instance"
ok "--show accepted, instance still running"

"$XNOTE" --quit || fail "--quit returned non-zero"
wait_gone "$XNOTE_PID" || { kill -9 "$XNOTE_PID" 2>/dev/null; fail "xnote did not exit after --quit"; }
wait "$XNOTE_PID" 2>/dev/null
RC=$?
[ "$RC" -eq 0 ] || fail "xnote exited with rc=$RC after --quit"
[ ! -e "$SOCKET" ] || fail "clean --quit left stale server socket behind at $SOCKET"
ok "clean exit after --quit (server socket removed)"

# --- phase 2: the no-tray reachability contract ---------------------------
# With no tray, closing the last visible pad must terminate the process rather
# than leave an invisible, unreachable instance holding the notes. That guard
# lives in xpad_pad_close() (xpad-pad.c) and it has been broken before -- 2.3.2
# fixed a related reachability bug in the tray-disabled startup path. Pin it.
# A restart here also proves the socket left behind by phase 1 is not fatal.
start_xnote
ok "restarted over the previous run's leftover socket"

"$XNOTE" --hide || fail "--hide returned non-zero"
wait_gone "$XNOTE_PID" || { kill -9 "$XNOTE_PID" 2>/dev/null; fail "--hide with no tray left an unreachable instance running"; }
wait "$XNOTE_PID" 2>/dev/null
RC=$?
[ "$RC" -eq 0 ] || fail "xnote exited with rc=$RC after --hide"
ok "--hide with no tray exits cleanly instead of going unreachable"

# --- phase 3: headless CLI invocation emits no GLib CRITICAL/WARNING ------
HEADLESS_ERR="$WORK/headless.err"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
	XDG_CONFIG_HOME="$WORK/headless-cfg" "$XNOTE" >"$WORK/headless.out" 2>"$HEADLESS_ERR" \
	|| fail "headless xnote invocation returned non-zero"
if grep -qE "CRITICAL|WARNING \*\*|assertion .* failed" "$HEADLESS_ERR"; then
	cat "$HEADLESS_ERR" >&2
	fail "headless xnote logged a GLib CRITICAL/WARNING"
fi
ok "headless CLI invocation exits cleanly without GLib CRITICAL/WARNING"

echo "wayland-smoke: PASS"
