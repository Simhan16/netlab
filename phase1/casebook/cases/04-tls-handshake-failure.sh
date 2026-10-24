# TLS name mismatch: the client connects to app.lab, but the server presents a
# certificate for wrong.lab. TCP is fine; the TLS handshake aborts with an alert.
# TLS 1.2 is forced so the alert is visible in plaintext (TLS 1.3 encrypts it).
CASE_LABEL=tls_handshake_failure
CASE_DESCRIPTION="server presents a certificate for the wrong hostname; client aborts the TLS handshake with a fatal alert"
CAPTURE_NS=$LAN_A_NS
CAPTURE_IF=a-eth0

case_break() {
  local d="$WORK_DIR/tls"
  mkdir -p "$d"
  openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=netlab-ca" \
    -keyout "$d/ca.key" -out "$d/ca.pem" 2>/dev/null
  openssl req -newkey rsa:2048 -nodes -subj "/CN=wrong.lab" -keyout "$d/server.key" -out "$d/server.csr" 2>/dev/null
  printf 'subjectAltName=DNS:wrong.lab\n' >"$d/san.ext"
  openssl x509 -req -in "$d/server.csr" -CA "$d/ca.pem" -CAkey "$d/ca.key" -CAcreateserial -days 1 \
    -extfile "$d/san.ext" -out "$d/server.pem" 2>/dev/null
  in_ns "$LAN_B_NS" openssl s_server -quiet -tls1_2 -accept 8443 -cert "$d/server.pem" -key "$d/server.key" -www \
    >"$WORK_DIR/s_server.log" 2>&1 &
  TLS_SERVER_PID=$!
  sleep 1
}
case_trigger() {
  in_ns "$LAN_A_NS" curl -sS -o /dev/null --max-time 8 --tlsv1.2 --tls-max 1.2 --cacert "$WORK_DIR/tls/ca.pem" \
    --resolve "app.lab:8443:$LAN_B_HOST" https://app.lab:8443/
}
case_fix() { [[ -n ${TLS_SERVER_PID:-} ]] && kill "$TLS_SERVER_PID" 2>/dev/null || true; }
