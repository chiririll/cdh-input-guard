#!/bin/bash
# Phase 1 checks for cdh_input_guard.
#
# Run as root in a THROWAWAY VM with the same kernel as production, never on
# the production host: a module bug panics the machine.
#
# What it does:
#   1. Baseline without the module and without the udev rule: a uinput
#      keyboard gets kbd/sysrq, Ctrl+Alt+F2 / Alt+SysRq+H take effect, and
#      systemd-logind opens its event node. Proves the checks can detect.
#   2. With the module and the rule: the same device keeps only evdev/joydev,
#      evdev still receives events, the keys have no effect on the console,
#      cdh_owner_cgroup (root only) shows the creator's cgroup, and logind
#      does not open the event node.
#   3. A device that existed before the module loaded is detached, no owner.
#   4. A handler registered after the module (sysrq, toggled off and on) is
#      detached from an existing device within the sweep interval, and never
#      reaches a new one.
#   5. A uhid keyboard is detached too and gets no owner.
#   6. Create/destroy loop and load/unload loop with a live device.
#   7. No WARNING/BUG/lockdep/KASAN lines in dmesg.
#
# Temporarily sets kernel.sysrq=1 and, unless the rule is already installed in
# /etc/udev/rules.d, copies it to /run/udev/rules.d. Both are undone on exit.
#
# Env: KO=path/to/cdh_input_guard.ko  LOOPS=200  RELOADS=20

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
KO=${KO:-$HERE/../build/cdh_input_guard.ko}
KBD=$HERE/../build/uinput_kbd
UHID=$HERE/../build/uhid_kbd
RULE_NAME=71-cdh-virtual-input.rules
RULE=$HERE/../udev/$RULE_NAME
CG=/sys/fs/cgroup/cdh-guard-test
LOOPS=${LOOPS:-200}
RELOADS=${RELOADS:-20}
MOD=cdh_input_guard
UHID_NAME=cdh-guard-test-uhid

fails=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
skip() { echo "SKIP  $*"; }
info() { echo "      $*"; }

[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
[ -f "$KO" ] && [ -x "$KBD" ] && [ -x "$UHID" ] || { echo "build first: make" >&2; exit 2; }
grep -q cgroup2 /proc/mounts || { echo "needs cgroup v2 at /sys/fs/cgroup" >&2; exit 2; }

WORK=$(mktemp -d)
old_sysrq=$(cat /proc/sys/kernel/sysrq)
old_vt=$(fgconsole 2>/dev/null || true)
rule_tmp=0

cleanup() {
	rmmod "$MOD" 2>/dev/null
	echo "$old_sysrq" > /proc/sys/kernel/sysrq
	[ -n "$old_vt" ] && chvt "$old_vt" 2>/dev/null
	if [ $rule_tmp = 1 ]; then
		rm -f /run/udev/rules.d/$RULE_NAME
		udevadm control --reload
	fi
	rmdir "$CG" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

modprobe uinput
modprobe uhid 2>/dev/null
rmmod "$MOD" 2>/dev/null
mkdir -p "$CG"
echo 1 > /proc/sys/kernel/sysrq

run_mark="cdh-guard-check-$$-$(date +%s)"
echo "$run_mark" > /dev/kmsg

kmsg_since() { dmesg | sed -n "/$1/,\$p"; }

# start_dev [uinput_kbd options] -> sets PID, SYS (inputN), EV (eventN)
# The device is created from inside $CG.
start_dev() {
	local out="$WORK/out.$RANDOM"
	sh -c 'cg=$1; shift; echo $$ > "$cg/cgroup.procs" && exec "$@"' _ \
		"$CG" "$KBD" "$@" > "$out" &
	PID=$!
	SYS= EV=
	for _ in $(seq 50); do
		[ -s "$out" ] && break
		sleep 0.1
	done
	SYS=$(head -n1 "$out")
	[ -n "$SYS" ] || return 1
	EV=$(basename "$(ls -d /sys/devices/virtual/input/"$SYS"/event* 2>/dev/null | head -n1)")
}

handlers() {
	grep -A3 "^S: Sysfs=/devices/virtual/input/$1\$" /proc/bus/input/devices |
		sed -n 's/^H: Handlers=//p'
}

# field_named NAME LETTER -> the S: or H: value of the device called NAME
field_named() {
	awk -v n="N: Name=\"$1\"" -v l="$2: " '
		$0 == n { found = 1; next }
		found && /^$/ { exit }
		found && index($0, l) == 1 { sub(/^[A-Z]: [A-Za-z]+=/, ""); print; exit }
	' /proc/bus/input/devices
}

has_word() { [[ " $1 " == *" $2 "* ]]; }

# Handlers in $1 that are not evdev, joydev or the module itself.
not_kept() {
	local w out=
	for w in $1; do
		[[ $w =~ ^(event[0-9]+|js[0-9]+|$MOD)$ ]] || out="$out $w"
	done
	echo "${out# }"
}

# 1 if systemd-logind has /dev/input/$1 open, that is if it acts on the
# device's power keys. Looks at logind itself, not at the udev database: the
# database can show the "power-switch" tag as removed while the event that
# logind received still carried it (see the comment in the udev rule).
# logind opens the node shortly after udev is done, so poll for a second.
logind_watches() {
	local pid
	udevadm settle --timeout=3 2>/dev/null
	pid=$(systemctl show -p MainPID --value systemd-logind 2>/dev/null)
	if [ -z "$pid" ] || [ "$pid" = 0 ]; then
		echo 0
		return
	fi
	for _ in $(seq 10); do
		if ls -l "/proc/$pid/fd" 2>/dev/null | grep -q -- "-> /dev/input/$1\$"; then
			echo 1
			return
		fi
		sleep 0.1
	done
	echo 0
}

# logind_check LABEL VALUE BASELINE: VALUE 1 always fails; VALUE 0 passes only
# if the baseline showed that logind does open such a device on this machine.
logind_check() {
	if [ "$2" = 1 ]; then
		fail "$1logind has the event node open: it acts on the device's power keys"
	elif [ "$3" = 1 ]; then
		pass "$1logind does not open the event node"
	else
		skip "$1logind (no baseline)"
	fi
}

# scenario NAME -> sets H, VT_SWITCHED, SYSRQ_FIRED, EV_BYTES, OWNER, OWNER_MODE, LOGIND
scenario() {
	local name=$1 mark="cdh-guard-$1-$$" vt_before vt_after attr
	echo "$mark" > /dev/kmsg

	vt_before=$(fgconsole 2>/dev/null || true)
	start_dev -w 2500 -H 1000 -v -r || { fail "$name: device not created"; return 1; }

	attr=/sys/devices/virtual/input/$SYS/cdh_owner_cgroup
	H=$(handlers "$SYS")
	OWNER=$(cat "$attr" 2>/dev/null)
	OWNER_MODE=$(stat -c %a "$attr" 2>/dev/null)
	LOGIND=$(logind_watches "$EV")
	timeout 4 cat /dev/input/"$EV" > "$WORK/ev.$name" 2>/dev/null &
	local reader=$!

	wait "$PID"
	wait "$reader"
	sleep 0.5

	vt_after=$(fgconsole 2>/dev/null || true)
	VT_SWITCHED=0
	[ -n "$vt_before" ] && [ "$vt_before" != "$vt_after" ] && VT_SWITCHED=1
	[ -n "$vt_before" ] && chvt "$vt_before" 2>/dev/null

	SYSRQ_FIRED=0
	kmsg_since "$mark" | grep -q 'sysrq: HELP' && SYSRQ_FIRED=1

	EV_BYTES=$(stat -c %s "$WORK/ev.$name")

	info "$name: $SYS handlers=[$H] vt_switched=$VT_SWITCHED sysrq_fired=$SYSRQ_FIRED evdev_bytes=$EV_BYTES logind=$LOGIND owner=${OWNER:-none}"
}

# uhid_scenario -> sets UH (handlers), USYS (sysfs path), ULOGIND; device stays for a moment
uhid_scenario() {
	UH= USYS= ULOGIND=0
	"$UHID" -n "$UHID_NAME" -H 4000 > /dev/null &
	UPID=$!
	for _ in $(seq 30); do
		USYS=$(field_named "$UHID_NAME" S)
		[ -n "$USYS" ] && break
		sleep 0.1
	done
	[ -n "$USYS" ] || { wait "$UPID"; return 1; }
	sleep 0.3
	local ev
	ev=$(basename "$(ls -d /sys"$USYS"/event* 2>/dev/null | head -n1)")
	[ -n "$ev" ] && ULOGIND=$(logind_watches "$ev")
	UH=$(field_named "$UHID_NAME" H)
	info "uhid: $USYS handlers=[$UH] logind=$ULOGIND"
}

echo "== 1. baseline, module not loaded, udev rule not installed"
if [ -e /etc/udev/rules.d/$RULE_NAME ]; then
	info "$RULE_NAME is already installed; the logind baseline cannot be taken"
	cmp -s "$RULE" /etc/udev/rules.d/$RULE_NAME ||
		info "the installed rule differs from udev/$RULE_NAME; the installed one is tested"
fi
scenario baseline || exit 1
base_kbd=0 base_sysrq=0 base_vt=$VT_SWITCHED base_rq=$SYSRQ_FIRED base_logind=$LOGIND
has_word "$H" kbd && base_kbd=1
has_word "$H" sysrq && base_sysrq=1
[ $base_kbd = 1 ] || info "baseline: kbd not attached; kbd checks are not meaningful"
[ $base_vt = 1 ] || info "baseline: Ctrl+Alt+F2 did not switch VT (no VT console?); VT check skipped"
[ $base_rq = 1 ] || info "baseline: Alt+SysRq+H did not fire; SysRq check skipped"
[ $base_logind = 1 ] || info "baseline: logind did not open the event node; logind checks skipped"

base_uhid=0 base_uhid_logind=0
if [ -c /dev/uhid ] && uhid_scenario; then
	has_word "$UH" kbd && base_uhid=1
	base_uhid_logind=$ULOGIND
	wait "$UPID"
fi
[ $base_uhid = 1 ] || info "baseline: no uhid keyboard with kbd attached; uhid checks skipped"

if [ ! -e /etc/udev/rules.d/$RULE_NAME ]; then
	mkdir -p /run/udev/rules.d
	cp "$RULE" /run/udev/rules.d/$RULE_NAME
	udevadm control --reload
	rule_tmp=1
fi

echo "== 2. module loaded, new device"
insmod "$KO" || { fail "insmod"; exit 1; }
scenario guarded || exit 1

extra=$(not_kept "$H")
if [ -z "$extra" ]; then pass "only evdev/joydev stay attached"; else fail "handlers still attached: $extra"; fi
if [ -n "$EV" ] && has_word "$H" "$EV"; then pass "evdev attached ($EV)"; else fail "evdev not attached"; fi
if [ "$EV_BYTES" -gt 0 ]; then pass "evdev received events ($EV_BYTES bytes)"; else fail "evdev received no events"; fi
if [ $base_vt = 1 ]; then
	if [ $VT_SWITCHED = 0 ]; then pass "Ctrl+Alt+F2 did not switch VT"; else fail "Ctrl+Alt+F2 switched VT"; fi
else
	skip "VT switch"
fi
if [ $base_rq = 1 ]; then
	if [ $SYSRQ_FIRED = 0 ]; then pass "Alt+SysRq+H had no effect"; else fail "Alt+SysRq+H fired"; fi
else
	skip "SysRq"
fi
if [ "$OWNER" = "/${CG#/sys/fs/cgroup/}" ]; then pass "owner = $OWNER"; else fail "owner = '${OWNER}', expected /${CG#/sys/fs/cgroup/}"; fi
if [ "$OWNER_MODE" = 400 ]; then pass "owner attribute is root only"; else fail "owner attribute mode is '${OWNER_MODE}', expected 400"; fi
logind_check "" "$LOGIND" "$base_logind"

echo "== 3. device created before the module loaded"
rmmod "$MOD"
start_dev -w 0 -H 3000 || fail "device not created"
sleep 0.3
insmod "$KO" || { fail "insmod"; exit 1; }
H=$(handlers "$SYS")
info "$SYS handlers=[$H]"
extra=$(not_kept "$H")
if [ -z "$extra" ]; then pass "pre-existing device detached"; else fail "pre-existing device keeps: $extra"; fi
if [ -e /sys/devices/virtual/input/"$SYS"/cdh_owner_cgroup ]; then fail "pre-existing device has an owner attribute"; else pass "pre-existing device has no owner"; fi
wait "$PID"

echo "== 4. handler registered after the module (sysrq off, then on)"
sweep=$(cat /sys/module/$MOD/parameters/sweep_ms)
if [ $base_sysrq != 1 ]; then
	skip "sysrq handler does not attach on this kernel"
elif [ "$sweep" = 0 ]; then
	skip "sweep_ms=0"
else
	[ "$sweep" -lt 100 ] && sweep=100
	start_dev -w 0 -H $((sweep * 2 + 3000)) || fail "device not created"
	echo 0 > /proc/sys/kernel/sysrq
	echo 1 > /proc/sys/kernel/sysrq
	sleep "$(awk "BEGIN { print ($sweep * 2 + 500) / 1000 }")"
	H=$(handlers "$SYS")
	info "$SYS handlers=[$H]"
	if has_word "$H" sysrq; then fail "late sysrq still attached to the existing device"; else pass "late sysrq detached from the existing device"; fi
	wait "$PID"

	scenario late
	if has_word "$H" sysrq; then fail "late sysrq attached to a new device"; else pass "late sysrq not attached to a new device"; fi
	if [ $base_rq = 1 ]; then
		if [ $SYSRQ_FIRED = 0 ]; then pass "Alt+SysRq+H had no effect"; else fail "Alt+SysRq+H fired"; fi
	fi
fi

echo "== 5. uhid keyboard"
if [ $base_uhid != 1 ]; then
	skip "uhid"
elif uhid_scenario; then
	extra=$(not_kept "$UH")
	if [ -z "$extra" ]; then pass "uhid: only evdev/joydev stay attached"; else fail "uhid: handlers still attached: $extra"; fi
	if [ -e /sys"$USYS"/cdh_owner_cgroup ]; then fail "uhid device has an owner attribute"; else pass "uhid device has no owner"; fi
	logind_check "uhid: " "$ULOGIND" "$base_uhid_logind"
	wait "$UPID"
else
	fail "uhid device not created"
fi

echo "== 6a. create/destroy $LOOPS devices"
ok=1
for _ in $(seq "$LOOPS"); do
	"$KBD" -w 0 -H 0 > /dev/null || { ok=0; break; }
done
[ $ok = 1 ] && pass "create/destroy loop" || fail "create/destroy loop"

echo "== 6b. load/unload $RELOADS times with a live device"
ok=1
for _ in $(seq "$RELOADS"); do
	rmmod "$MOD" || { ok=0; break; }
	insmod "$KO" || { ok=0; break; }
	start_dev -w 0 -H 300 || { ok=0; break; }
	[ -e /sys/devices/virtual/input/"$SYS"/cdh_owner_cgroup ] || { ok=0; break; }
	rmmod "$MOD" || { ok=0; break; }
	[ -e /sys/devices/virtual/input/"$SYS"/cdh_owner_cgroup ] && { ok=0; break; }
	insmod "$KO" || { ok=0; break; }
	wait "$PID"
done
[ $ok = 1 ] && pass "load/unload loop" || fail "load/unload loop"

echo "== 7. dmesg"
bad=$(kmsg_since "$run_mark" | grep -E 'WARNING:|BUG:|Oops|lockdep|possible (circular|recursive) locking|KASAN|UBSAN|suspicious RCU|list_(add|del) corruption')
if [ -z "$bad" ]; then pass "no kernel warnings"; else fail "kernel warnings:"; echo "$bad"; fi

echo
if [ $fails = 0 ]; then echo "ALL PASSED"; else echo "$fails FAILED"; fi
echo "Manual checks left, VM only. This script has unloaded the module, so first:"
echo "  insmod build/cdh_input_guard.ko"
if [ $rule_tmp = 1 ]; then
	echo "and it has removed its temporary udev rule, so also:"
	echo "  cp udev/$RULE_NAME /etc/udev/rules.d/ && udevadm control --reload"
elif ! cmp -s "$RULE" /etc/udev/rules.d/$RULE_NAME; then
	echo "and the installed udev rule is not the one in this tree, so also:"
	echo "  cp udev/$RULE_NAME /etc/udev/rules.d/ && udevadm control --reload"
fi
echo "then:"
echo "  build/uinput_kbd -t at the tty1 login prompt (text must not appear)"
echo "  build/uinput_kbd -d with ctrl-alt-del.target unmasked (must not reboot)"
echo "  build/uinput_kbd -p with the udev rule installed (must not power off)"
[ $fails = 0 ]
