#!/usr/bin/env python3
"""Save the Switch HDMI input from an existing OBS WebSocket v5 server.

Only GetInputList, GetInputSettings and GetSourceScreenshot are used. The
helper never changes OBS configuration, scenes, sources or capture state.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

import websockets
from websockets.exceptions import ConnectionClosed, InvalidHandshake


DEFAULT_CONFIG = Path.home() / ".config/obs-studio/plugin_config/obs-websocket/config.json"
MAX_PNG_BYTES = 16 * 1024 * 1024
MAX_MESSAGE_BYTES = 24 * 1024 * 1024
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
READ_ONLY_REQUESTS = frozenset(("GetInputList", "GetInputSettings", "GetSourceScreenshot"))


class CaptureError(Exception):
    pass


def load_configuration(path):
    try:
        if path.stat().st_size > 64 * 1024:
            raise CaptureError("OBS WebSocket configuration is unexpectedly large.")
        configuration = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        raise CaptureError("Cannot read the local OBS WebSocket configuration.") from None
    if not isinstance(configuration, dict):
        raise CaptureError("Invalid OBS WebSocket configuration.")
    if configuration.get("server_enabled") is not True:
        raise CaptureError(
            "OBS WebSocket server is disabled. In OBS, open Tools > WebSocket Server Settings "
            "and enable the server, then retry. This helper does not change OBS settings."
        )
    port = configuration.get("server_port")
    if type(port) is not int or not 1 <= port <= 65535:
        raise CaptureError("OBS WebSocket configuration has no valid server port.")
    password = configuration.get("server_password", "")
    if not isinstance(password, str):
        raise CaptureError("OBS WebSocket configuration has an invalid credential field.")
    return port, password


def authentication(password, salt, challenge):
    secret = base64.b64encode(hashlib.sha256((password + salt).encode("utf-8")).digest())
    return base64.b64encode(hashlib.sha256(secret + challenge.encode("utf-8")).digest()).decode("ascii")


def is_v4l2(kind):
    return isinstance(kind, str) and (kind == "v4l2_input" or
                                    (kind.startswith("v4l2_input_v") and kind.removeprefix("v4l2_input_v").isdigit()))


def is_switch_device(device):
    if not isinstance(device, str) or not device.startswith("/dev/") or "\0" in device:
        return False
    try:
        path = Path(device).resolve(strict=True)
        # Linux renumbers video nodes when the capture card reconnects. Match
        # its stable USB identity and image endpoint, not a fixed /dev/videoN.
        for identity in Path("/dev/v4l/by-id").iterdir():
            if "macrosilicon" in identity.name.casefold() and identity.name.endswith("-video-index0"):
                if identity.resolve(strict=True) == path:
                    return True
        return False
    except (OSError, RuntimeError):
        return False


class ObsReader:
    def __init__(self, socket):
        self.socket = socket
        self.sequence = 0

    async def receive(self):
        try:
            message = json.loads(await self.socket.recv())
        except (ValueError, TypeError):
            raise CaptureError("OBS returned an invalid WebSocket message.") from None
        if not isinstance(message, dict) or not isinstance(message.get("d"), dict):
            raise CaptureError("OBS returned an unexpected WebSocket message.")
        return message

    async def identify(self, password):
        hello = await self.receive()
        version = hello["d"].get("rpcVersion")
        if hello.get("op") != 0 or type(version) is not int or version < 1:
            raise CaptureError("The server does not support OBS WebSocket v5.")
        identify = {"rpcVersion": 1, "eventSubscriptions": 0}
        challenge = hello["d"].get("authentication")
        if challenge is not None:
            if not isinstance(challenge, dict) or not all(
                    isinstance(challenge.get(key), str) for key in ("salt", "challenge")):
                raise CaptureError("OBS sent an invalid authentication challenge.")
            if not password:
                raise CaptureError("OBS requires authentication but its local credential is unavailable.")
            identify["authentication"] = authentication(password, challenge["salt"], challenge["challenge"])
        await self.socket.send(json.dumps({"op": 1, "d": identify}))
        response = await self.receive()
        if response.get("op") != 2:
            raise CaptureError("OBS did not accept the WebSocket identification.")

    async def request(self, kind, data=None):
        if kind not in READ_ONLY_REQUESTS:
            raise CaptureError("This helper only permits read-only capture requests.")
        self.sequence += 1
        request_id = f"switch-capture-{self.sequence}"
        await self.socket.send(json.dumps({"op": 6, "d": {
            "requestType": kind, "requestId": request_id, "requestData": data or {},
        }}))
        while True:
            message = await self.receive()
            if message.get("op") == 5:
                continue
            response = message["d"]
            if message.get("op") != 7 or response.get("requestId") != request_id:
                raise CaptureError("OBS returned an unexpected request response.")
            status = response.get("requestStatus", {})
            if not isinstance(status, dict) or status.get("result") is not True:
                # Do not print server payloads, credentials or arbitrary input settings.
                raise CaptureError(f"OBS could not complete the read-only {kind} request.")
            result = response.get("responseData", {})
            if not isinstance(result, dict):
                raise CaptureError("OBS returned invalid capture metadata.")
            return result


async def capture(port, password, requested_source=None):
    # Explicit loopback and no proxy: the credential never goes to a remote endpoint.
    async with websockets.connect(
            f"ws://127.0.0.1:{port}", proxy=None, compression=None,
            open_timeout=5, close_timeout=2, ping_interval=None, max_size=MAX_MESSAGE_BYTES) as socket:
        obs = ObsReader(socket)
        await obs.identify(password)
        inputs = (await obs.request("GetInputList")).get("inputs")
        if not isinstance(inputs, list) or len(inputs) > 4096:
            raise CaptureError("OBS returned an invalid input list.")
        matches = []
        for item in inputs:
            if not isinstance(item, dict) or not isinstance(item.get("inputName"), str):
                continue
            name = item["inputName"]
            if requested_source is not None and name != requested_source:
                continue
            if not (is_v4l2(item.get("inputKind")) or is_v4l2(item.get("unversionedInputKind"))):
                continue
            settings = await obs.request("GetInputSettings", {"inputName": name})
            if not is_v4l2(settings.get("inputKind")):
                continue
            values = settings.get("inputSettings")
            if isinstance(values, dict) and is_switch_device(values.get("device_id")):
                matches.append(name)
        if not matches:
            raise CaptureError("No OBS V4L2 input matches the MACROSILICON HDMI capture device.")
        if len(matches) != 1:
            raise CaptureError("Several OBS inputs match the HDMI capture device. Select one with --source NAME.")
        response = await obs.request("GetSourceScreenshot", {"sourceName": matches[0], "imageFormat": "png"})
        data = response.get("imageData")
        prefix = "data:image/png;base64,"
        if not isinstance(data, str) or not data.startswith(prefix):
            raise CaptureError("OBS did not return a PNG screenshot.")
        try:
            png = base64.b64decode(data[len(prefix):], validate=True)
        except ValueError:
            raise CaptureError("OBS returned invalid screenshot data.") from None
        if not 24 <= len(png) <= MAX_PNG_BYTES or not png.startswith(PNG_SIGNATURE):
            raise CaptureError("OBS returned an invalid or oversized PNG screenshot.")
        return png


def save_png(path, contents):
    temporary = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    except OSError:
        raise CaptureError("Cannot save the requested PNG file.") from None
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Destination PNG file")
    parser.add_argument("--source", help="Exact OBS input name, still restricted to the Switch V4L2 device")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="Local OBS WebSocket configuration file")
    arguments = parser.parse_args()
    try:
        if arguments.output.resolve() == arguments.config.resolve():
            raise CaptureError("The screenshot destination cannot replace the OBS configuration.")
        port, password = load_configuration(arguments.config)

        async def bounded_capture():
            async with asyncio.timeout(30):
                return await capture(port, password, arguments.source)

        png = asyncio.run(bounded_capture())
        save_png(arguments.output, png)
    except CaptureError as error:
        print(str(error), file=sys.stderr)
        return 1
    except (OSError, TimeoutError, InvalidHandshake, ConnectionClosed):
        print("Cannot connect or authenticate to the local OBS WebSocket server. Check that the server is enabled in OBS.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Capture cancelled.", file=sys.stderr)
        return 130
    print(f"Saved Switch HDMI capture: {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
