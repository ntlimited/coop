#!/usr/bin/env bash
set -euo pipefail

IMAGE=coop-dev
BUILD_TYPE=${1:-debug}
BUILD_DIR=build/$BUILD_TYPE
CMAKE_TYPE=$([ "$BUILD_TYPE" = "release" ] && echo Release || echo Debug)

docker build -t "$IMAGE" -q .
docker run --rm \
    -v "$PWD:/src" \
    -w /src \
    "$IMAGE" \
    bash -c "mkdir -p $BUILD_DIR && cd $BUILD_DIR && cmake -DCMAKE_BUILD_TYPE=$CMAKE_TYPE ../.. && cmake --build . -j\$(nproc)"
