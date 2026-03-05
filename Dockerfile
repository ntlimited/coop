FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake make g++ liburing-dev libssl-dev openssl ca-certificates git \
    linux-tools-generic linux-tools-common wrk \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
