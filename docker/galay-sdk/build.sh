#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly REPOS=(
    concurrentqueue
    galay-utils
    galay-kernel
    galay-ssl
    galay-http
    galay-rpc
    galay-mcp
    galay-redis
    galay-mysql
    galay-mongo
    galay-etcd
)

REMOTE_HOST="${REMOTE_HOST:-140.143.142.251}"
REMOTE_USER="${REMOTE_USER:-ubuntu}"
REMOTE_PORT="${REMOTE_PORT:-22}"
REMOTE_PASSWORD="${REMOTE_PASSWORD:-}"
REMOTE_DIR="${REMOTE_DIR:-/home/${REMOTE_USER}/galay-sdk}"
IMAGE_DATE="${IMAGE_DATE:-$(date +%Y%m%d)}"
UBUNTU_TAG="${UBUNTU_TAG:-ubuntu24.04}"
BUILD_IMAGE="${BUILD_IMAGE:-galay-build:${UBUNTU_TAG}-${IMAGE_DATE}}"
BUILD_IMAGE_ALIAS="${BUILD_IMAGE_ALIAS:-galay-build:latest}"
RUNTIME_IMAGE="${RUNTIME_IMAGE:-galay-runtime:${UBUNTU_TAG}-${IMAGE_DATE}}"
RUNTIME_IMAGE_ALIAS="${RUNTIME_IMAGE_ALIAS:-galay-runtime:latest}"

if [[ -z "${REMOTE_PASSWORD}" ]]; then
    echo "REMOTE_PASSWORD is required" >&2
    exit 1
fi

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing command: $1" >&2
        exit 1
    }
}

detect_repos_root() {
    local repo_root
    repo_root="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"

    local candidates=(
        "$(cd "${repo_root}/.." && pwd)"
        "$(cd "${repo_root}/../../.." && pwd)"
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        local ok=1
        local repo
        for repo in "${REPOS[@]}"; do
            if [[ ! -d "${candidate}/${repo}" ]]; then
                ok=0
                break
            fi
        done
        if [[ "${ok}" -eq 1 ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done

    return 1
}

run_expect_ssh() {
    local remote_cmd="$1"
    REMOTE_CMD="${remote_cmd}" \
    REMOTE_HOST="${REMOTE_HOST}" \
    REMOTE_USER="${REMOTE_USER}" \
    REMOTE_PORT="${REMOTE_PORT}" \
    REMOTE_PASSWORD="${REMOTE_PASSWORD}" \
    expect <<'EOF'
set timeout -1
set cmd $env(REMOTE_CMD)
spawn ssh -p $env(REMOTE_PORT) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $env(REMOTE_USER)@$env(REMOTE_HOST) $cmd
expect {
  -re ".*yes/no.*" { send "yes\r"; exp_continue }
  -re ".*assword:.*" { send "$env(REMOTE_PASSWORD)\r"; exp_continue }
  eof
}
catch wait result
exit [lindex $result 3]
EOF
}

run_expect_scp() {
    local src="$1"
    local dst="$2"
    SCP_SRC="${src}" \
    SCP_DST="${dst}" \
    REMOTE_PORT="${REMOTE_PORT}" \
    REMOTE_PASSWORD="${REMOTE_PASSWORD}" \
    expect <<'EOF'
set timeout -1
spawn scp -P $env(REMOTE_PORT) -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null $env(SCP_SRC) $env(SCP_DST)
expect {
  -re ".*yes/no.*" { send "yes\r"; exp_continue }
  -re ".*assword:.*" { send "$env(REMOTE_PASSWORD)\r"; exp_continue }
  eof
}
catch wait result
exit [lindex $result 3]
EOF
}

require_cmd expect
require_cmd rsync
require_cmd tar
require_cmd git

REPOS_ROOT="${GALAY_REPOS_ROOT:-$(detect_repos_root)}"
STAGE_DIR="$(mktemp -d /tmp/galay-sdk-context.XXXXXX)"
TARBALL="/tmp/galay-sdk-context-${IMAGE_DATE}.tar.gz"

cleanup() {
    rm -rf "${STAGE_DIR}"
    rm -f "${TARBALL}"
}
trap cleanup EXIT

mkdir -p "${STAGE_DIR}/src" "${STAGE_DIR}/verify" "${STAGE_DIR}/out"

cp "${SCRIPT_DIR}/Dockerfile" "${STAGE_DIR}/Dockerfile"
cp "${SCRIPT_DIR}/build-all.sh" "${STAGE_DIR}/build-all.sh"
cp "${SCRIPT_DIR}/run-remote.sh" "${STAGE_DIR}/run-remote.sh"
cp "${SCRIPT_DIR}/context.dockerignore" "${STAGE_DIR}/.dockerignore"
rsync -a --delete "${SCRIPT_DIR}/verify/" "${STAGE_DIR}/verify/"

for repo in "${REPOS[@]}"; do
    rsync -a --delete --exclude-from="${SCRIPT_DIR}/excludes.txt" \
        "${REPOS_ROOT}/${repo}/" "${STAGE_DIR}/src/${repo}/"
done

chmod +x "${STAGE_DIR}/build-all.sh" "${STAGE_DIR}/run-remote.sh"

find "${STAGE_DIR}" -name '._*' -delete
find "${STAGE_DIR}" -name '.DS_Store' -delete

COPYFILE_DISABLE=1 COPY_EXTENDED_ATTRIBUTES_DISABLE=1 tar -C "${STAGE_DIR}" -czf "${TARBALL}" .

run_expect_ssh "bash -lc 'mkdir -p ${REMOTE_DIR}'"
run_expect_scp "${TARBALL}" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_DIR}/$(basename "${TARBALL}")"

run_expect_ssh "bash -lc 'set -e; rm -rf ${REMOTE_DIR}/context; mkdir -p ${REMOTE_DIR}/context; tar -xzf ${REMOTE_DIR}/$(basename "${TARBALL}") -C ${REMOTE_DIR}/context; rm -f ${REMOTE_DIR}/$(basename "${TARBALL}")'"

run_expect_ssh "bash -lc 'cd ${REMOTE_DIR}/context && BUILD_IMAGE=${BUILD_IMAGE} BUILD_IMAGE_ALIAS=${BUILD_IMAGE_ALIAS} RUNTIME_IMAGE=${RUNTIME_IMAGE} RUNTIME_IMAGE_ALIAS=${RUNTIME_IMAGE_ALIAS} ./run-remote.sh'"
