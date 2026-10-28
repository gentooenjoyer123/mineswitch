#!/usr/bin/env python3
"""Run the real libcurl tests against a temporary, loopback-only HTTPS server.

Usage: python3 launcher/tests/http_integration.py PATH_TO_HTTP_TESTS_BINARY
The fixture generates its own certificate; it does not change system trust.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time


ARTIFACT = b"MineSwitch HTTPS fixture\n" * 64
METADATA = b'{"name":"local fixture","version":1}'


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            self.close_connection = True

    def log_message(self, *_):
        pass

    def fixed(self, status, data, content_type="text/plain", location=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Fixture", self.headers.get("X-Fixture", ""))
        if location:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(data)
        self.wfile.flush()

    def stream(self, data, delay):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        for offset in range(0, len(data), 256):
            chunk = data[offset:offset + 256]
            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.flush()
            time.sleep(delay)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def do_GET(self):
        try:
            if self.path == "/metadata":
                self.fixed(200, METADATA, "application/json")
            elif self.path == "/artifact":
                self.fixed(200, ARTIFACT, "application/octet-stream")
            elif self.path == "/redirect-metadata":
                self.fixed(302, b"Moved metadata\n", location="/metadata")
            elif self.path == "/redirect-artifact":
                self.fixed(302, b"Moved artifact\n", location="/artifact")
            elif self.path == "/stream":
                self.stream(ARTIFACT, 0.015)
            elif self.path == "/slow":
                self.stream(ARTIFACT * 128, 0.015)
            elif self.path == "/empty":
                self.fixed(200, b"")
            else:
                self.fixed(404, b"Ordinary missing fixture\n")
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            # Cancellation and finite budgets intentionally close the client.
            self.close_connection = True

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.fixed(200, body, "application/json")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mineswitch-https-") as temporary:
        root = Path(temporary)
        key, certificate = root / "key.pem", root / "certificate.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256",
            "-nodes", "-days", "1", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout", str(key), "-out", str(certificate),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        output = root / "output"
        output.mkdir()
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        environment = os.environ.copy()
        environment["NO_PROXY"] = environment["no_proxy"] = "127.0.0.1,localhost"
        try:
            subprocess.run([
                str(binary), f"https://127.0.0.1:{server.server_port}",
                str(certificate), str(output),
            ], check=True, env=environment, timeout=45)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == "__main__":
    main()
