#!/usr/bin/env bash
# clientlink_e2e.sh: end-to-end test of the client link (docs/CLIENT_PROTOCOL.md).
#
#   A (0 km) --- B (1.5 km) --- C (3 km)      all on the UDP virtual radio
#
# Each daemon runs with --client-tcp 127.0.0.1:PORT. clientlink_e2e.py then
# drives the nodes only through that link: A's client DMs B's client and gets
# the delivery receipt, A DMs C while C has no client (replayed on connect),
# A's client raises an SOS that B's client sees and acks, plus the error paths.
# Also checks that the stdin commands still work alongside the link.
#
# Usage: firmware/pi/tools/clientlink_e2e.sh [path/to/ghostwaved]
# Env:   CLIENTLINK_PORT  multicast port (default 47750)
#        CLIENTLINK_TCP   first client TCP port (default 47810; uses 3)
#        VMESH_IF         multicast interface address, e.g. 127.0.0.1
#        CLIENTLINK_KEEP=1 keeps the work dir
set -u

here=$(cd "$(dirname "$0")" && pwd)
fw=$(cd "$here/../.." && pwd)
bin=${1:-$fw/build/ghostwaved}
if [ ! -x "$bin" ]; then
  echo "== building $bin (GHOSTWAVE_BUILD_VIRT=ON)"
  cmake -S "$fw" -B "$fw/build" -DGHOSTWAVE_BUILD_VIRT=ON >/dev/null &&
    cmake --build "$fw/build" --target ghostwaved --parallel >/dev/null || exit 1
fi
command -v python3 >/dev/null || { echo "needs python3"; exit 1; }

work=$(mktemp -d "${TMPDIR:-/tmp}/clientlink.XXXXXX")
port=${CLIENTLINK_PORT:-47750}
tcp=${CLIENTLINK_TCP:-47810}
pids=""
keep=${CLIENTLINK_KEEP:-0}

cleanup() {
  for p in $pids; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  if [ "$keep" = 1 ]; then echo "logs kept in $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

lat_at() { awk -v k="$1" 'BEGIN { printf "%.7f", 46.87 + k * 0.0089932 }'; }

fdnum=3
start() {   # start NAME KM TCPPORT
  local n=$1 km=$2 cport=$3
  local args=(--radio udp --mcast "239.77.0.1:$port" --key "$work/$n.key" --name "$n"
              --lat "$(lat_at "$km")" --lon -113.99 --client-tcp "127.0.0.1:$cport")
  [ -n "${VMESH_IF:-}" ] && args+=(--vif "$VMESH_IF")
  mkfifo "$work/$n.in"
  "$bin" "${args[@]}" <"$work/$n.in" >"$work/$n.log" 2>&1 &
  pids="$pids $!"
  eval "exec $fdnum>\"\$work/\$n.in\""
  eval "fd_$n=$fdnum"
  fdnum=$((fdnum + 1))
}
wait_for() {   # wait_for SECONDS NAME PATTERN
  local end=$(( $(date +%s) + $1 ))
  while [ "$(date +%s)" -lt "$end" ]; do
    grep -q "$3" "$work/$2.log" && return 0
    sleep 0.2
  done
  return 1
}

echo "work dir $work, group 239.77.0.1:$port, client ports $tcp-$((tcp + 2))"
start A 0 "$tcp"; sleep 0.5
start B 1.5 $((tcp + 1)); sleep 0.5
start C 3 $((tcp + 2))
for n in A B C; do
  if ! wait_for 5 $n "^client link on tcp"; then
    echo "FAIL: $n did not open its client link"; cat "$work/$n.log"; keep=1; exit 1
  fi
done
grep -h "^client link on tcp" "$work"/*.log | sed 's/^/    /'

# the stdin interface still works next to the link
echo stats >&"$fd_A"
if ! wait_for 5 A "^tx "; then echo "FAIL: stdin 'stats' got no answer"; keep=1; exit 1; fi
echo "stdin commands still answered: $(grep '^tx ' "$work/A.log" | tail -1 | cut -c1-40)..."

rc=0
python3 "$here/clientlink_e2e.py" "$work" "127.0.0.1:$tcp" "127.0.0.1:$((tcp + 1))" "127.0.0.1:$((tcp + 2))" || rc=$?
if [ $rc != 0 ]; then
  keep=1
  for n in A B C; do echo "--- $n.log (tail)"; tail -25 "$work/$n.log"; done
fi
exit $rc
