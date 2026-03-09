#!/usr/bin/env bash
set -euo pipefail

readonly REPO_ROOT=/src
readonly BUILD_ROOT=/tmp/galay-sdk-build

mkdir -p "${BUILD_ROOT}"

export CMAKE_PREFIX_PATH=/usr/local
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig
export LD_LIBRARY_PATH=/usr/local/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}

build_repo() {
    local repo="$1"
    shift

    echo
    echo "========== build ${repo} =========="

    cmake -S "${REPO_ROOT}/${repo}" \
        -B "${BUILD_ROOT}/${repo}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_PREFIX_PATH=/usr/local \
        -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_CXX_EXTENSIONS=OFF \
        "$@"

    cmake --build "${BUILD_ROOT}/${repo}" -j"$(nproc)"
    cmake --install "${BUILD_ROOT}/${repo}"
    rm -rf "${BUILD_ROOT:?}/${repo}"

    echo "========== done ${repo} =========="
}

build_repo concurrentqueue

build_repo galay-utils \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_MODULE_TESTS=OFF \
    -DDISABLE_IOURING=OFF

build_repo galay-kernel \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DENABLE_CPP23_MODULES=OFF \
    -DDISABLE_IOURING=OFF

build_repo galay-ssl \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_MODULE_EXAMPLES=OFF \
    -DDISABLE_IOURING=OFF

build_repo galay-http \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_MODULE_EXAMPLES=OFF \
    -DGALAY_HTTP_ENABLE_SSL=ON

build_repo galay-rpc \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_MODULE_EXAMPLES=OFF \
    -DGALAY_RPC_INSTALL_MODULE_INTERFACE=OFF \
    -DDISABLE_IOURING=OFF

build_repo galay-mcp \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_MODULE_EXAMPLES=OFF

build_repo galay-redis \
    -DBUILD_TESTS=OFF \
    -DBUILD_BENCHMARKS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DGALAY_REDIS_ENABLE_IMPORT_COMPILATION=OFF \
    -DGALAY_REDIS_INSTALL_MODULE_INTERFACE=OFF \
    -DDISABLE_IOURING=OFF

build_repo galay-mysql \
    -DGALAY_MYSQL_BUILD_TESTS=OFF \
    -DGALAY_MYSQL_BUILD_EXAMPLES=OFF \
    -DGALAY_MYSQL_BUILD_BENCHMARKS=OFF \
    -DGALAY_MYSQL_BUILD_SHARED_LIBS=ON \
    -DGALAY_MYSQL_ENABLE_IMPORT_COMPILATION=OFF \
    -DGALAY_MYSQL_INSTALL_MODULE_INTERFACE=OFF \
    -DGALAY_MYSQL_BUILD_MODULE_EXAMPLES=OFF

build_repo galay-mongo \
    -DGALAY_MONGO_BUILD_TESTS=OFF \
    -DGALAY_MONGO_BUILD_EXAMPLES=OFF \
    -DGALAY_MONGO_BUILD_BENCHMARKS=OFF \
    -DGALAY_MONGO_BUILD_SHARED_LIBS=ON \
    -DGALAY_MONGO_ENABLE_IMPORT_COMPILATION=OFF \
    -DGALAY_MONGO_INSTALL_MODULE_INTERFACE=OFF \
    -DGALAY_MONGO_BUILD_MODULE_EXAMPLES=OFF

build_repo galay-etcd \
    -DGALAY_ETCD_BUILD_TESTS=OFF \
    -DGALAY_ETCD_BUILD_EXAMPLES=OFF \
    -DGALAY_ETCD_BUILD_BENCHMARKS=OFF \
    -DGALAY_ETCD_BUILD_SHARED_LIBS=ON \
    -DGALAY_ETCD_ENABLE_IMPORT_COMPILATION=OFF \
    -DGALAY_ETCD_INSTALL_MODULE_INTERFACE=OFF

rm -rf "${BUILD_ROOT}"
