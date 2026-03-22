#!/usr/bin/env python3
"""
Test HTTP server for --mode capture in VisionStudio.

Simulates the camera control console app that VisionStudio communicates with.

Usage:
    python tools/test_capture_server.py [--port 8080] [--tiff /path/to/image.tiff]

Endpoints:
    POST /connect           -> 200 OK
    POST /start             -> 200 OK (capture starts)
    POST /stop              -> 200 OK, SSE "capture_done" with download URL (after 0.5s)
    POST /disconnect        -> 200 OK, SSE "disconnected" event
    GET  /events            -> SSE stream
    GET  /capture/latest    -> download the TIFF file
"""

import argparse
import json
import os
import queue
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


sse_clients: list[queue.Queue] = []
sse_clients_lock = threading.Lock()

DEFAULT_TIFF = "/tmp/captured_image.tiff"
capture_tiff_path = DEFAULT_TIFF
server_port = 8080


def broadcast_event(event: str, data: dict) -> None:
    """Send an SSE event to all connected clients."""
    msg = f"event: {event}\ndata: {json.dumps(data)}\n\n"
    with sse_clients_lock:
        for q in sse_clients:
            q.put(msg)
    print(f"[SSE broadcast] event={event} data={data}")


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # noqa: A002
        print(f"[HTTP] {self.address_string()} - {format % args}")

    # ------------------------------------------------------------------ POST
    def do_POST(self):
        if self.path == "/connect":
            self._send_json(200, {"status": "ok"})

        elif self.path == "/start":
            self._send_json(200, {"status": "ok"})
            print("[Server] Capture started.")

        elif self.path == "/stop":
            self._send_json(200, {"status": "ok"})
            url = f"http://127.0.0.1:{server_port}/capture/latest"
            threading.Timer(
                0.5,
                broadcast_event,
                args=("capture_done", {"url": url}),
            ).start()
            print(f"[Server] Capture stopped. Will send capture_done url={url}")

        elif self.path == "/disconnect":
            self._send_json(200, {"status": "ok"})
            threading.Timer(0.2, broadcast_event, args=("disconnected", {})).start()

        else:
            self._send_json(404, {"error": "not found"})

    # ------------------------------------------------------------------ GET
    def do_GET(self):
        if self.path == "/events":
            self._handle_sse()
        elif self.path == "/capture/latest":
            self._serve_tiff()
        else:
            self._send_json(404, {"error": "not found"})

    def _serve_tiff(self):
        if not os.path.exists(capture_tiff_path):
            self._send_json(404, {"error": f"tiff not found: {capture_tiff_path}"})
            print(f"[Server] TIFF not found: {capture_tiff_path}")
            return
        with open(capture_tiff_path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "image/tiff")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print(f"[Server] Served TIFF ({len(data)} bytes): {capture_tiff_path}")

    def _handle_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.flush()  # send response headers immediately

        client_queue: queue.Queue = queue.Queue()
        with sse_clients_lock:
            sse_clients.append(client_queue)
        print(f"[SSE] Client connected. Total clients: {len(sse_clients)}")

        try:
            while True:
                try:
                    msg = client_queue.get(timeout=15.0)
                    self.wfile.write(msg.encode("utf-8"))
                    self.wfile.flush()
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
        except OSError:
            pass
        finally:
            with sse_clients_lock:
                sse_clients.remove(client_queue)
            print(f"[SSE] Client disconnected. Total clients: {len(sse_clients)}")

    def _send_json(self, code: int, body: dict) -> None:
        data = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    parser = argparse.ArgumentParser(description="VisionStudio capture test server")
    parser.add_argument("--port", type=int, default=8080, help="Port to listen on (default: 8080)")
    parser.add_argument(
        "--tiff",
        default=DEFAULT_TIFF,
        help=f"TIFF file to serve at /capture/latest (default: {DEFAULT_TIFF})",
    )
    args = parser.parse_args()

    global capture_tiff_path, server_port
    capture_tiff_path = args.tiff
    server_port = args.port

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Test capture server listening on http://127.0.0.1:{args.port}")
    print(f"  TIFF file: {capture_tiff_path}")
    print()
    print("Available endpoints:")
    print("  POST /connect        -> 200 OK")
    print("  POST /start          -> 200 OK (capture starts)")
    print("  POST /stop           -> SSE 'capture_done' with download URL (after 0.5s)")
    print("  POST /disconnect     -> SSE 'disconnected' event")
    print("  GET  /events         -> SSE stream")
    print("  GET  /capture/latest -> download TIFF")
    print()
    print("Press Ctrl+C to stop.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Server] Shutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
