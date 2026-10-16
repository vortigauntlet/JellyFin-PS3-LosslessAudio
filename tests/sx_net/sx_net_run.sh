#!/bin/bash
# Starts sx_net_server.py, runs the given test binary against it, stops the server.
# usage: sx_net_run.sh <test-binary> <certdir>
BIN=$1
CERTS=$2
PORTS=$(mktemp)
rm -f "$PORTS"
python3 "$(dirname "$0")/sx_net_server.py" "$CERTS" "$PORTS" &
SRV=$!
for i in $(seq 1 100); do [ -s "$PORTS" ] && break; sleep 0.1; done
if [ ! -s "$PORTS" ]; then echo "server did not start"; kill $SRV 2>/dev/null; exit 2; fi
"$BIN" "$PORTS" "$CERTS/ca.pem"
RC=$?
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
rm -f "$PORTS"
exit $RC
