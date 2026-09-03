"""
ESP Garden — local frontend simulator.

Serves the firmware ``data/`` directory and mocks the device HTTP endpoints
(``/data.json``, ``/logs``, ``/control``, ``/updateEnable``, ``/update``) so
that the UI can be developed and tested on a desktop without flashing the
ESP32.

Usage:
    python scripts/dev_server.py            # http://localhost:8080
    python scripts/dev_server.py --port 9000
    python scripts/dev_server.py --host 0.0.0.0

No external dependencies — only the Python standard library.

The simulated device, its login, its stored documents and the moisture model
live in the sim_*.py modules beside this file; what stays here is the HTTP
layer that dispatches to them.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# Plain sibling modules: this file is run as a script, so scripts/ is sys.path[0].
# sim_moisture is imported as a module as well, because main() has to write back
# into its MOISTURE_SCENARIO global.
import sim_moisture
from sim_auth import AUTH, AuthSim
from sim_config import (SIM_CONFIG, SIM_USERS, config_apply, config_masked,
                        users_apply)
from sim_moisture import moisture_scenario_names, moisture_snapshot
from sim_state import STATE, _ticker

ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = ROOT / "data"

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".ico": "image/x-icon",
    ".png": "image/png",
    ".svg": "image/svg+xml",
    ".pem": "text/plain; charset=utf-8",
}


# Served without a token: the pages carry no data, and the login page has to
# load before a token can exist. Mirrors servePublicFile() in src/web.cpp.
PUBLIC_PATHS = {
    "/",
    "/index.html",
    "/index.js",
    "/login.html",
    "/login.js",
    "/sha256.js",
    "/auth.js",
    "/update.html",
    "/update.js",
    "/config.html",
    "/config.js",
    "/users.html",
    "/users.js",
    "/history.html",
    "/history.js",
    "/devices.html",
    "/devices_model.js",
    "/devices_render.js",
    "/devices.js",
    "/schedules.html",
    "/schedules.js",
    "/moisture.html",
    "/moisture.js",
    "/bootstrap.css",
    "/jquery.js",
    "/spark-md5.js",
    "/favicon.ico",
}

# Same cap as g_historyMaxResponse in src/web.cpp: the device cannot render a
# larger reply without running out of DRAM, so the simulator must not either.
HISTORY_MAX_RESPONSE = 200


class Handler(BaseHTTPRequestHandler):
    server_version = "ESPGardenSim/1.0"

    # Reduce log noise.
    def log_message(self, format: str, *args) -> None:  # noqa: A002 - stdlib signature
        pass

    # ---------- helpers ----------
    def _send(self, status: int, body: bytes = b"", content_type: str = "text/plain; charset=utf-8") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    def _serve_static(self, rel_path: str) -> None:
        if rel_path in ("", "/"):
            rel_path = "index.html"
        rel_path = rel_path.lstrip("/")
        target = (DATA_DIR / rel_path).resolve()
        try:
            target.relative_to(DATA_DIR.resolve())
        except ValueError:
            self._send(HTTPStatus.FORBIDDEN, b"forbidden")
            return
        # Mirrors AsyncFileResponse: when the plain file is absent, serve the
        # .gz beside it with Content-Encoding. Vendored assets ship compressed
        # so the filesystem has room for the history buffer.
        gz = target.with_name(target.name + ".gz")
        if not target.is_file() and gz.is_file():
            ctype = CONTENT_TYPES.get(target.suffix.lower(), "application/octet-stream")
            body = gz.read_bytes()
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if not target.is_file():
            self._send(HTTPStatus.NOT_FOUND, b"not found")
            return
        ctype = CONTENT_TYPES.get(target.suffix.lower(), "application/octet-stream")
        self._send(HTTPStatus.OK, target.read_bytes(), ctype)

    # ---------- routes ----------
    # ---------- auth ----------
    def _authorized(self) -> bool:
        return AUTH.valid(self.headers.get("Authorization-Token") or "")

    def _unauthorized(self) -> None:
        self._send(HTTPStatus.UNAUTHORIZED, b"Unauthorized")

    # ---------- routes ----------
    def do_GET(self) -> None:  # noqa: N802 - stdlib signature
        url = urlparse(self.path)
        path = url.path

        if path == "/nonce":
            username = parse_qs(url.query).get("username", [""])[0]
            self._send_json(AUTH.issue_nonce(username))
            return

        if path in PUBLIC_PATHS:
            self._serve_static(path)
            return

        if not self._authorized():
            self._unauthorized()
            return

        if path == "/data.json":
            self._send_json(STATE.snapshot())
        elif path == "/config.json":
            # ?secrets=1 mirrors the device's explicit backup export.
            if parse_qs(url.query).get("secrets", [""])[0] == "1":
                STATE.log("warning", "Config exported WITH secrets")
                self._send_json(SIM_CONFIG)
            else:
                self._send_json(config_masked())
        elif path == "/users.json":
            self._send_json(SIM_USERS)
        elif path == "/moisture.json":
            # ?scenario= forces every probe onto one scenario for this request
            # only. The device has no such parameter — it exists because the
            # states worth checking (bands that overlap, a model short of
            # watering events, a device that has never trained) take days to
            # reach on real hardware and seconds here.
            self._send_json(moisture_snapshot(
                parse_qs(url.query).get("scenario", [""])[0]))
        elif path == "/capabilities.json":
            # Mirrors src/web_capabilities.cpp, derived from the same three
            # predicates rather than a copied list, so the two cannot drift.
            def is_flash(p): return 6 <= p <= 11
            def is_adc1(p): return 32 <= p <= 39 and p not in (37, 38)
            def is_input_only(p): return 34 <= p <= 39
            def is_strapping(p): return p in (0, 2, 5, 12, 15)
            def is_bonded(p):
                return not (p == 20 or p == 24 or 28 <= p <= 31) and p not in (37, 38)
            def is_serial(p):
                return p in (1, 3)
            # Mirrors web_capabilities.cpp: unbonded pins are never offered and
            # the serial console is listed separately. Without this the page
            # validated a relay on GPIO 28 here and got a 400 from the device.
            pins = [p for p in range(40)
                    if not is_flash(p) and is_bonded(p) and not is_serial(p)]
            self._send_json({
                "firmware": "2.2.0",
                "relayMax": 8,
                "moistureMax": 4,
                # FILESYSTEM_MAX_PATH. /update.html reads it from here rather
                # than restating the limit, so the simulator has to carry it or
                # the upload page silently falls back to its own literal.
                "maxPathLength": 31,
                "kinds": ["relays", "soilMoisture", "dht", "luminosity",
                          "waterLevel", "flow", "floatSwitch"],
                "analogPins": [p for p in pins if is_adc1(p)],
                "outputPins": [p for p in pins if not is_input_only(p)],
                "digitalPins": [p for p in pins if not is_input_only(p)],
                "strappingPins": [p for p in pins if is_strapping(p)],
                "reservedPins": [p for p in range(40) if is_serial(p)],
            })
        elif path == "/history.json":
            if not STATE.history_enabled:
                self._send(HTTPStatus.SERVICE_UNAVAILABLE,
                           b"history buffer not available")
                return
            limit = 100
            raw = parse_qs(url.query).get("limit", [""])[0]
            if raw.isdigit() and int(raw) > 0:
                limit = int(raw)
            limit = min(limit, HISTORY_MAX_RESPONSE)
            raw_offset = parse_qs(url.query).get("offset", [""])[0]
            raw_window = parse_qs(url.query).get("window", [""])[0]
            window = int(raw_window) if raw_window.lstrip("-").isdigit() else 0
            stride = 1

            with STATE.lock:
                everything = list(STATE.history)

                if window > 0:
                    # ?window=<seconds> selects by time and DECIMATES to fit,
                    # exactly as handleHistoryJson does. Without mirroring it
                    # every window button here returns the same newest 200
                    # records and the page reports a range it is not showing.
                    since = time.time() - window
                    start = 0
                    for i, record in enumerate(everything):
                        if record["t"] >= since:
                            start = i
                            break
                    else:
                        start = len(everything)

                    span = len(everything) - start
                    stride = max(1, -(-span // limit))  # ceiling division

                    # Walk backwards from the newest so the last sample is
                    # always present, and OR the relay mask across each bucket:
                    # the mask is sticky on the device and sampling it would
                    # drop seven of every eight waterings at a 1-day window.
                    picked = []
                    bucket = None
                    bucket_relays = 0
                    for back in range(span):
                        record = everything[len(everything) - 1 - back]
                        if bucket is None:
                            bucket = dict(record)
                        bucket_relays |= record["relays"]
                        if back % stride == stride - 1 or back == span - 1:
                            bucket["relays"] = bucket_relays
                            picked.append(bucket)
                            bucket = None
                            bucket_relays = 0
                        if len(picked) >= limit:
                            break
                    records = list(reversed(picked))
                    skip = start
                elif raw_offset.isdigit():
                    skip = int(raw_offset)
                    records = everything[skip:skip + limit]
                else:
                    skip = max(0, len(everything) - limit)
                    records = everything[-limit:]

                payload = {
                    "capacity": STATE.history_capacity,
                    "stored": len(STATE.history),
                    "returned": len(records),
                    "offset": skip,
                    "stride": stride,
                    "window": window,
                    "records": records,
                }
            self._send_json(payload)
        elif path == "/logs":
            body = STATE.logs_text().encode("utf-8")
            self._send(HTTPStatus.OK, body, "text/plain; charset=utf-8")
        else:
            # The firmware only registers an explicit allow-list of files; it
            # has no blanket static handler on "/" any more.
            self._send(HTTPStatus.NOT_FOUND, b"not found")

    def do_POST(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        path = url.path
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""

        if path == "/login":
            params = parse_qs(raw.decode("utf-8", errors="replace"))
            token = AUTH.login(
                params.get("username", [""])[0],
                params.get("nonce", [""])[0],
                params.get("response", [""])[0],
            )
            if token is None:
                STATE.log("warning", "Unauthorized access attempt")
                self._send(HTTPStatus.UNAUTHORIZED, b"Unauthorized")
            else:
                STATE.log("info", f"Login OK: user='{AUTH.USERNAME}'")
                self._send_json({"token": token, "role": AUTH.ROLE_ADMIN})
            return

        if path == "/logout":
            AUTH.logout(self.headers.get("Authorization-Token") or "")
            self._send(HTTPStatus.OK, b"OK")
            return

        if not self._authorized():
            self._unauthorized()
            return

        if path == "/users":
            params = {k: v[0] for k, v in
                      parse_qs(raw.decode("utf-8", errors="replace")).items()}
            status, message, reauth = users_apply(params)
            if status != 200:
                self._send(status, message.encode())
            else:
                STATE.log("info", message)
                self._send_json({"reauth": reauth})
            return

        if path == "/config.json":
            params = parse_qs(raw.decode("utf-8", errors="replace"))
            if "config" not in params:
                self._send(HTTPStatus.BAD_REQUEST, b"missing 'config' parameter")
                return
            try:
                incoming = json.loads(params["config"][0])
            except ValueError:
                self._send(HTTPStatus.BAD_REQUEST, b"config is not a JSON object")
                return
            status, message = config_apply(incoming)
            if status != 200:
                STATE.log("error", f"Refusing to save config: {message}")
                self._send(status, message.encode())
                return
            STATE.log("info", "Saved /config.json")
            self._send_json({"saved": True, "restartRequired": True})
            return

        if path == "/control":
            params = parse_qs(raw.decode("utf-8", errors="replace"))
            self._handle_control({k: v[0] for k, v in params.items()})
            self._send(HTTPStatus.OK, b"ok")
        elif path == "/updateEnable":
            STATE.log("info", "[OTA] Enabled OTA")
            self._send(HTTPStatus.OK, b"ok")
        elif path == "/update":
            # Accepts, and then does the one thing the page actually watches
            # for: reports a different firmware version afterwards. A firmware
            # image bumps it; a filesystem image deliberately does not, because
            # on the device it does not either -- which is the case the page
            # has to fall back to a reboot for.
            STATE.log("info", f"[OTA] Received {length} bytes (simulated)")
            if b'filename="filesystem"' not in raw:
                parts = STATE.firmware.split(".")
                parts[-1] = str(int(parts[-1]) + 1)
                STATE.firmware = ".".join(parts)
                STATE.log("info", f"[OTA] Now reporting {STATE.firmware}")
            self._send(HTTPStatus.OK, b"OK")
        elif path == "/spiffs/upload":
            self._handle_file_upload(raw)
        elif path == "/spiffs/delete":
            self._handle_file_delete(raw)
        else:
            self._send(HTTPStatus.NOT_FOUND, b"not found")

    # ---------- single-file upload ----------
    # Mirrors src/web_files.cpp, refusals included. It deliberately does NOT
    # write anything: the simulator serves the repository's data/ directory, so
    # an upload that landed on disk would turn a UI test into an edit of the
    # source tree.
    UPLOAD_PROTECTED = ("/users", "/sessions")

    @classmethod
    def _upload_path_problem(cls, path: str):
        """One copy of uploadPathIsUsable(), shared by upload and delete.

        Two copies would drift, and the drift would be a path one endpoint
        refuses and the other accepts.
        """
        if len(path) < 2:
            return "path must start with a slash"
        if ".." in path:
            return "path must not contain .."
        if len(path) > 31:
            return "path longer than 31 characters"
        if path == "/upload.tmp":
            return "reserved path"
        if path.startswith("/config"):
            return "use POST /config.json, which validates the document"
        if path.startswith(cls.UPLOAD_PROTECTED):
            return "credential store"
        return None

    def _handle_file_upload(self, raw: bytes) -> None:
        filename, body, fields = self._parse_multipart(raw)
        if filename is None:
            self._send_json({"ok": False, "error": "no file in request"},
                            HTTPStatus.BAD_REQUEST)
            return

        path = filename if filename.startswith("/") else "/" + filename

        reason = self._upload_path_problem(path)
        if reason:
            STATE.log("warning", f"[upload] refused {path}: {reason}")
            self._send_json({"ok": False, "error": f"refused {path}: {reason}"},
                            HTTPStatus.BAD_REQUEST)
            return

        expected = fields.get("MD5")
        if expected:
            actual = hashlib.md5(body).hexdigest()
            if actual.lower() != expected.lower():
                self._send_json(
                    {"ok": False,
                     "error": f"checksum mismatch: expected {expected}, "
                              f"got {actual}"},
                    HTTPStatus.BAD_REQUEST)
                return

        STATE.log("warning", f"[upload] wrote {path} ({len(body)} B) (simulated)")
        self._send_json({"ok": True, "path": path, "bytes": len(body),
                         "free": (463 - 165) * 1024})

    def _handle_file_delete(self, raw: bytes) -> None:
        """Mirrors handleFileDelete in src/web_files.cpp, refusals included.

        Like the upload, it does not touch disk: the simulator serves the
        repository's data/ directory and a delete that landed would remove a
        source file.
        """
        params = parse_qs(raw.decode("utf-8", errors="replace"))
        path = params.get("path", [""])[0]
        if not path:
            self._send_json({"ok": False, "error": "missing path"},
                            HTTPStatus.BAD_REQUEST)
            return
        if not path.startswith("/"):
            path = "/" + path

        reason = self._upload_path_problem(path)
        if reason:
            STATE.log("warning", f"[delete] refused {path}: {reason}")
            self._send_json({"ok": False, "error": f"refused {path}: {reason}"},
                            HTTPStatus.BAD_REQUEST)
            return

        # A path that is not there answers 404 rather than reporting success —
        # a wrong path has to be visible, not silently accepted.
        if not (DATA_DIR / path.lstrip("/")).exists():
            self._send_json({"ok": False, "error": f"{path} does not exist"},
                            HTTPStatus.NOT_FOUND)
            return

        STATE.log("warning", f"[delete] removed {path} (simulated)")
        self._send_json({"ok": True, "path": path,
                         "free": (463 - 165) * 1024})

    @staticmethod
    def _parse_multipart(raw: bytes):
        """Returns (filename, file bytes, {other form fields}).

        Deliberately minimal: enough for the one shape data/update.js sends.
        The point is to exercise the client and the refusals, not to be a
        multipart implementation.
        """
        crlf = b"\r\n"
        boundary = raw.partition(crlf)[0].strip()
        if not boundary.startswith(b"--"):
            return None, b"", {}

        filename, file_body, fields = None, b"", {}
        for part in raw.split(boundary):
            header, sep, content = part.partition(crlf + crlf)
            if not sep:
                continue
            # EXACTLY the one CRLF that separates this part from the next
            # boundary. rstrip() would strip any trailing newline, including
            # the file's own — which is a checksum mismatch on every text file
            # that ends the way text files end.
            if content.endswith(crlf):
                content = content[:-2]
            disposition = header.decode("utf-8", "replace")
            match = re.search(r'filename="([^"]*)"', disposition)
            if match:
                filename, file_body = match.group(1), content
                continue
            match = re.search(r'name="([^"]+)"', disposition)
            if match:
                fields[match.group(1)] = content.decode("utf-8", "replace").strip()
        return filename, file_body, fields

    # ---------- control parser ----------
    def _handle_control(self, params: dict[str, str]) -> None:
        if "relay" in params:
            try:
                index = int(params["relay"])
                ms = int(params.get("relayTime", 5000))
            except ValueError:
                index, ms = -1, 0
            # An explicit stop, matching handleControl() in src/web.cpp. It is
            # not "relayTime == 0" there because String::toInt() answers 0 for
            # unparseable input, which would make a malformed field stop a pump.
            if "relayStop" in params:
                STATE.stop_relay(index)
            else:
                STATE.start_relay(index, ms)
        if "wateringTime" in params:
            try:
                ms = int(params["wateringTime"])
            except ValueError:
                ms = 0
            STATE.start_watering(ms)
        if params.get("watering") == "enable":
            STATE.start_watering(5000)
        if "mqtt" in params:
            STATE.set_mqtt(params["mqtt"] == "enable")
        if params.get("reset") == "1":
            STATE.reset()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument(
        "--moisture-scenario", default="", metavar="NAME",
        choices=[""] + moisture_scenario_names(),
        help="force every probe in /moisture.json onto one scenario "
             "(default: a different one per probe). One of: "
             + ", ".join(moisture_scenario_names()))
    args = parser.parse_args()

    # The flag lives in sim_moisture; `global` here would rebind a name in this
    # module instead, and resolve_scenario() would never see it.
    sim_moisture.MOISTURE_SCENARIO = args.moisture_scenario

    if not DATA_DIR.is_dir():
        raise SystemExit(f"data/ not found at {DATA_DIR}")

    threading.Thread(target=_ticker, daemon=True).start()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"ESP Garden simulator serving {DATA_DIR} on {url}")
    print(f"Login: {AuthSim.USERNAME} / {AuthSim.PASSWORD}")
    print("Endpoints: /  /nonce  /login  /logout  /data.json  /logs"
          "  /control  /updateEnable  /update  /moisture.json")
    print("Moisture scenarios: " + ", ".join(moisture_scenario_names())
          + "  (?scenario=NAME on /moisture.json, or --moisture-scenario)")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
