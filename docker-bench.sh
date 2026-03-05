#!/usr/bin/env bash
set -euo pipefail

IMAGE=coop-dev
docker build -t "$IMAGE" -q .

docker run --rm \
    -v "$PWD:/src" \
    -w /src \
    --privileged \
    "$IMAGE" \
    bash -c '
        cmake -B build/release -DCMAKE_BUILD_TYPE=Release && \
        cmake --build build/release -j$(nproc) --target bench_server && \
        build/release/bin/bench_server 8080 &
        SERVER_PID=$!
        sleep 2

        echo "=== wrk -t1 -c10 -d10s ==="
        wrk -t1 -c10 -d10s http://127.0.0.1:8080/plaintext
        echo ""

        echo "=== wrk -t1 -c50 -d10s ==="
        wrk -t1 -c50 -d10s http://127.0.0.1:8080/plaintext
        echo ""

        echo "=== wrk -t1 -c100 -d10s ==="
        wrk -t1 -c100 -d10s http://127.0.0.1:8080/plaintext
        echo ""

        echo "=== wrk -t4 -c100 -d10s ==="
        wrk -t4 -c100 -d10s http://127.0.0.1:8080/plaintext

        kill $SERVER_PID
        wait $SERVER_PID 2>/dev/null
    '
