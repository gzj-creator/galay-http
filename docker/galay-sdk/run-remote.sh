#!/usr/bin/env bash
set -euo pipefail

readonly ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly VERIFY_DIR="${ROOT_DIR}/verify"
readonly OUT_DIR="${ROOT_DIR}/out"

BUILD_IMAGE="${BUILD_IMAGE:-galay-build:latest}"
BUILD_IMAGE_ALIAS="${BUILD_IMAGE_ALIAS:-galay-build:latest}"
RUNTIME_IMAGE="${RUNTIME_IMAGE:-galay-runtime:latest}"
RUNTIME_IMAGE_ALIAS="${RUNTIME_IMAGE_ALIAS:-galay-runtime:latest}"

mkdir -p "${OUT_DIR}"

export DOCKER_BUILDKIT=1

docker build --progress=plain \
    --target galay-build \
    -t "${BUILD_IMAGE}" \
    -t "${BUILD_IMAGE_ALIAS}" \
    "${ROOT_DIR}"

docker build --progress=plain \
    --target galay-runtime \
    -t "${RUNTIME_IMAGE}" \
    -t "${RUNTIME_IMAGE_ALIAS}" \
    "${ROOT_DIR}"

echo
echo "========== image summary =========="
for image in "${BUILD_IMAGE}" "${BUILD_IMAGE_ALIAS}" "${RUNTIME_IMAGE}" "${RUNTIME_IMAGE_ALIAS}"; do
    docker image inspect "${image}" --format '{{index .RepoTags 0}} {{.Size}}'
done

echo
echo "========== build image toolchain =========="
docker run --rm "${BUILD_IMAGE}" bash -lc 'g++ --version | head -n1; cmake --version | head -n1'

echo
echo "========== compile verify =========="
docker run --rm \
    -v "${VERIFY_DIR}:/work/verify" \
    -v "${OUT_DIR}:/work/out" \
    "${BUILD_IMAGE}" \
    bash -lc 'set -euo pipefail; cmake -S /work/verify -B /tmp/verify-build -G Ninja -DCMAKE_BUILD_TYPE=Release; cmake --build /tmp/verify-build -j"$(nproc)"; cp /tmp/verify-build/galay-sdk-verify /work/out/galay-sdk-verify'

echo
echo "========== runtime verify =========="
docker run --rm \
    -v "${OUT_DIR}:/work/out" \
    "${RUNTIME_IMAGE}" \
    bash -lc 'set -euo pipefail; ldd /work/out/galay-sdk-verify; LD_LIBRARY_PATH=/usr/local/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}} /work/out/galay-sdk-verify'
