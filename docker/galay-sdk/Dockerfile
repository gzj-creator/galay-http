FROM ubuntu:24.04 AS build-deps

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libaio-dev \
        libsimdjson-dev \
        libspdlog-dev \
        libssl-dev \
        liburing-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV CMAKE_PREFIX_PATH=/usr/local \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig \
    LD_LIBRARY_PATH=/usr/local/lib

WORKDIR /workspace

COPY build-all.sh /usr/local/bin/build-all.sh
COPY src /src

RUN chmod +x /usr/local/bin/build-all.sh \
    && /usr/local/bin/build-all.sh \
    && printf '/usr/local/lib\n' >/etc/ld.so.conf.d/galay-sdk.conf \
    && ldconfig

FROM ubuntu:24.04 AS galay-build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        git \
        libaio-dev \
        libfmt-dev \
        libsimdjson-dev \
        libspdlog-dev \
        libssl-dev \
        liburing-dev \
        ninja-build \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build-deps /usr/local /usr/local

RUN printf '/usr/local/lib\n' >/etc/ld.so.conf.d/galay-sdk.conf \
    && ldconfig

ENV CMAKE_PREFIX_PATH=/usr/local \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig \
    LD_LIBRARY_PATH=/usr/local/lib

WORKDIR /workspace

FROM ubuntu:24.04 AS galay-runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        libaio1t64 \
        libfmt9 \
        libsimdjson19 \
        libspdlog1.12 \
        libssl3t64 \
        liburing2 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build-deps /usr/local /usr/local

RUN find /usr/local/lib -type f \( -name '*.so' -o -name '*.so.*' \) -exec strip --strip-unneeded {} + 2>/dev/null || true \
    && printf '/usr/local/lib\n' >/etc/ld.so.conf.d/galay-sdk.conf \
    && ldconfig

ENV LD_LIBRARY_PATH=/usr/local/lib

WORKDIR /workspace
