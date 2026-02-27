#!/usr/bin/env python3
"""
Minimal HTTP/2 (TLS + ALPN h2) echo server for benchmark comparison.
"""

from __future__ import annotations

import argparse
import asyncio
import signal
import ssl
import time
from collections import defaultdict
from typing import Dict

from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import DataReceived, RequestReceived, StreamEnded


class H2EchoServer:
    def __init__(self, log_interval: float = 1.0) -> None:
        self._req_count = 0
        self._start_ts = time.monotonic()
        self._log_interval = log_interval
        self._last_log_ts = self._start_ts
        self._lock = asyncio.Lock()

    async def _on_request_done(self) -> None:
        async with self._lock:
            self._req_count += 1
            now = time.monotonic()
            if now - self._last_log_ts >= self._log_interval:
                elapsed = now - self._start_ts
                qps = self._req_count / elapsed if elapsed > 0 else 0.0
                print(f"[python-h2] requests={self._req_count} qps={qps:.2f}", flush=True)
                self._last_log_ts = now

    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        ssl_obj = writer.get_extra_info("ssl_object")
        selected = ssl_obj.selected_alpn_protocol() if ssl_obj else None
        if selected != "h2":
            writer.close()
            await writer.wait_closed()
            return

        conn = H2Connection(config=H2Configuration(client_side=False, header_encoding="utf-8"))
        conn.initiate_connection()
        writer.write(conn.data_to_send())
        await writer.drain()

        stream_bodies: Dict[int, bytearray] = defaultdict(bytearray)

        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break

                events = conn.receive_data(data)
                for event in events:
                    if isinstance(event, RequestReceived):
                        stream_bodies[event.stream_id]
                    elif isinstance(event, DataReceived):
                        stream_bodies[event.stream_id].extend(event.data)
                        conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                    elif isinstance(event, StreamEnded):
                        body = bytes(stream_bodies.pop(event.stream_id, bytearray()))
                        response_headers = [
                            (":status", "200"),
                            ("server", "python-h2-echo/1.0"),
                            ("content-type", "text/plain"),
                            ("content-length", str(len(body))),
                        ]
                        conn.send_headers(event.stream_id, response_headers, end_stream=False)
                        conn.send_data(event.stream_id, body, end_stream=True)
                        await self._on_request_done()

                to_send = conn.data_to_send()
                if to_send:
                    writer.write(to_send)
                    await writer.drain()
        except Exception as exc:  # noqa: BLE001
            print(f"[python-h2] client handler error: {exc}", flush=True)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:  # noqa: BLE001
                pass

    @property
    def req_count(self) -> int:
        return self._req_count

    @property
    def elapsed(self) -> float:
        return time.monotonic() - self._start_ts


async def main() -> None:
    parser = argparse.ArgumentParser(description="Python HTTP/2 TLS echo server (ALPN h2)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=19460)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--log-interval", type=float, default=1.0)
    args = parser.parse_args()

    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
    ssl_ctx.set_alpn_protocols(["h2"])

    server_impl = H2EchoServer(log_interval=args.log_interval)
    server = await asyncio.start_server(
        server_impl.handle_client,
        args.host,
        args.port,
        ssl=ssl_ctx,
        reuse_address=True,
        reuse_port=False,
    )

    sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    print(f"[python-h2] listen={sockets}", flush=True)

    stop_event = asyncio.Event()

    def _stop() -> None:
        stop_event.set()

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, _stop)

    await stop_event.wait()
    server.close()
    await server.wait_closed()

    elapsed = server_impl.elapsed
    qps = server_impl.req_count / elapsed if elapsed > 0 else 0.0
    print(
        f"[python-h2] shutdown requests={server_impl.req_count} elapsed={elapsed:.3f}s qps={qps:.2f}",
        flush=True,
    )


if __name__ == "__main__":
    asyncio.run(main())

