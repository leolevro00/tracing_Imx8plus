#!/bin/sh
# Throttle PegExec with cgroups (v1 or v2).
# Default: 10ms CPU every 20ms (= 50% of one core) = "10ms on / 10ms off"
#
# Usage (as root, PegExec already running):
#   sh peg_cgroup_throttle.sh start
#   sh peg_cgroup_throttle.sh start 10000 20000
#   sh peg_cgroup_throttle.sh start 10000 100000
#   sh peg_cgroup_throttle.sh status
#   sh peg_cgroup_throttle.sh stop
#
# If you see "via: command not found", the file has Windows CRLF:
#   sed -i 's/\r$//' peg_cgroup_throttle.sh

set -eu

CGNAME="peg_gui_rt"
CPUS_GUI="${CPUS_GUI:-0-2}"
MEMS="${MEMS:-0}"

die() { echo "ERR: $*" >&2; exit 1; }

need_root() {
	[ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
}

detect_mode() {
	# v2 unified: /sys/fs/cgroup/cgroup.controllers
	if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
		echo v2
		return
	fi
	# v1 legacy: /sys/fs/cgroup/cpu/cpu.cfs_quota_us
	if [ -f /sys/fs/cgroup/cpu/cpu.cfs_quota_us ]; then
		echo v1
		return
	fi
	# v1 alternate mount
	if [ -f /sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us ]; then
		echo v1_alt
		return
	fi
	echo none
}

pid_peg() {
	pidof PegExec 2>/dev/null || true
}

# ---------- cgroup v2 ----------
v2_path() { echo "/sys/fs/cgroup/${CGNAME}"; }

v2_start() {
	quota_us="$1"
	period_us="$2"
	pid="$3"
	path="$(v2_path)"

	if [ -f /sys/fs/cgroup/cgroup.subtree_control ]; then
		echo "+cpu +cpuset" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || \
		echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true
	fi

	mkdir -p "${path}"

	if [ -f "${path}/cpuset.cpus" ]; then
		# empty cpuset must be initialized from parent on some kernels
		echo "${MEMS}" > "${path}/cpuset.mems" 2>/dev/null || true
		echo "${CPUS_GUI}" > "${path}/cpuset.cpus" 2>/dev/null || \
			echo "WARN: cpuset.cpus failed (continuing with cpu.max only)" >&2
	fi

	[ -f "${path}/cpu.max" ] || die "v2 detected but ${path}/cpu.max missing"
	echo "${quota_us} ${period_us}" > "${path}/cpu.max"
	echo "${pid}" > "${path}/cgroup.procs"
}

v2_stop() {
	path="$(v2_path)"
	[ -d "${path}" ] || { echo "[OK] cgroup not present"; return 0; }
	if [ -f "${path}/cgroup.procs" ]; then
		while read -r p; do
			[ -n "${p}" ] || continue
			echo "${p}" > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true
		done < "${path}/cgroup.procs"
	fi
	rmdir "${path}" 2>/dev/null || die "cannot remove ${path}"
	echo "[OK] throttle removed (v2)"
}

v2_status() {
	path="$(v2_path)"
	[ -d "${path}" ] || { echo "cgroup ${CGNAME}: inactive"; return 0; }
	echo "mode=v2 path=${path}"
	echo "cpu.max=$(cat "${path}/cpu.max")"
	[ -f "${path}/cpuset.cpus" ] && echo "cpuset.cpus=$(cat "${path}/cpuset.cpus")"
	echo "procs:"
	while read -r p; do
		[ -n "${p}" ] || continue
		cmd="$(tr '\0' ' ' < "/proc/${p}/cmdline" 2>/dev/null | cut -c1-80 || echo '?')"
		echo "  pid ${p}: ${cmd}"
	done < "${path}/cgroup.procs"
	if [ -f "${path}/cpu.stat" ]; then
		echo "cpu.stat:"
		cat "${path}/cpu.stat"
	fi
}

# ---------- cgroup v1 ----------
v1_cpu_root() {
	if [ -d /sys/fs/cgroup/cpu ]; then
		echo /sys/fs/cgroup/cpu
	else
		echo /sys/fs/cgroup/cpu,cpuacct
	fi
}

v1_start() {
	quota_us="$1"
	period_us="$2"
	pid="$3"
	cpu_root="$(v1_cpu_root)"
	cpu_path="${cpu_root}/${CGNAME}"

	mkdir -p "${cpu_path}"
	echo "${period_us}" > "${cpu_path}/cpu.cfs_period_us"
	echo "${quota_us}" > "${cpu_path}/cpu.cfs_quota_us"
	# move all threads: write pid to tasks repeatedly / use cgroup.procs if exists
	if [ -f "${cpu_path}/cgroup.procs" ]; then
		echo "${pid}" > "${cpu_path}/cgroup.procs"
	else
		echo "${pid}" > "${cpu_path}/tasks"
		# also move threads listed in /proc/pid/task
		for t in /proc/"${pid}"/task/*; do
			tid="$(basename "${t}")"
			echo "${tid}" > "${cpu_path}/tasks" 2>/dev/null || true
		done
	fi

	# optional cpuset (separate controller in v1)
	if [ -d /sys/fs/cgroup/cpuset ]; then
		cs_path="/sys/fs/cgroup/cpuset/${CGNAME}"
		mkdir -p "${cs_path}"
		# clone parent mems/cpus if empty
		if [ -f /sys/fs/cgroup/cpuset/cpuset.cpus ]; then
			cat /sys/fs/cgroup/cpuset/cpuset.mems > "${cs_path}/cpuset.mems" 2>/dev/null || echo "${MEMS}" > "${cs_path}/cpuset.mems"
			echo "${CPUS_GUI}" > "${cs_path}/cpuset.cpus" 2>/dev/null || \
				echo "WARN: cpuset pin failed" >&2
		fi
		if [ -f "${cs_path}/cgroup.procs" ]; then
			echo "${pid}" > "${cs_path}/cgroup.procs" 2>/dev/null || true
		else
			echo "${pid}" > "${cs_path}/tasks" 2>/dev/null || true
			for t in /proc/"${pid}"/task/*; do
				tid="$(basename "${t}")"
				echo "${tid}" > "${cs_path}/tasks" 2>/dev/null || true
			done
		fi
	fi
}

v1_stop() {
	cpu_root="$(v1_cpu_root)"
	cpu_path="${cpu_root}/${CGNAME}"
	if [ -d "${cpu_path}" ]; then
		listf="tasks"
		[ -f "${cpu_path}/cgroup.procs" ] && listf="cgroup.procs"
		while read -r p; do
			[ -n "${p}" ] || continue
			echo "${p}" > "${cpu_root}/${listf}" 2>/dev/null || true
		done < "${cpu_path}/${listf}"
		# reset quota to unlimited before rmdir
		echo "-1" > "${cpu_path}/cpu.cfs_quota_us" 2>/dev/null || true
		rmdir "${cpu_path}" 2>/dev/null || echo "WARN: cannot rmdir ${cpu_path}" >&2
	fi
	if [ -d "/sys/fs/cgroup/cpuset/${CGNAME}" ]; then
		cs_path="/sys/fs/cgroup/cpuset/${CGNAME}"
		listf="tasks"
		[ -f "${cs_path}/cgroup.procs" ] && listf="cgroup.procs"
		while read -r p; do
			[ -n "${p}" ] || continue
			echo "${p}" > "/sys/fs/cgroup/cpuset/${listf}" 2>/dev/null || true
		done < "${cs_path}/${listf}"
		rmdir "${cs_path}" 2>/dev/null || true
	fi
	echo "[OK] throttle removed (v1)"
}

v1_status() {
	cpu_root="$(v1_cpu_root)"
	cpu_path="${cpu_root}/${CGNAME}"
	[ -d "${cpu_path}" ] || { echo "cgroup ${CGNAME}: inactive"; return 0; }
	echo "mode=v1 path=${cpu_path}"
	echo "cpu.cfs_quota_us=$(cat "${cpu_path}/cpu.cfs_quota_us")"
	echo "cpu.cfs_period_us=$(cat "${cpu_path}/cpu.cfs_period_us")"
	if [ -f "${cpu_path}/cpu.stat" ]; then
		grep -E 'nr_throttled|throttled_time' "${cpu_path}/cpu.stat" || true
	fi
	echo "tasks/procs:"
	listf="tasks"
	[ -f "${cpu_path}/cgroup.procs" ] && listf="cgroup.procs"
	head -20 "${cpu_path}/${listf}"
	if [ -d "/sys/fs/cgroup/cpuset/${CGNAME}" ]; then
		echo "cpuset.cpus=$(cat /sys/fs/cgroup/cpuset/${CGNAME}/cpuset.cpus 2>/dev/null || true)"
	fi
}

cmd_start() {
	need_root
	quota_us="${1:-10000}"
	period_us="${2:-20000}"
	pid="$(pid_peg)"
	[ -n "${pid}" ] || die "PegExec not running - start HMI first"

	mode="$(detect_mode)"
	echo "detected cgroup mode: ${mode}"
	case "${mode}" in
		v2) v2_start "${quota_us}" "${period_us}" "${pid}" ;;
		v1|v1_alt) v1_start "${quota_us}" "${period_us}" "${pid}" ;;
		*)
			echo "ERR: no cgroup cpu controller found." >&2
			echo "Debug:" >&2
			ls -la /sys/fs/cgroup 2>&2 || true
			mount | grep -i cgroup >&2 || true
			die "cgroup cpu unavailable"
			;;
	esac

	pct=$((quota_us * 100 / period_us))
	echo "[OK] PegExec pid=${pid}"
	echo "     quota=${quota_us}us period=${period_us}us (~${pct}% of one core)"
	echo "     duty ~ ${quota_us}us ON / $((period_us - quota_us))us OFF"
	cmd_status
}

cmd_stop() {
	need_root
	mode="$(detect_mode)"
	case "${mode}" in
		v2) v2_stop ;;
		v1|v1_alt) v1_stop ;;
		*) die "no cgroup cpu controller" ;;
	esac
}

cmd_status() {
	mode="$(detect_mode)"
	case "${mode}" in
		v2) v2_status ;;
		v1|v1_alt) v1_status ;;
		*) echo "cgroup cpu: not available"; mount | grep -i cgroup || true ;;
	esac
}

# [AI] stop+start = nuovo cgroup → cpu.stat riparte da zero
cmd_reset() {
	need_root
	quota_us="${1:-10000}"
	period_us="${2:-20000}"
	cmd_stop
	cmd_start "${quota_us}" "${period_us}"
}

cmd_detect() {
	echo "mode=$(detect_mode)"
	echo "--- mounts ---"
	mount | grep -i cgroup || true
	echo "--- /sys/fs/cgroup ---"
	ls /sys/fs/cgroup 2>/dev/null || true
}

case "${1:-}" in
	start)  shift; cmd_start "$@" ;;
	stop)   cmd_stop ;;
	status) cmd_status ;;
	reset)  shift; cmd_reset "$@" ;;
	detect) cmd_detect ;;
	*)
		echo "Usage: sh $0 {start [quota_us period_us]|stop|status|reset [quota_us period_us]|detect}"
		echo "  default start/reset: 10000 20000   (10ms ON / 10ms OFF)"
		echo "  reset = stop + start (azzera cpu.stat)"
		exit 1
		;;
esac
