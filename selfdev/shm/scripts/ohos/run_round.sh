#!/bin/sh
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# One bench round on the device (run_shm_ohos.sh pushes this next to the bench):
#   sh run_round.sh <timeout s> <hex cpu mask|-> <pid file> <bench> <args...>
# Runs `[taskset <mask>] /bin/time -p <bench> <args...>` in the background, so that
# time is directly above the bench (toybox timeout does not pass the CPU time of
# its child on) and the round has its own timeout: after <timeout s> it kills the
# process tree of time (time and the bench, and whatever the bench started) by
# pid, never by name. The pid of time goes to <pid file> while it runs.
# Exit status: that of the bench (time passes it on: exit n gives n, a signal s
# gives s), 124 after the timeout. POSIX sh, toybox only.
t=$1 mask=$2 pidfile=$3
shift 3
case "${t}" in '' | *[!0-9]*) echo "run_round.sh: bad timeout '${t}'" >&2; exit 64 ;; esac
rm -f "${pidfile}.timeout"
if [ "${mask}" = - ]; then
  /bin/time -p "$@" &
else
  taskset "${mask}" /bin/time -p "$@" &
fi
p=$!
echo "${p}" >"${pidfile}"
me=$$

# ppid_map: "pid ppid" of every process, without forking per process (the device has
# hundreds and the kill must not be seconds late): ppid is field 2 after the ") "
# that ends comm.
ppid_map() {
  for d in /proc/[0-9]*; do
    read -r l <"${d}/stat" 2>/dev/null || continue
    l=${l##*) }
    set -- ${l}
    echo "${d#/proc/} $2"
  done
}

(
  i=0
  while [ "${i}" -lt "${t}" ]; do
    sleep 1 >/dev/null 2>&1
    i=$((i + 1))
    kill -0 "${p}" 2>/dev/null || exit 0
  done
  # Still ours (parent is this script)? A reaped pid could have been reused.
  map=$(ppid_map)
  [ "$(echo "${map}" | awk -v p="${p}" '$1 == p { print $2 }')" = "${me}" ] || exit 0
  tree=$(echo "${map}" | awk -v root="${p}" '
    { parent[$1] = $2 }
    END {
      keep[root] = 1
      do { more = 0; for (x in parent) if (!(x in keep) && (parent[x] in keep)) { keep[x] = 1; more = 1 } } while (more)
      for (x in keep) printf "%s ", x
    }')
  # The round may have ended during the scan: then it did not time out.
  kill -0 "${p}" 2>/dev/null || exit 0
  : >"${pidfile}.timeout"
  echo "ROUND_TIMEOUT after ${t} s: kill -9 ${tree}"
  kill -STOP ${tree} 2>/dev/null
  kill -9 ${tree} 2>/dev/null
) &
w=$!
wait "${p}"
st=$?
# Do not wait for the watchdog's sleep: it would add up to 1 s to the round window.
kill "${w}" 2>/dev/null
wait "${w}" 2>/dev/null
[ -f "${pidfile}.timeout" ] && st=124
rm -f "${pidfile}" "${pidfile}.timeout"
exit "${st}"
