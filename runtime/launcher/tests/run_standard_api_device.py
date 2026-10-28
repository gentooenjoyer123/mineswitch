#!/usr/bin/env python3
"""Keep the owned HTTP fixture alive for exactly one serialized NXLink check."""
from http_fixture import Handler
from http.server import HTTPServer
from pathlib import Path
import subprocess
import threading

root = Path(__file__).resolve().parents[3]
with HTTPServer(("192.168.3.163", 25577), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print("JAVA21_HTTP_FIXTURE_READY 192.168.3.163:25577", flush=True)
    try:
        result = subprocess.run([
            "bash", "scripts/run-nro.sh", "192.168.3.177", "runtime/launcher/MineSwitch-StandardApi.nro",
            "--class=mineswitch.probe.StandardApiProbe", "--jvm-options=romfs:/launch/standard-api.options",
            "--", "http://192.168.3.163:25577/java21-check",
        ], cwd=root)
    finally:
        server.shutdown()
        thread.join()
    raise SystemExit(result.returncode)
