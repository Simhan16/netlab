#!/usr/bin/env bash
# Break the lab in a known way, capture the failure, label it, and fix it.
#
#   sudo ./run_case.sh list
#   sudo ./run_case.sh 03-mtu-blackhole [runs]
#   sudo ./run_case.sh all [runs]
#
# Each case file in cases/ defines CASE_LABEL, CASE_DESCRIPTION, CAPTURE_NS,
# CAPTURE_IF, optional CAPTURE_FILTER/CASE_SETTLE, and case_break /
# case_trigger / case_fix functions.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

run_one() {
  local case_file=$1 run=$2
  local name
  name="$(basename "$case_file" .sh)"
  local rel="pcaps/$name/run$run.pcap"
  # Subshell: each case's variables and functions stay isolated, and the
  # EXIT trap guarantees case_fix runs even if the trigger blows up.
  (
    CAPTURE_FILTER=""
    CASE_SETTLE=2
    case_break() { :; }
    case_fix() { :; }
    source "$case_file"
    trap 'stop_capture; case_fix' EXIT

    log "[$name run $run] break"
    case_break
    start_capture "$CAPTURE_NS" "$CAPTURE_IF" "$CASEBOOK_DIR/$rel" $CAPTURE_FILTER
    log "[$name run $run] trigger"
    case_trigger || true  # the trigger is *supposed* to fail in most cases
    sleep "$CASE_SETTLE"
    stop_capture
    log "[$name run $run] fix"
    trap - EXIT
    case_fix
    add_label "$name-run$run" "$rel" "$CASE_LABEL" "$name" "$CAPTURE_NS:$CAPTURE_IF" "$CASE_DESCRIPTION"
    log "[$name run $run] saved $rel ($CASE_LABEL)"
  )
}

main() {
  require_root
  [[ $# -ge 1 ]] || die "usage: $0 <case|all|list> [runs]"
  local which=$1 runs=${2:-1}
  if [[ $which == list ]]; then
    for f in "$CASEBOOK_DIR"/cases/*.sh; do
      (source "$f"; printf '%-24s %-22s %s\n' "$(basename "$f" .sh)" "$CASE_LABEL" "$CASE_DESCRIPTION")
    done
    return
  fi
  require_lab
  mkdir -p "$WORK_DIR" "$PCAP_DIR"

  local files=()
  if [[ $which == all ]]; then
    files=("$CASEBOOK_DIR"/cases/*.sh)
  else
    files=("$CASEBOOK_DIR/cases/$which.sh")
    [[ -f ${files[0]} ]] || die "no such case: $which (try: $0 list)"
  fi
  for f in "${files[@]}"; do
    for ((run = 1; run <= runs; run++)); do run_one "$f" "$run"; done
  done
  # pcaps are written as root; hand them back to whoever ran sudo.
  [[ -n ${SUDO_USER:-} ]] && chown -R "$SUDO_USER" "$PCAP_DIR" "$LABELS_FILE"
  log "labels: $LABELS_FILE"
}

main "$@"
