#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PROTO_FILTER="${PROTO_FILTER:-wss h2}"

echo "[focus] running protocols: $PROTO_FILTER"
PROTO_FILTER="$PROTO_FILTER" "$ROOT/benchmark/compare/protocols/run_compare.sh"
