#!/usr/bin/env bash
# vmesh.sh: four ghostwaved daemons on the UDP virtual radio, no hardware.
#
#   A (0 km) --- B (1.5 km)  . . . gap . . .  C (9 km) --- D (10.5 km)
#
# The chain is partitioned: nothing in {A, B} can hear {C, D}. B is a hiker
# whose position comes from a fake NMEA file. A sends an encrypted DM to D
# and raises an SOS; B picks both up, walks to 8 km, and carries them over
# the gap. The script passes when D prints the DM and both C and D hold the SOS.
#
# Usage: firmware/pi/tools/vmesh.sh [path/to/ghostwaved]
# Env:   VMESH_PORT (default 47700), VMESH_IF (multicast interface address,
#        e.g. 127.0.0.1), VMESH_KEEP=1 keeps the work dir, VMESH_VLOG=1 logs
#        every frame each node hears, VMESH_TRIES (default 3) attempts.
#
# Retries: the virtual radio models hidden terminals, and now and then the
# one SOS offer B makes to C is lost in a collision. The router does not
# re-offer to a known neighbor for 15 min, so that attempt cannot pass in
# time (exit 3). Only that case is retried; any other failure stops at once.
set -u

here=$(cd "$(dirname "$0")" && pwd)
fw=$(cd "$here/../.." && pwd)
bin=${1:-$fw/build/ghostwaved}
if [ ! -x "$bin" ]; then
  echo "== building $bin (GHOSTWAVE_BUILD_VIRT=ON)"
  cmake -S "$fw" -B "$fw/build" -DGHOSTWAVE_BUILD_VIRT=ON >/dev/null &&
    cmake --build "$fw/build" --target ghostwaved --parallel >/dev/null || exit 1
fi

if [ -z "${VMESH_INNER:-}" ]; then
  tries=${VMESH_TRIES:-3}
  for (( try = 1; try <= tries; try++ )); do
    [ $tries -gt 1 ] && echo "== attempt $try of $tries"
    VMESH_INNER=1 "$0" "$bin"
    rc=$?
    [ $rc -ne 3 ] && exit $rc
    echo "== attempt $try: SOS offer lost to a collision, not re-offered within the run (router gap)"
  done
  exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/vmesh.XXXXXX")
port=${VMESH_PORT:-47700}
names="A B C D"
pids=""
t0=$(date +%s)

cleanup() {
  for p in $pids; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  if [ "${VMESH_KEEP:-0}" = 1 ]; then echo "logs kept in $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say() { printf '[%3ss] %s\n' "$(( $(date +%s) - t0 ))" "$*"; }

# Positions along a meridian, 1 km = 0.0089932 degrees of latitude.
lat_at() { awk -v k="$1" 'BEGIN { printf "%.7f", 46.87 + k * 0.0089932 }'; }
LON=-113.99

# $GPGGA for (lat, LON) with a valid checksum.
gga() {
  local body c i sum=0
  body=$(awk -v la="$1" -v lo="$LON" 'BEGIN {
    ns = la < 0 ? "S" : "N"; ew = lo < 0 ? "W" : "E"
    la = la < 0 ? -la : la; lo = lo < 0 ? -lo : lo
    printf "GPGGA,120000.00,%02d%08.5f,%s,%03d%08.5f,%s,1,08,1.0,1500.0,M,0.0,M,,",
      int(la), (la - int(la)) * 60, ns, int(lo), (lo - int(lo)) * 60, ew }')
  for (( i = 0; i < ${#body}; i++ )); do
    c=$(printf '%d' "'${body:i:1}")
    sum=$(( sum ^ c ))
  done
  printf '$%s*%02X\r\n' "$body" "$sum"
}

# start NAME ID KM [extra args...]   (KM "nmea" = position from $work/NAME.nmea)
fdnum=3
start() {
  local n=$1 id=$2 km=$3
  shift 3
  local args=(--radio udp --mcast "239.77.0.1:$port" --id "$id" --key "$work/$n.key")
  [ -n "${VMESH_IF:-}" ] && args+=(--vif "$VMESH_IF")
  [ "${VMESH_VLOG:-0}" = 1 ] && args+=(--vlog)
  if [ "$km" = nmea ]; then args+=(--nmea "$work/$n.nmea"); else args+=(--lat "$(lat_at "$km")" --lon "$LON"); fi
  mkfifo "$work/$n.in"
  "$bin" "${args[@]}" "$@" <"$work/$n.in" >"$work/$n.log" 2>&1 &
  pids="$pids $!"
  eval "exec $fdnum>\"\$work/\$n.in\""   # hold the FIFO open: the daemon's stdin
  eval "fd_$n=$fdnum"
  fdnum=$((fdnum + 1))
}
cmd() {   # cmd NAME text...
  local n=$1 fd
  shift
  eval "fd=\$fd_$n"
  echo "$*" >&"$fd"
}
has() { grep -q "$2" "$work/$1.log"; }
wait_for() {   # wait_for SECONDS NAME PATTERN
  local end=$(( $(date +%s) + $1 ))
  while [ "$(date +%s)" -lt "$end" ]; do
    has "$2" "$3" && return 0
    sleep 1
  done
  return 1
}
show() { grep -E "$2" "$work/$1.log" | sed "s/^/    $1: /"; }
last() { grep -E "$2" "$work/$1.log" | tail -1 | sed "s/^/    $1: /"; }

ID_A=0a000001 ID_B=0b000002 ID_C=0c000003 ID_D=0d000004

say "work dir $work, group 239.77.0.1:$port"
gga "$(lat_at 1.5)" >"$work/B.nmea"
# staggered, so the four start-up beacons do not all collide
start A $ID_A 0; sleep 1
start B $ID_B nmea --responder; sleep 1
start C $ID_C 9; sleep 1
start D $ID_D 10.5 --require-enc

for n in $names; do
  wait_for 5 $n "^key " || { say "FAIL: $n did not start"; cat "$work/$n.log"; exit 1; }
done
pub_A=$(awk '/^key /{print substr($2,1,64); exit}' "$work/A.log")
pub_D=$(awk '/^key /{print substr($2,1,64); exit}' "$work/D.log")
cmd A peer $ID_D "$pub_D"
cmd D peer $ID_A "$pub_A"
say "A and D swapped public keys; D refuses plaintext (--require-enc)"

say "waiting for start-up beacons"
sleep 6
for n in $names; do cmd $n stats; done
sleep 1
for n in $names; do last $n "^tx "; done

say "A -> D: encrypted DM (D's position is known; nothing on A's side can reach it)"
cmd A known $ID_D "$(lat_at 10.5)" $LON
cmd A send $ID_D "hello D, sealed for you only"
say "A: SOS"
cmd A sos "twisted ankle at the creek"
wait_for 20 B "SOS from $ID_A" && say "B heard the SOS (and, as a responder, acked it)"
sleep 8
cmd B stats
sleep 1
last B "^tx "
if has C "SOS from" || has D "MSG from"; then say "note: something crossed the gap early"; fi

say "B walks north from 1.5 km to 8 km"
for km in 2.0 2.5 3.0 3.5 4.0 4.5 5.0 5.5 6.0 6.5 7.0 7.5 8.0; do
  gga "$(lat_at $km)" >>"$work/B.nmea"
  sleep 2
done
say "B is at 8 km: 1 km from C, 2.5 km from D, 8 km from A"

ok=1
wait_for 150 D "MSG from $ID_A" || ok=0
wait_for 30 D "SOS from $ID_A" || ok=0
wait_for 5 C "SOS from $ID_A" || ok=0
for n in $names; do cmd $n stats; done
sleep 1

echo
say "results"
show A "^(SOS on|SOSACK|DELIVERED)"
show B "^SOS from"
show C "^SOS from"
show D "^(MSG from|SOS from)"
for n in $names; do last $n "^tx "; last $n "^vradio heard"; done
echo "    (A gets no DELIVERED: D's receipt is stored on the far side until someone walks back)"
echo
if [ $ok = 1 ]; then
  say "PASS: the encrypted DM reached D and the SOS reached C and D across the partition"
  exit 0
fi
has D "MSG from" || say "FAIL: D never got the DM"
has D "SOS from" || say "FAIL: D never got the SOS"
has C "SOS from" || say "FAIL: C never got the SOS"
VMESH_KEEP=1
has D "MSG from" && exit 3   # only the SOS was stuck: see "Retries" above
exit 1
