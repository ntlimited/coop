#!/usr/bin/env bash
set -euo pipefail

IMAGE=coop-dev
BUILD_DIR=build/${1:-debug}

docker run --rm \
    -v "$PWD:/src" \
    -w /src \
    "$IMAGE" \
    "$BUILD_DIR/bin/coop_tests" "${@:2}"
