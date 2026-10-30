#!/system/bin/sh
# aod_idled: switch the CO5300 panel to idle mode (AOD) while the framework is
# Dozing and back on wake. Replaces the Magisk "wearit_aod" helper on DW100U.
# Interface: /sys/kernel/aod_idle/{idle,idle_brightness} (sprd_aod_idle.c).
# Wake by double tap needs nothing here: the driver emits KEY_WAKEUP from
# "aod_dtap", the framework wakes, and the wake event below writes idle=0
# (the driver also forgets idle on the display RUN event).
#
# Signal: the event log (blocking logcat read, no polling) tags
# power_screen_state and screen_toggled. An event only triggers a re-read of
# mWakefulness from "dumpsys power"; the event value is never trusted.
# Fallback: setprop persist.sys.aod_idled.poll <seconds> polls instead.
# Properties: persist.sys.aod_brightness (0..255, optional idle brightness).
# Plain POSIX sh for Android 8.1 mksh/toybox.

TAG=aod_idled
D=/sys/kernel/aod_idle
POLL=$(getprop persist.sys.aod_idled.poll)

logi() { log -t "$TAG" -p i "$*"; }
loge() { log -t "$TAG" -p e "$*"; }

cleanup() {
	# never leave the panel idle (touch swallowed) when the service stops
	echo 0 > "$D/idle" 2>/dev/null
	logi "stopping"
	exit 0
}
trap cleanup TERM INT HUP

# oneshot service: log once and stop, no restart loop
[ -w "$D/idle" ] || { loge "$D/idle missing or not writable (kernel/chown/SELinux?), exiting"; exit 0; }

wakefulness() {
	dumpsys power 2>/dev/null | grep -m1 'mWakefulness=' | sed 's/.*mWakefulness=//' | tr -d '\r \t'
}

apply_brightness() {
	b=$(getprop persist.sys.aod_brightness)
	case "$b" in
	''|*[!0-9]*) ;;
	*) [ "$b" -le 255 ] && echo "$b" > "$D/idle_brightness" 2>/dev/null ;;
	esac
}

set_idle() {	# $1 = 0|1 ; the driver refuses (EIO) while the panel is powered down
	if [ "$(cat $D/idle 2>/dev/null)" != "$1" ]; then
		echo "$1" > "$D/idle" 2>/dev/null || return 1
		logi "idle=$1"
	fi
	return 0
}

# Debounce 1 s (2-3 events per transition), then re-read the state and act.
# Dozing: the framework's own doze brightness (DCS 0x51) lands about 1 s after
# doze starts and would overwrite idle_brightness, so write idle_brightness
# after that settle, every time (also when idle is already 1). If the panel/DSI
# is runtime-suspended the driver returns EIO: retry 4x 1 s, then once more
# after 15 s; the next event re-arms everything.
reconcile() {
	sleep 1
	n=0
	while [ $n -lt 6 ]; do
		case "$(wakefulness)" in
		Awake) set_idle 0; return 0 ;;
		Dozing)
			if set_idle 1; then apply_brightness; return 0; fi ;;
		Asleep) return 0 ;;	# screen off: driver keeps idle state, only a double tap wakes
		esac
		n=$((n + 1))
		if [ $n -eq 4 ]; then sleep 15; else sleep 1; fi
	done
	loge "gave up (panel powered down? see status panel_powered), waiting for next event"
	return 1
}

logi "start, poll='${POLL:-event}'"
reconcile

fails=0
while :; do
	if [ -n "$POLL" ] && [ "$POLL" -gt 0 ] 2>/dev/null; then
		sleep "$POLL"
		reconcile
		continue
	fi
	start=$(date +%s)
	# -T 1: no old backlog; blocks in the kernel until the next event
	logcat -b events -T 1 power_screen_state:I screen_toggled:I '*:S' 2>/dev/null |
	while read -r line; do
		reconcile
	done
	# logd restarted or logcat missing: back off, never spin
	[ $(( $(date +%s) - start )) -lt 5 ] && fails=$((fails + 1)) || fails=0
	[ $fails -gt 6 ] && fails=6
	loge "logcat ended, retry in $((fails * 5 + 5)) s"
	sleep $((fails * 5 + 5))
	reconcile
done
