#!/usr/bin/env python3
"""Owned LAN fixture for the Switch Java21 HttpClient integration check."""
from http.server import BaseHTTPRequestHandler, HTTPServer

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"mineswitch-java21-http-ok\n" if self.path == "/java21-check" else b"Not found\n"
        self.send_response(200 if self.path == "/java21-check" else 404)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

if __name__ == "__main__":
    with HTTPServer(("192.168.3.163", 25577), Handler) as server:
        print("JAVA21_HTTP_FIXTURE_READY 192.168.3.163:25577", flush=True)
        server.serve_forever()
