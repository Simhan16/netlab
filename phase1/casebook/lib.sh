# Helpers shared by run_case.sh and the case files. Sourced, not executed.

CASEBOOK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE1_DIR="$(dirname "$CASEBOOK_DIR")"
source "$PHASE1_DIR/netns-router/config.sh"

MINI_TCPDUMP="${MINI_TCPDUMP:-$PHASE1_DIR/build/mini-tcpdump/mini-tcpdump}"
LABELS_FILE="$CASEBOOK_DIR/labels.json"
PCAP_DIR="$CASEBOOK_DIR/pcaps"
WORK_DIR="${WORK_DIR:-/tmp/netlab-casebook}"

HOST_HTTP_PORT=8000
HOST_HTTP_URL="http://$UPLINK_HOST_IP:$HOST_HTTP_PORT"

in_ns() { ip netns exec "$@"; }

require_lab() {
  ip netns list | grep -qw "$ROUTER_NS" || die "netns lab not running: sudo $PHASE1_DIR/netns-router/setup.sh"
  [[ -x $MINI_TCPDUMP ]] || die "mini-tcpdump not built at $MINI_TCPDUMP (cd phase1 && cmake -B build && cmake --build build)"
  command -v python3 >/dev/null || die "python3 is required"
  command -v curl >/dev/null || die "curl is required"
}

# start_capture <netns> <iface> <pcap> [bpf filter...]
start_capture() {
  local ns=$1 iface=$2 pcap=$3
  shift 3
  mkdir -p "$(dirname "$pcap")"
  # `ip netns exec` execs the command, so $! is mini-tcpdump itself.
  in_ns "$ns" "$MINI_TCPDUMP" -i "$iface" -q -w "$pcap" "$@" >"$WORK_DIR/capture.log" 2>&1 &
  CAPTURE_PID=$!
  sleep 1  # let libpcap attach before generating traffic
  kill -0 "$CAPTURE_PID" 2>/dev/null || { cat "$WORK_DIR/capture.log" >&2; die "capture failed to start"; }
}

stop_capture() {
  [[ -n ${CAPTURE_PID:-} ]] || return 0
  kill -INT "$CAPTURE_PID" 2>/dev/null || true
  wait "$CAPTURE_PID" 2>/dev/null || true
  CAPTURE_PID=
}

# A plain HTTP server on the host side of the uplink, playing "the internet".
# Local, so the cases don't depend on how a real CDN handles loss or MTU.
start_host_http() {
  mkdir -p "$WORK_DIR/www"
  [[ -f $WORK_DIR/www/big.bin ]] || head -c 2000000 /dev/urandom >"$WORK_DIR/www/big.bin"
  echo "hello from the casebook" >"$WORK_DIR/www/index.html"
  python3 -m http.server "$HOST_HTTP_PORT" --bind "$UPLINK_HOST_IP" --directory "$WORK_DIR/www" \
    >"$WORK_DIR/http.log" 2>&1 &
  HOST_HTTP_PID=$!
  sleep 1
}

stop_host_http() {
  [[ -n ${HOST_HTTP_PID:-} ]] && kill "$HOST_HTTP_PID" 2>/dev/null || true
  HOST_HTTP_PID=
}

# start_ns_http <netns> <ip> <port>: an HTTP server inside a LAN namespace.
start_ns_http() {
  mkdir -p "$WORK_DIR/www"
  echo "hello from $1" >"$WORK_DIR/www/index.html"
  in_ns "$1" python3 -m http.server "$3" --bind "$2" --directory "$WORK_DIR/www" >"$WORK_DIR/ns-http.log" 2>&1 &
  NS_HTTP_PID=$!
  sleep 1
}

stop_ns_http() {
  [[ -n ${NS_HTTP_PID:-} ]] && kill "$NS_HTTP_PID" 2>/dev/null || true
  NS_HTTP_PID=
}

# add_label <id> <pcap relative path> <label> <case> <capture point> <description>
# Replaces any existing entry with the same id, so re-running a case is safe.
add_label() {
  python3 - "$LABELS_FILE" "$@" <<'PY'
import json, os, sys, datetime
path, cid, pcap, label, case, point, desc = sys.argv[1:]
data = {"version": 1, "captures": []}
if os.path.exists(path):
    with open(path) as f:
        data = json.load(f)
data["captures"] = [c for c in data["captures"] if c["id"] != cid]
data["captures"].append({
    "id": cid, "pcap": pcap, "label": label, "case": case,
    "capture_point": point, "description": desc, "synthetic": False,
    "captured_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
})
data["captures"].sort(key=lambda c: c["id"])
tmp = path + ".tmp"
with open(tmp, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
os.replace(tmp, path)
PY
}
