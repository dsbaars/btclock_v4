# Native multi-arch build of 3proxy for the local proxy test rig.
#
# The upstream Dockerfile.full
# (https://github.com/3proxy/3proxy/blob/master/Dockerfile.full) is
# distroless — it copies just the binary plus the handful of shared
# libraries it needs into a `scratch` image. That works great inside a
# checked-out 3proxy tree, but it's cumbersome to build standalone
# because every arch needs the corresponding `gcc -dumpmachine` library
# paths spelled out.
#
# We instead use a two-stage debian-slim build that downloads a pinned
# 3proxy release tarball and produces a small image that runs natively
# on whatever architecture the Docker daemon is on (arm64 on Apple
# Silicon, amd64 on Linux servers). ~30 MB final image vs the ~10 MB
# distroless one — a fair trade for the simplicity.

FROM debian:bookworm-slim AS build
ARG VERSION=0.9.4
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget build-essential libssl-dev libpcre2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN wget -O 3proxy.tar.gz \
        "https://github.com/3proxy/3proxy/archive/refs/tags/${VERSION}.tar.gz" \
    && tar xf 3proxy.tar.gz \
    && cd "3proxy-${VERSION}" \
    && make -f Makefile.Linux \
    && strip bin/3proxy \
    && install -D -m0755 bin/3proxy /out/bin/3proxy

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/3proxy /usr/local/bin/3proxy
EXPOSE 1080 1081 3128 3129
ENTRYPOINT ["/usr/local/bin/3proxy"]
CMD ["/etc/3proxy/3proxy.cfg"]
