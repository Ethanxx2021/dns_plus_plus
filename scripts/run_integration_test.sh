#!/bin/bash
# ============================================================
# DNS++ Phase 1 Integration Test
# Tests Algorithm 1 (Proximity Routing) on a single broker
#
# Scenario:
#   Subscriber A: London (51.5, -0.1)
#   Subscriber B: Berlin (52.5, 13.4)
#   Publisher 1:  Paris  (48.8, 2.3)  → closer to London
#   Publisher 2:  Warsaw (52.2, 21.0) → closer to Berlin
#
# Expected:
#   Paris  → Both receive (first pub, closest=∞)
#   Warsaw → Only Berlin receives (closer to Berlin than Paris)
# ============================================================

set -e

BROKER_PORT=8080
BROKER_BIN="./build/dns_broker"
CLIENT_BIN="./build/test_client"
SERVICE="test.integration"
LOG_DIR="/tmp/dnspp_test"
mkdir -p "$LOG_DIR"

echo "=== DNS++ Phase 1 Integration Test ==="
echo ""

# Start broker
$BROKER_BIN $BROKER_PORT 2 10 > "$LOG_DIR/broker.log" 2>&1 &
BROKER_PID=$!
sleep 0.5

# Verify broker is running
if ! kill -0 $BROKER_PID 2>/dev/null; then
    echo "FAIL: Broker failed to start"
    cat "$LOG_DIR/broker.log"
    exit 1
fi
echo "[OK] Broker started on port $BROKER_PORT (PID $BROKER_PID)"

# Start subscribers (5s timeout each)
timeout 5 $CLIENT_BIN sub $SERVICE 51.5 -0.1 > "$LOG_DIR/sub_london.log" 2>&1 &
SUB_LON_PID=$!
timeout 5 $CLIENT_BIN sub $SERVICE 52.5 13.4 > "$LOG_DIR/sub_berlin.log" 2>&1 &
SUB_BER_PID=$!
sleep 0.5
echo "[OK] Subscribers started (London + Berlin)"

# Publish from Paris
$CLIENT_BIN pub $SERVICE 48.8 2.3 Paris-edge-1
sleep 0.5
echo "[OK] Published from Paris"

# Publish from Warsaw
$CLIENT_BIN pub $SERVICE 52.2 21.0 Warsaw-edge-1
sleep 0.5
echo "[OK] Published from Warsaw"

# Wait for subscribers to finish (timeout)
wait $SUB_LON_PID 2>/dev/null || true
wait $SUB_BER_PID 2>/dev/null || true

# Kill broker
kill $BROKER_PID 2>/dev/null || true
wait $BROKER_PID 2>/dev/null || true

# --- Verify results ---
# --- Verify results ---
echo ""
echo "--- Results ---"

LONDON_GOT_PARIS=$(grep -c "Paris" "$LOG_DIR/sub_london.log" 2>/dev/null || true)
LONDON_GOT_PARIS=${LONDON_GOT_PARIS:-0}
LONDON_GOT_WARSAW=$(grep -c "Warsaw" "$LOG_DIR/sub_london.log" 2>/dev/null || true)
LONDON_GOT_WARSAW=${LONDON_GOT_WARSAW:-0}
BERLIN_GOT_PARIS=$(grep -c "Paris" "$LOG_DIR/sub_berlin.log" 2>/dev/null || true)
BERLIN_GOT_PARIS=${BERLIN_GOT_PARIS:-0}
BERLIN_GOT_WARSAW=$(grep -c "Warsaw" "$LOG_DIR/sub_berlin.log" 2>/dev/null || true)
BERLIN_GOT_WARSAW=${BERLIN_GOT_WARSAW:-0}

echo "London subscriber: Paris=$LONDON_GOT_PARIS, Warsaw=$LONDON_GOT_WARSAW"
echo "Berlin subscriber: Paris=$BERLIN_GOT_PARIS, Warsaw=$BERLIN_GOT_WARSAW"
echo ""

PASS=true

if [ "$LONDON_GOT_PARIS" -lt 1 ]; then
    echo "FAIL: London should have received Paris"
    PASS=false
fi
if [ "$LONDON_GOT_WARSAW" -ge 1 ]; then
    echo "FAIL: London should NOT have received Warsaw (farther than Paris)"
    PASS=false
fi
if [ "$BERLIN_GOT_PARIS" -lt 1 ]; then
    echo "FAIL: Berlin should have received Paris (first pub, closest=∞)"
    PASS=false
fi
if [ "$BERLIN_GOT_WARSAW" -lt 1 ]; then
    echo "FAIL: Berlin should have received Warsaw (closer than Paris)"
    PASS=false
fi

if [ "$PASS" = true ]; then
    echo "=== ALL TESTS PASSED ==="
    exit 0
else
    echo "=== TESTS FAILED ==="
    echo ""
    echo "--- Broker log ---"
    cat "$LOG_DIR/broker.log"
    echo ""
    echo "--- London subscriber log ---"
    cat "$LOG_DIR/sub_london.log"
    echo ""
    echo "--- Berlin subscriber log ---"
    cat "$LOG_DIR/sub_berlin.log"
    exit 1
fi