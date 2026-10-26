# The bot images, with no Qt, in a slim Debian: the chat responder (the default target) and nickbot.
#
#   scripts/build-image.sh [ssh-host] [tag] [target]     (from the root of the tree)
#   target: responder (default) or nickbot
#
# The build stage is the recipe of scripts/check-qt-free.sh (a plain Debian, SQLCipher 4 built from a
# pinned commit because Debian ships 3.4.1) with -DYNOTBIT_QT=OFF. nickbot is Go on top of libbmbot,
# built in that same stage with the toolchain copied from the golang image. The runtime stages hold
# only what runs. The data (the vault, the mailbox, the node's objects, the log and the status file)
# is the volume /data; the phrase is a file, mounted read-only at /run/secrets/phrase.
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

# nickbot is Go with cgo on libbmbot. The toolchain comes from the golang image; the module is built
# here, in the stage that has the library, its header and everything it links.
FROM build AS gobuild
COPY --from=golang:1.24-bookworm /usr/local/go /usr/local/go
ENV PATH=/usr/local/go/bin:$PATH GOTOOLCHAIN=local CGO_ENABLED=1 CGO_LDFLAGS=-L/build/bmbot
RUN cd /src/nickbot \
 && go vet -tags libbmbot ./... \
 && go build -tags libbmbot -trimpath -ldflags "-s -w" -o /build/nickbot ./cmd/nickbot

FROM debian:bookworm-slim AS base
ENV DEBIAN_FRONTEND=noninteractive
# tini is the entrypoint: the bot starts its node as a child, and a container's first process has to
# pass signals on and reap what exits.
RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends libssl3 libsodium23 tini \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --user-group --home-dir /data --shell /usr/sbin/nologin bot \
 && mkdir -p /data && chown bot:bot /data
COPY --from=build /usr/local/lib/libsqlcipher.so* /usr/local/lib/
COPY --from=build /build/bmbot/libbmbot.so* /usr/local/lib/
COPY --from=build /build/bmnode /usr/local/bin/
COPY bmbot/bmbot.h /usr/local/include/bmbot.h
RUN ldconfig

# nickbot: nicknames for Bitmessage addresses (docs/nickbot.md). It keeps its registry, its backups and
# its status in /data. The shared lookup chan and the node's port are published as for the responder;
# a seed file, if there is one, is mounted and named with --seed.
FROM base AS nickbot
COPY --from=gobuild /build/nickbot /usr/local/bin/nickbot
USER bot
WORKDIR /data
VOLUME /data
EXPOSE 8444
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=10s --start-period=180s --retries=3 \
  CMD ["nickbot", "--check", "/data/status.json", "--max-age", "90", "--peers", "1"]
ENTRYPOINT ["tini", "--", "nickbot"]
CMD ["--data-dir", "/data", "--phrase-file", "/run/secrets/phrase", "--listen", "--port", "8444", \
     "--retention-mb", "512", "--retention-days", "4"]

# The chat responder: the default target, so that a build with no --target is what it was.
FROM base AS responder
COPY --from=build /build/chat_responder /usr/local/bin/
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
