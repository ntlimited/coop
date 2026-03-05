#!/usr/bin/env bash
set -euo pipefail

IMAGE=coop-dev
docker build -t "$IMAGE" -q .

# Create the inner script to avoid shell escaping issues with $!
cat > /tmp/coop-profile-inner.sh << 'INNER'
#!/bin/bash
set -euo pipefail

PERF=/usr/lib/linux-tools/6.8.0-101-generic/perf

# Build RelWithDebInfo for symbols + optimization
cmake -B build/profile -DCMAKE_BUILD_TYPE=RelWithDebInfo 2>&1 | tail -3
cmake --build build/profile -j$(nproc) --target bench_server 2>&1 | tail -3

# Start server
build/profile/bin/bench_server 8080 2>/dev/null &
SERVER_PID=$!
sleep 2

# Attach perf to server process
$PERF record -g -o /tmp/perf.data -p $SERVER_PID &
PERF_PID=$!
sleep 0.2

# Drive load
wrk -t4 -c100 -d10s http://127.0.0.1:8080/plaintext

# Stop perf cleanly
kill -INT $PERF_PID
wait $PERF_PID 2>/dev/null || true

# Stop server
kill $SERVER_PID
wait $SERVER_PID 2>/dev/null || true

echo ""
echo "=== Top functions (self) ==="
$PERF report -i /tmp/perf.data --stdio --no-children -g none 2>/dev/null | head -50

echo ""
echo "=== Call graph (caller) ==="
$PERF report -i /tmp/perf.data --stdio --no-children -g caller 2>/dev/null | head -150
INNER

chmod +x /tmp/coop-profile-inner.sh

docker run --rm \
    -v "$PWD:/src" \
    -v /tmp/coop-profile-inner.sh:/tmp/profile.sh:ro \
    -w /src \
    --privileged \
    "$IMAGE" \
    bash /tmp/profile.sh
