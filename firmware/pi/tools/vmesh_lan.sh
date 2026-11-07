#!/usr/bin/env bash
# vmesh_lan.sh: a 2-6 node virtual mesh that stays up, each node with its
# client link open, for driving nodes from an app (or gwclient.py).
#
#   N1 --1.5 km-- N2 --1.5 km-- N3 ...      UDP virtual radio, fixed positions
#
# Usage: firmware/pi/tools/vmesh_lan.sh [-n NODES] [--bind ADDR | --lan]
#                                      [--base-port PORT] [--spacing KM] [path/to/ghostwaved]
#   -n NODES        2..6, default 4
#   --bind ADDR     address the client ports listen on, default 127.0.0.1
#   --lan           bind to this machine's LAN address so a phone on the same
#                   Wi-Fi can connect (see the warning this prints)
#   --base-port P   node i listens on P+i-1, default 47800
#   --spacing KM    distance between neighbors, default 1.5
# Env: VMESH_PORT (multicast port, default 47790), VMESH_IF.
# Ctrl-C stops every node. Logs, keys and a node table are in the work dir,
# which is kept so the node IDs stay the same if you pass it back as
# VMESH_LAN_DIR=<dir>.
#
# Security: the client link has no authentication yet. Anyone who can reach a
# bound port can read that node's messages, send as it, and raise or ack SOS.
# Use --lan only on a network you trust, and stop the mesh when done.
set -u

here=$(cd "$(dirname "$0")" && pwd)
fw=$(cd "$here/../.." && pwd)
n=4 bind=127.0.0.1 base=47800 spacing=1.5 bin=""
while [ $# -gt 0 ]; do
  case $1 in
    -n) n=$2; shift ;;
    --bind) bind=$2; shift ;;
    --lan) bind=lan ;;
    --base-port) base=$2; shift ;;
    --spacing) spacing=$2; shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) bin=$1 ;;
  esac
  shift
done
bin=${bin:-$fw/build/ghostwaved}
if ! [[ "$n" =~ ^[2-6]$ ]]; then echo "-n wants 2..6"; exit 2; fi
if [ "$bind" = lan ]; then
  # the source address the OS would use for the internet; nothing is sent
  bind=$(python3 -c 'import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.connect(("192.0.2.1", 9)); print(s.getsockname()[0])' 2>/dev/null)
  [ -n "$bind" ] || { echo "cannot find a LAN address; pass --bind ADDR"; exit 1; }
fi
if [ ! -x "$bin" ]; then
  echo "== building $bin (GHOSTWAVE_BUILD_VIRT=ON)"
  cmake -S "$fw" -B "$fw/build" -DGHOSTWAVE_BUILD_VIRT=ON >/dev/null &&
    cmake --build "$fw/build" --target ghostwaved --parallel >/dev/null || exit 1
fi

work=${VMESH_LAN_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/vmesh-lan.XXXXXX")}
mkdir -p "$work"
port=${VMESH_PORT:-47790}
pids=""
cleanup() {
  for p in $pids; do kill "$p" 2>/dev/null; done
  wait 2>/dev/null
  echo "stopped; logs and keys kept in $work"
}
trap cleanup EXIT
trap 'exit 0' INT TERM

for i in $(seq 1 "$n"); do
  name=N$i
  lat=$(awk -v k="$(awk -v i="$i" -v s="$spacing" 'BEGIN{print (i-1)*s}')" 'BEGIN { printf "%.7f", 46.87 + k * 0.0089932 }')
  args=(--radio udp --mcast "239.77.0.1:$port" --key "$work/$name.key" --name "$name"
        --lat "$lat" --lon -113.99 --client-tcp "$bind:$((base + i - 1))")
  [ -n "${VMESH_IF:-}" ] && args+=(--vif "$VMESH_IF")
  # stdin is a FIFO held open by this script: `echo stats > $work/N1.in` works
  rm -f "$work/$name.in"; mkfifo "$work/$name.in"
  "$bin" "${args[@]}" <"$work/$name.in" >"$work/$name.log" 2>&1 &
  pids="$pids $!"
  eval "exec $((i + 2))>\"\$work/\$name.in\""
  sleep 0.5
done

table="$work/nodes.txt"
: >"$table"
for i in $(seq 1 "$n"); do
  name=N$i
  for _ in $(seq 1 25); do grep -q "^client link on tcp" "$work/$name.log" && break; sleep 0.2; done
  if ! grep -q "^client link on tcp" "$work/$name.log"; then
    echo "$name failed to start:"; cat "$work/$name.log"; exit 1
  fi
  id=$(awk '/^ghostwaved node /{print $3; exit}' "$work/$name.log")
  pub=$(awk '/^key ed25519 /{print substr($3,1,64); exit}' "$work/$name.log")
  addr=$(awk '/^client link on tcp /{print $5; exit}' "$work/$name.log")
  printf "%-3s %s %-21s %s\n" "$name" "$id" "$addr" "$pub" >>"$table"
done

echo "virtual mesh: $n nodes, ${spacing} km apart, radio group 239.77.0.1:$port"
echo
echo "    node id       client link           ed25519 public key (contact / QR card)"
sed 's/^/    /' "$table"
echo
case $bind in
  127.*|::1|localhost) echo "client ports are on loopback: only this machine can connect." ;;
  *) echo "WARNING: client ports listen on $bind. Anyone on that network can drive these nodes"
     echo "         (read DMs, send, raise/ack SOS): there is no client authentication yet." ;;
esac
first=$(awk 'NR==1{print $3}' "$table")
echo
echo "try:  '$here/gwclient.py' $first info"
echo "      '$here/gwclient.py' $first listen"
echo "      echo stats > $work/N1.in   (stdin commands; output in $work/N1.log)"
echo "logs: $work/N*.log     Ctrl-C to stop"
wait
