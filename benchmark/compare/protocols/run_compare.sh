#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
COMPARE_ROOT="$ROOT/benchmark/compare/protocols"
GO_SERVER_DIR="$COMPARE_ROOT/go-server"
RUST_SERVER_DIR="$COMPARE_ROOT/rust-server"
GO_CLIENT_DIR="$COMPARE_ROOT/go-client"
GALAY_BUILD_DIR="${GALAY_BUILD_DIR:-$ROOT/build-ssl-nolog}"
BENCH_THREADS="${BENCH_THREADS:-4}"

OUT_DIR="$ROOT/benchmark/results/$(date +%Y%m%d-%H%M%S)-galay-go-rust-http-proto-compare"
mkdir -p "$OUT_DIR"

GO_SERVER_BIN="$OUT_DIR/go-proto-server"
RUST_SERVER_BIN="$RUST_SERVER_DIR/target/release/galay-http-rust-proto-server"
CLIENT_BIN="$OUT_DIR/go-proto-client"

GALAY_HTTP_BIN="$GALAY_BUILD_DIR/benchmark/B1-HttpServer"
GALAY_HTTPS_BIN="$GALAY_BUILD_DIR/benchmark/B14-HttpsServer"
GALAY_WS_BIN="$GALAY_BUILD_DIR/benchmark/B5-WebsocketServer"
GALAY_WSS_BIN="$GALAY_BUILD_DIR/benchmark/B7-WssServer"
GALAY_H2C_BIN="$GALAY_BUILD_DIR/benchmark/B3-H2cServer"
GALAY_H2_BIN="$GALAY_BUILD_DIR/benchmark/B12-H2Server"

CSV_FILE="$OUT_DIR/metrics.csv"
SUMMARY_FILE="$OUT_DIR/summary.md"

command -v sample >/dev/null
command -v inferno-collapse-sample >/dev/null
command -v inferno-flamegraph >/dev/null
command -v go >/dev/null
command -v cargo >/dev/null
command -v cmake >/dev/null

echo "[build] go server"
GOPROXY=https://goproxy.cn,direct go -C "$GO_SERVER_DIR" mod tidy >/dev/null
GOPROXY=https://goproxy.cn,direct go -C "$GO_SERVER_DIR" build -o "$GO_SERVER_BIN" .

echo "[build] rust server"
(cd "$RUST_SERVER_DIR" && cargo build --release >/dev/null)

echo "[build] go client"
GOPROXY=https://goproxy.cn,direct go -C "$GO_CLIENT_DIR" mod tidy >/dev/null
GOPROXY=https://goproxy.cn,direct go -C "$GO_CLIENT_DIR" build -o "$CLIENT_BIN" .

if [[ ! -f "$GALAY_BUILD_DIR/CMakeCache.txt" ]]; then
  echo "[build] configure galay build dir: $GALAY_BUILD_DIR"
  cmake -S "$ROOT" -B "$GALAY_BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGALAY_HTTP_ENABLE_SSL=ON \
    -DGALAY_HTTP_DISABLE_FRAMEWORK_LOG=ON >/dev/null
fi

echo "[build] galay servers"
cmake --build "$GALAY_BUILD_DIR" --parallel \
  --target B1-HttpServer B14-HttpsServer B5-WebsocketServer B7-WssServer B3-H2cServer B12-H2Server >/dev/null

echo "[config] unified worker threads: $BENCH_THREADS"

echo "language,proto,addr,conns,duration,success,fail,rps,avg_ms" > "$CSV_FILE"

proto_path() {
  case "$1" in
    http|https) echo "/" ;;
    ws|wss) echo "/ws" ;;
    h2c|h2) echo "/echo" ;;
    *) echo "/" ;;
  esac
}

proto_conns() {
  case "$1" in
    http) echo "200" ;;
    https) echo "160" ;;
    ws) echo "80" ;;
    wss) echo "60" ;;
    h2c|h2) echo "140" ;;
    *) echo "100" ;;
  esac
}

proto_duration() {
  case "$1" in
    http|https|ws|wss|h2c|h2) echo "8" ;;
    *) echo "8" ;;
  esac
}

proto_size() {
  case "$1" in
    ws|wss) echo "1024" ;;
    http|https|h2c|h2) echo "128" ;;
    *) echo "128" ;;
  esac
}

parse_metric() {
  local line="$1" key="$2"
  awk -v k="$key" '{for(i=1;i<=NF;i++){if(index($i,k"=")==1){split($i,a,"=");print a[2];exit}}}' <<<"$line"
}

wait_port_ready() {
  local addr="$1"
  local host="${addr%:*}"
  local port="${addr##*:}"
  local timeout_ms="${2:-8000}"
  local loops=$((timeout_ms / 100))
  local i=0
  while (( i < loops )); do
    if (echo >"/dev/tcp/$host/$port") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
    ((i+=1))
  done
  return 1
}

set_addr() {
  local language="$1" proto="$2"
  case "$language" in
    go)
      case "$proto" in
        http|ws) ADDR="127.0.0.1:18080" ;;
        h2c) ADDR="127.0.0.1:19080" ;;
        https|wss|h2) ADDR="127.0.0.1:19443" ;;
      esac
      ;;
    rust)
      case "$proto" in
        http|ws) ADDR="127.0.0.1:28080" ;;
        h2c) ADDR="127.0.0.1:29080" ;;
        https|wss|h2) ADDR="127.0.0.1:29443" ;;
      esac
      ;;
    galay)
      case "$proto" in
        http) ADDR="127.0.0.1:38080" ;;
        https) ADDR="127.0.0.1:39443" ;;
        ws) ADDR="127.0.0.1:38081" ;;
        wss) ADDR="127.0.0.1:39444" ;;
        h2c) ADDR="127.0.0.1:39080" ;;
        h2) ADDR="127.0.0.1:39445" ;;
      esac
      ;;
    *)
      echo "[fatal] unknown language: $language" >&2
      exit 1
      ;;
  esac
}

start_server() {
  local language="$1" proto="$2"
  local log_file="$OUT_DIR/${language}_${proto}_server.log"
  local -a cmd

  case "$language" in
    go)
      cmd=(
        env
        "GOMAXPROCS=$BENCH_THREADS"
        "$GO_SERVER_BIN"
        --http ":18080"
        --h2c ":19080"
        --https ":19443"
        --cert "$ROOT/cert/test.crt"
        --key "$ROOT/cert/test.key"
      )
      ;;
    rust)
      cmd=(
        env
        "TOKIO_WORKER_THREADS=$BENCH_THREADS"
        "$RUST_SERVER_BIN"
        "0.0.0.0:28080"
        "0.0.0.0:29080"
        "0.0.0.0:29443"
        "$ROOT/cert/test.crt"
        "$ROOT/cert/test.key"
      )
      ;;
    galay)
      case "$proto" in
        http)
          cmd=("$GALAY_HTTP_BIN" "38080" "$BENCH_THREADS")
          ;;
        https)
          cmd=("$GALAY_HTTPS_BIN" "39443" "$BENCH_THREADS" "$ROOT/cert/test.crt" "$ROOT/cert/test.key")
          ;;
        ws)
          cmd=("$GALAY_WS_BIN" "38081" "$BENCH_THREADS")
          ;;
        wss)
          cmd=("$GALAY_WSS_BIN" "39444" "$BENCH_THREADS" "$ROOT/cert/test.crt" "$ROOT/cert/test.key")
          ;;
        h2c)
          cmd=("$GALAY_H2C_BIN" "39080" "$BENCH_THREADS" "0")
          ;;
        h2)
          cmd=("$GALAY_H2_BIN" "39445" "$BENCH_THREADS" "$ROOT/cert/test.crt" "$ROOT/cert/test.key")
          ;;
      esac
      ;;
  esac

  "${cmd[@]}" >"$log_file" 2>&1 &
  SERVER_PID=$!
}

stop_server() {
  local pid="$1"
  kill "$pid" >/dev/null 2>&1 || true
  wait "$pid" 2>/dev/null || true
}

run_case() {
  local language="$1" proto="$2"
  set_addr "$language" "$proto"
  local addr="$ADDR"
  local path
  local conns
  local duration
  local size
  path="$(proto_path "$proto")"
  conns="$(proto_conns "$proto")"
  duration="$(proto_duration "$proto")"
  size="$(proto_size "$proto")"
  local prefix="${language}_${proto}"
  local sample_txt="$OUT_DIR/${prefix}.sample.txt"
  local folded="$OUT_DIR/${prefix}.folded"
  local flame="$OUT_DIR/${prefix}.flame.svg"
  local run_log="$OUT_DIR/${prefix}.run.log"

  echo "[run] $language $proto addr=$addr conns=$conns duration=${duration}s"

  start_server "$language" "$proto"
  local server_pid="$SERVER_PID"
  trap 'stop_server "$server_pid"' RETURN

  if ! wait_port_ready "$addr" 12000; then
    echo "[fatal] server not ready: $language/$proto ($addr)" >&2
    tail -n 50 "$OUT_DIR/${language}_${proto}_server.log" >&2 || true
    exit 1
  fi

  sample "$server_pid" 5 -file "$sample_txt" >/dev/null 2>&1 &
  local sample_pid=$!

  "$CLIENT_BIN" \
    --proto "$proto" \
    --addr "$addr" \
    --path "$path" \
    --conns "$conns" \
    --duration "$duration" \
    --size "$size" | tee "$run_log"

  wait "$sample_pid" || true

  if inferno-collapse-sample "$sample_txt" > "$folded"; then
    if [[ -s "$folded" ]]; then
      inferno-flamegraph "$folded" > "$flame" || true
    else
      echo "[warn] empty folded stack for $language/$proto" >&2
      : >"$flame"
    fi
  else
    echo "[warn] collapse sample failed for $language/$proto" >&2
    : >"$folded"
    : >"$flame"
  fi

  local line
  line="$(grep '^RESULT ' "$run_log" | tail -n 1)"
  if [[ -z "$line" ]]; then
    echo "[warn] missing RESULT line for $language/$proto" >&2
  else
    local success fail rps avg_ms
    success="$(parse_metric "$line" success)"
    fail="$(parse_metric "$line" fail)"
    rps="$(parse_metric "$line" rps)"
    avg_ms="$(parse_metric "$line" avg_ms)"
    echo "$language,$proto,$addr,$conns,$duration,$success,$fail,$rps,$avg_ms" >> "$CSV_FILE"
  fi

  stop_server "$server_pid"
  trap - RETURN
}

for language in galay go rust; do
  for proto in http https ws wss h2c h2; do
    run_case "$language" "$proto"
  done
done

{
  echo "# Galay-HTTP vs Go vs Rust Protocol Benchmark"
  echo
  echo "Output directory: \`$OUT_DIR\`"
  echo
  echo "Unified threads: \`$BENCH_THREADS\`"
  echo
  echo "| language | proto | addr | conns | duration | success | fail | rps | avg_ms |"
  echo "|---|---|---|---:|---:|---:|---:|---:|---:|"
  tail -n +2 "$CSV_FILE" | awk -F',' '{printf("| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",$1,$2,$3,$4,$5,$6,$7,$8,$9)}'
} > "$SUMMARY_FILE"

echo "[done] results: $OUT_DIR"
