# Qt-free core, slice 6: bot platform — Dockerfile
# Multi-stage build: build stage (host with full toolchain) -> runtime (slim Debian)
# Use: docker build -t bmbot . && docker run --rm bmbot

FROM ubuntu:22.04 AS builder

# Install build dependencies
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && \
    apt-get install -y -qq --no-install-recommends \
        build-essential cmake ninja-build pkg-config \
        ca-certificates curl git file tcl \
        libssl-dev libsodium-dev \
        sqlcipher libsqlite3-modern-cpp-dev \
        libsecp256k1-dev \
        > /dev/null

# Install secp256k1 from source (the project pins a specific version)
WORKDIR /tmp/secp256k1
RUN git -c --depth 1 fetch --tags https://github.com/bitcoin-core/secp256k1.git && \
    git checkout v0.6.0 && \
    ./autogen.sh && \
    ./configure --enable-ecdh --enable-modular-arch && \
    make -j$(nproc) && make install > /dev/null

WORKDIR /work
COPY . /work

# Configure and build the Qt-free targets
RUN cmake -S /work -B /build -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /build --parallel $(nproc)

# Runtime stage
FROM debian:bookworm-slim

# Create a non-root user
RUN useradd -m -u 1000 bot

# Copy built artefacts from builder
COPY --from=builder /build/bmnode /usr/local/bin/bmnode
COPY --from=builder /build/libbmbot.so /usr/local/lib/libbmbot.so
COPY --from=builder /build/chat_responder /usr/local/bin/chat_responder
COPY --from=builder /work /work

# Environment
ENV HOME=/home/bot \
    QT_QPA_PLATFORM=offscreen \
    PATH=/usr/local/bin:/usr/bin:/bin:/home/bot/.local/bin

# Work as the bot user
USER bot
WORKDIR /home/bot

# Node folder (data directory)
ENV NOTBIT_DATA=/home/bot/node-data

# Expose the chat port
EXPOSE 8444

# Health check: the responder must be able to report its status
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s \
  CMD chat_responder --check || exit 1

# Initialisation: start the node and responder as the container init
# (init: true in compose will run this on container start)
CMD ["/bin/bash", "-c", \
  "bmnode --data-dir $NOTBIT_DATA -m $NOTBIT_DATA/unused-maildir -i -R 64 -A 1 -P 127.0.0.1:8444 -L &\\n \
   sleep 3\\n \
   chat_responder --data-dir $NOTBIT_DATA --fresh --silent --phrase \"slice6 bot\" --peer 127.0.0.1:8444 --status-file /home/bot/status.json &\\n \
   wait"]