# The bot image: chat_responder, bmnode and libbmbot, with no Qt, in a slim Debian.
#
#   scripts/build-image.sh [ssh-host] [tag]     (from the root of the tree)
#
# Two stages. The build stage is the recipe of scripts/check-qt-free.sh (a plain Debian, SQLCipher 4
# built from a pinned commit because Debian ships 3.4.1) with -DYNOTBIT_QT=OFF; the runtime stage
# holds only what runs. The data (the vault, the mailbox, the node's objects, the log and the status
# file) is the volume /data; the phrase is a file, mounted read-only at /run/secrets/phrase.
FROM debian:bookworm-slim AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends build-essential cmake ninja-build pkg-config \
      ca-certificates curl git file tcl libssl-dev libsodium-dev python3 \
 && rm -rf /var/lib/apt/lists/*
# SQLCipher is a layer of its own: it changes seldom and takes minutes. A shared library here, so
# that linking it needs none of the private dependencies of SQLite.
RUN git clone --depth 1 --branch v4.6.1 https://github.com/sqlcipher/sqlcipher /sqlcipher \
 && test "$(git -C /sqlcipher rev-parse HEAD)" = c5bd336ece77922433aaf6d6fe8cf203b0c299d5 \
 && cd /sqlcipher \
 && ./configure --prefix=/usr/local --enable-shared --enable-tempstore=yes \
      CFLAGS="-DSQLITE_HAS_CODEC $(pkg-config --cflags libcrypto)" \
      LDFLAGS="$(pkg-config --libs libcrypto)" >/dev/null \
 && make -j"$(nproc)" >/dev/null \
 && make install >/dev/null \
 && ldconfig \
 && rm -rf /sqlcipher
COPY . /src
RUN cmake -S /src -B /build -G Ninja -DYNOTBIT_QT=OFF -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build --target chat_responder bmnode bmbot \
 && strip /build/chat_responder /build/bmnode

FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive
# tini is the entrypoint: the responder starts its node as a child, and a container's first
# process has to pass signals on and reap what exits.
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends libssl3 libsodium23 tini \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --shell /usr/sbin/nologin bot \
 && mkdir -p /data && chown bot:bot /data
COPY --from=build /usr/local/lib/libsqlcipher.so* /usr/local/lib/
COPY --from=build /build/bmbot/libbmbot.so* /usr/local/lib/
COPY --from=build /build/chat_responder /build/bmnode /usr/local/bin/
COPY bmbot/bmbot.h /usr/local/include/bmbot.h
RUN ldconfig
USER bot
WORKDIR /data
VOLUME /data
# The Bitmessage port: an incoming connection needs it reachable from outside.
EXPOSE 8444
STOPSIGNAL SIGTERM
# Healthy when its status file is fresh (made again every ten seconds) and it has a peer.
HEALTHCHECK --interval=30s --timeout=10s --start-period=180s --retries=3 \
  CMD ["chat_responder", "--check", "/data/status.json", "--max-age", "90", "--peers", "1"]
ENTRYPOINT ["tini", "--", "chat_responder"]
# No --peer: that default is ynotbit's node on the same machine, and a container has none. A bot
# keeps 512 MB of the network's objects for four days (the network drops one after about three).
CMD ["--public", "--peer", "", "--listen", "--port", "8444", "--retention-mb", "512", \
     "--retention-days", "4", "--data-dir", "/data/state", "--phrase-file", "/run/secrets/phrase", \
     "--log", "/data/responder.log", "--status-file", "/data/status.json"]
