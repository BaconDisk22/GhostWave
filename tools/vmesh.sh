#!/usr/bin/env bash
# Four ghostwaved daemons on the UDP virtual radio, no hardware:
#
#   A (0 km) ── B (2 km, hiker) ......... C (9 km) ── D (11 km)
#
# The chain is partitioned: radio range is ~3.3 km, so A/B cannot reach C/D.
# A sends D an encrypted DM and raises an SOS. B (a responder) stores both,
# then walks north to 7.5 km (fake NMEA), meets C, and the DM and SOS reach D.
# B then walks back, so D's end-to-end ACK can reach A (best effort, not
# checked). A sniffer on the multicast group checks the DM text never went
# over the air in clear.
#
# Usage: tools/vmesh.sh [--keep] [--port N] [--vif ADDR]
#   --keep   leave the work directory (logs, keys, captured frames) in place
#   --vif    local address for multicast, e.g. 127.0.0.1 on a host without a LAN
# Env: GHOSTWAVED=path/to/ghostwaved (default: build firmware/build-virt)
# Takes about five minutes: fixed nodes beacon rarely, B at most once a minute.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
GROUP=239.77.0.1
PORT=47791        # not the default 47700, so a demo never mixes with a real virtual mesh
VIF=""
KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --keep) KEEP=1 ;;
    --port) PORT=$2; shift ;;
    --vif) VIF=$2; shift ;;
    *) echo "usage: $0 [--keep] [--port N] [--vif ADDR]" >&2; exit 2 ;;
  esac
  shift
done

BIN=${GHOSTWAVED:-}
if [ -z "$BIN" ]; then
  BIN="$ROOT/firmware/build-virt/ghostwaved"
  echo "== building ghostwaved (virtual radio only) in firmware/build-virt"
  cmake -S "$ROOT/firmware" -B "$ROOT/firmware/build-virt" -DGHOSTWAVE_BUILD_VIRT=ON >/dev/null
  cmake --build "$ROOT/firmware/build-virt" --target ghostwaved --parallel >/dev/null
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/vmesh.XXXXXX")
PIDS=()
cleanup() {
  exec 3>&- 4>&- 5>&- 6>&- 2>/dev/null || true
  for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  if [ "$KEEP" = 1 ]; then echo "work directory kept: $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ID_A=a0000001 ID_B=b0000002 ID_C=c0000003 ID_D=d0000004
LAT0=46.5 LON0=-114.0
lat_at() { awk -v k="$1" -v l="$LAT0" 'BEGIN { printf "%.6f", l + k / 111.195 }'; }

# One $GPGGA sentence for a point `km` north of the origin, checksummed.
nmea_at() {
  local body sum=0 i c
  body=$(awk -v k="$1" -v l="$LAT0" -v o="$LON0" 'BEGIN {
    la = l + k / 111.195; lo = -o
    printf "GPGGA,120000.00,%02d%07.4f,N,%03d%07.4f,W,1,08,0.9,1500.0,M,,M,,",
           int(la), (la - int(la)) * 60, int(lo), (lo - int(lo)) * 60 }')
  for ((i = 0; i < ${#body}; i++)); do
    printf -v c '%d' "'${body:i:1}"
    sum=$((sum ^ c))
  done
  printf '$%s*%02X\r\n' "$body" "$sum"
}

# start NAME ID FD [ghostwaved args...]: stdin from a FIFO held open on FD
start() {
  local name=$1 id=$2 fd=$3
  shift 3
  mkfifo "$WORK/$name.in"
  "$BIN" --radio udp --mcast "$GROUP:$PORT" ${VIF:+--vif "$VIF"} --id "$id" \
    --key "$WORK/$name.key" "$@" <"$WORK/$name.in" >"$WORK/$name.log" 2>&1 &
  PIDS+=($!)
  eval "exec $fd>\"\$WORK/\$name.in\""
}
say() { local fd=$1; shift; echo "$*" >&"$fd"; }

# wait_for NAME REGEX SECONDS: true once NAME's log has a matching line
wait_for() {
  local t=0
  while ! grep -Eq "$2" "$WORK/$1.log" 2>/dev/null; do
    [ "$t" -ge "$3" ] && return 1
    sleep 1
    t=$((t + 1))
  done
}
show() { grep -E "$2" "$WORK/$1.log" | sed "s/^/  $1: /"; }
last() { grep -E "$2" "$WORK/$1.log" | tail -n 1 | sed "s/^/  $1: /"; }
fail() { echo "FAIL: $*" >&2; for n in A B C D; do echo "--- $n.log" >&2; tail -n 25 "$WORK/$n.log" >&2; done; exit 1; }

# Sniffer: every datagram on the group, appended raw, so we can grep the air.
python3 - "$GROUP" "$PORT" "$WORK/air.bin" "${VIF:-0.0.0.0}" <<'EOF' &
import socket, struct, sys
group, port, out, vif = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if hasattr(socket, "SO_REUSEPORT"):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
s.bind(("", port))
s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
             socket.inet_aton(group) + socket.inet_aton(vif))
with open(out, "ab", buffering=0) as f:
    while True:
        f.write(s.recv(512))
EOF
PIDS+=($!)

echo "== starting 4 daemons on $GROUP:$PORT (work dir $WORK)"
nmea_at 2 >"$WORK/b.nmea"
start A $ID_A 3 --lat "$(lat_at 0)" --lon $LON0
start B $ID_B 4 --nmea "$WORK/b.nmea" --responder
start C $ID_C 5 --lat "$(lat_at 9)" --lon $LON0
start D $ID_D 6 --lat "$(lat_at 11)" --lon $LON0 --require-enc
for n in A B C D; do wait_for $n '^key ' 10 || fail "$n did not start"; done

# A and D swap public keys, as two users would with the `keys` command
say 3 keys
say 6 keys
wait_for A "^$ID_A [0-9a-f]{64}\$" 5 || fail "no key from A"
wait_for D "^$ID_D [0-9a-f]{64}\$" 5 || fail "no key from D"
say 3 "peer $(grep -E "^$ID_D [0-9a-f]{64}\$" "$WORK/D.log")"
say 6 "peer $(grep -E "^$ID_A [0-9a-f]{64}\$" "$WORK/A.log")"
sleep 8   # first beacons

SECRET="meet at the saddle at 3pm"
echo "== A -> D encrypted DM: \"$SECRET\"  (D is 11 km away, no path)"
say 3 "known $ID_D $(lat_at 11) $LON0"
say 3 "send $ID_D $SECRET"
echo "== A raises an SOS"
say 3 "sos broken leg"
wait_for B "^SOS from $ID_A" 30 || fail "B never heard A's SOS"
show B "^SOS from"
wait_for A "^SOSACK from $ID_B" 30 && show A "^SOSACK"
sleep 10
say 4 stats
sleep 1
show B "^tx "
grep -Eq "MSG|SOS from" "$WORK/D.log" && fail "D got something before B moved: chain not partitioned?"
echo "   D has nothing yet (partitioned)"

echo "== B walks north from 2 km to 7.5 km"
for km in 2.5 3 3.5 4 4.5 5 5.5 6 6.5 7 7.5; do
  nmea_at "$km" >>"$WORK/b.nmea"
  sleep 2
done
echo "== waiting for C/D to meet B (beacons are at least 60 s apart)"
wait_for D "^MSG from $ID_A" 240 || fail "DM never reached D"
show D "^MSG from"
wait_for D "^SOS from $ID_A" 120 || fail "SOS never reached D"
show D "^SOS from"
show C "^SOS from"

echo "== B walks back to A with D's end-to-end ACK"
for km in 7 6.5 6 5.5 5 4.5 4 3.5 3 2.5 2; do
  nmea_at "$km" >>"$WORK/b.nmea"
  sleep 2
done
if wait_for A "^DELIVERED to $ID_D" 90; then show A "^DELIVERED"; else echo "  (no ACK back to A within 90 s; best effort, not checked)"; fi

echo "== on the air"
for fd in 3 4 5 6; do say $fd stats; done
sleep 1
for n in A B C D; do last $n "^tx "; last $n "^vradio "; done
frames=$(LC_ALL=C grep -c -a 'GWV1' "$WORK/air.bin" || true)
if LC_ALL=C grep -q -a -F "$SECRET" "$WORK/air.bin"; then
  fail "DM text was on the air in clear"
fi
echo "  sniffer: $(wc -c <"$WORK/air.bin" | tr -d ' ') bytes captured; DM text never in clear"
LC_ALL=C grep -q -a -F "broken leg" "$WORK/air.bin" && echo "  (SOS text is sent in clear by design, DESIGN §8)"
echo "PASS: encrypted DM and SOS reached D across a partition"
