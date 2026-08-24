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
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import secrets
import threading
import time
from collections import deque
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

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


class DeviceState:
    """In-memory simulation of the ESP Garden device state."""

    LOG_CAPACITY = 400

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.boot_time = time.time()
        self.hostname = "espgarden-sim"
        self.channel = "1348790"
        self.mqtt_enabled = True
        self.packages_sent = 0
        self.watering_cycles = 0
        self.connection_loss_count = 0
        self.dht_total_reads = 0
        self.dht_read_errors = 0
        self.logs: deque[str] = deque(maxlen=self.LOG_CAPACITY)

        # Mirrors the on-device ring buffer in src/io_history.cpp. A deque with
        # maxlen IS a ring buffer, so the simulator gets the same drop-oldest
        # behaviour without the file. Capacity and period come from the same
        # config block the device reads — hardcoding them let the simulator
        # report a capacity the device would never return.
        history_cfg = SIM_CONFIG.get("history", {})
        self.history_capacity = int(history_cfg.get("records", 1440))
        # records = 0 means disabled on the device, where /history.json answers
        # 503. Coercing it to 1 here made that branch unreachable in the UI.
        self.history_enabled = self.history_capacity > 0
        self.history: deque[dict] = deque(
            maxlen=self.history_capacity if self.history_enabled else 1)
        self.history_period_s = int(history_cfg.get("periodSec", 60))
        self._history_next = 0.0

        # Mirrors config.io.relays. Index 0 is the watering relay, as in the
        # firmware — TalkBack and the legacy `watering` control target it.
        self.relay_names = ["Watering", "Relay 2", "Relay 3", "Relay 4"]
        self.relays = [{"on": 0, "until": 0.0} for _ in self.relay_names]

        # Sensor accumulators: rolling state to fake sensible averages/variance.
        self._sensors = {
            "Soil Moisture 1": _Sensor(45.0, amplitude=8.0, period=900, noise=1.5),
            "Soil Moisture 2": _Sensor(38.0, amplitude=6.0, period=1100, noise=1.5),
            "Soil Moisture 3": _Sensor(52.0, amplitude=7.0, period=1300, noise=1.5),
            "Luminosity":      _Sensor(55.0, amplitude=35.0, period=120, noise=4.0),
            "Temperature":     _Sensor(25.5, amplitude=3.0, period=1800, noise=0.4),
            "Air Humidity":    _Sensor(70.0, amplitude=8.0, period=2400, noise=1.5),
        }

        self._seed_history()
        self.log("info", "Simulator booted")

    def _seed_history(self) -> None:
        """Backdated records so the charts are testable immediately.

        Without this the deque starts empty and gains one record per period:
        every path the history page adds — gap breaking, the crosshair index
        map, the relay run-length strip — is unreachable for hours.
        """
        if not self.history_enabled:
            return
        import math as _math
        now = time.time()
        count = min(self.history_capacity, 400)
        for i in range(count, 0, -1):
            t = now - i * self.history_period_s
            phase = i / 30.0
            self.history.append({
                "t": int(t),
                "relays": 1 if i % 37 == 0 else 0,
                "moisture": [
                    round(45 + 6 * _math.sin(phase), 2),
                    round(38 + 4 * _math.sin(phase + 1), 2),
                    round(52 + 5 * _math.sin(phase + 2), 2),
                    None,
                ],
                "lum": round(max(0.0, 55 + 40 * _math.sin(i / 60.0)), 2),
                "temp": round(25 + 3 * _math.sin(phase / 2), 2),
                "hum": round(70 + 8 * _math.sin(phase / 3), 2),
                "water": None,
            })

    # ----- logging -----
    def log(self, level: str, message: str) -> None:
        ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        letter = level[0].upper() if level else "I"
        line = f"[{ts}] [{letter}] {message}"
        with self.lock:
            self.logs.append(line)

    def logs_text(self) -> str:
        with self.lock:
            return "\n".join(self.logs) + "\n"

    # ----- control -----
    def start_relay(self, index: int, duration_ms: int) -> None:
        if not 0 <= index < len(self.relays):
            self.log("error", f"Invalid relay index: {index}")
            return
        # The firmware caps a relay at 20 s (g_relayMaxTime) and rejects 0.
        if duration_ms <= 0 or duration_ms > 20_000:
            self.log("error", f"Invalid relay time: {duration_ms}")
            return
        # self.log() takes the same non-reentrant lock, so every log call has to
        # stay outside the guarded block.
        with self.lock:
            busy = bool(self.relays[index]["on"])
            if not busy:
                self.relays[index]["on"] = 1
                self.relays[index]["until"] = time.time() + duration_ms / 1000.0
                if index == 0:
                    self.watering_cycles += 1

        if busy:
            self.log("warning", f"{self.relay_names[index]} already active.")
            return

        self.log("info", f"Starting {self.relay_names[index]} for {duration_ms} ms")

    def start_watering(self, duration_ms: int) -> None:
        self.start_relay(0, duration_ms)

    def set_mqtt(self, enabled: bool) -> None:
        with self.lock:
            self.mqtt_enabled = enabled
        self.log("info", f"MQTT {'enabled' if enabled else 'disabled'}")

    def reset(self) -> None:
        self.log("warning", "Reset requested (simulator: re-seeding state)")
        with self.lock:
            self.boot_time = time.time()
            self.packages_sent = 0
            self.watering_cycles = 0
            self.connection_loss_count = 0
            for relay in self.relays:
                relay["on"] = 0
                relay["until"] = 0.0

    # ----- snapshot -----
    def tick(self) -> None:
        """Advance simulated state once per second."""
        now = time.time()
        finished = []
        with self.lock:
            for index, relay in enumerate(self.relays):
                if relay["on"] and now >= relay["until"]:
                    relay["on"] = 0
                    finished.append(self.relay_names[index])
            for sensor in self._sensors.values():
                sensor.update()
            # Mqtt publishes a "package" every 30s when enabled.
            if self.mqtt_enabled and int(now - self.boot_time) % 30 == 0:
                self.packages_sent += 1
            # Random DHT reads
            self.dht_total_reads += 1
            if random.random() < 0.02:
                self.dht_read_errors += 1

            if now >= self._history_next:
                self._history_next = now + self.history_period_s
                mask = 0
                for i, relay in enumerate(self.relays):
                    if relay["on"]:
                        mask |= 1 << i
                # Absent channels are null, not 0 — the device writes NaN and
                # the reader has to tell "not fitted" from "read zero".
                self.history.append({
                    "t": int(now),
                    "relays": mask,
                    "moisture": [
                        round(self._sensors["Soil Moisture 1"].average, 2),
                        round(self._sensors["Soil Moisture 2"].average, 2),
                        round(self._sensors["Soil Moisture 3"].average, 2),
                        None,
                    ],
                    "lum": round(self._sensors["Luminosity"].average, 2),
                    "temp": round(self._sensors["Temperature"].average, 2),
                    "hum": round(self._sensors["Air Humidity"].average, 2),
                    "water": None,
                })

        # Logging takes the same lock, so it cannot happen inside the block.
        for name in finished:
            self.log("info", f"{name} finished")

    def snapshot(self) -> dict:
        with self.lock:
            uptime = int(time.time() - self.boot_time)
            days, rem = divmod(uptime, 86400)
            hours, rem = divmod(rem, 3600)
            minutes, seconds = divmod(rem, 60)

            status = {
                "Hostname": self.hostname,
                "Firmware": "2.0.0",
                "Date/Time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "Uptime": f"{days}d {hours}h {minutes}m {seconds}s",
                "Internet": "online",
                "Signal Strength": f"{random.randint(70, 95)}%",
                "Ping": f"{random.randint(15, 60)}ms",
                "Connection Loss Count": str(self.connection_loss_count),
                "MQTT": "enabled" if self.mqtt_enabled else "disabled",
                "Packages Sent": str(self.packages_sent),
                "Watering Cycles": str(self.watering_cycles),
                "Filesystem": "139 / 463 KB",
            }
            if self.dht_total_reads:
                rate = self.dht_read_errors / self.dht_total_reads * 100
                status["DHT Error Rate"] = f"{rate:.2f}"

            inputs = {
                name: {
                    "val": f"{s.value:.2f}",
                    "avg": f"{s.average:.2f}",
                    "var": f"{s.variance:.4f}",
                }
                for name, s in self._sensors.items()
            }

            outputs = {
                name: str(self.relays[i]["on"])
                for i, name in enumerate(self.relay_names)
            }
            relays = [
                {
                    "index": i,
                    "name": name,
                    "on": self.relays[i]["on"],
                    "remaining": max(
                        0, int((self.relays[i]["until"] - time.time()) * 1000)
                    )
                    if self.relays[i]["on"]
                    else 0,
                }
                for i, name in enumerate(self.relay_names)
            ]

            return {
                "Status": status,
                "Inputs": inputs,
                "Outputs": outputs,
                "Relays": relays,
                "Channel": self.channel,
            }


class _Sensor:
    """Sinusoid with noise + running average/variance."""

    def __init__(self, baseline: float, amplitude: float, period: float, noise: float) -> None:
        self.baseline = baseline
        self.amplitude = amplitude
        self.period = period
        self.noise = noise
        self.t0 = time.time()
        self.value = baseline
        self.samples: deque[float] = deque(maxlen=64)
        self.average = baseline
        self.variance = 0.0

    def update(self) -> None:
        t = time.time() - self.t0
        wave = self.amplitude * math.sin(2 * math.pi * t / self.period)
        self.value = self.baseline + wave + random.uniform(-self.noise, self.noise)
        self.samples.append(self.value)
        n = len(self.samples)
        self.average = sum(self.samples) / n
        if n > 1:
            mean = self.average
            self.variance = sum((s - mean) ** 2 for s in self.samples) / n


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


class AuthSim:
    """Mirror of src/custom_login.cpp — nonce + SHA-256 challenge.

    Credentials here are fixed and printed at startup: this simulator only ever
    binds to a developer machine, and a surprise password would just make the
    login page untestable.
    """

    NONCE_TTL_S = 30
    USERNAME = "admin"
    PASSWORD = "admin"
    ROLE_ADMIN = 2

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.salt = "0123456789abcdef"
        self.password_hash = _sha256(f"{self.salt}:{self.PASSWORD}")
        self.nonces: dict[str, float] = {}
        self.tokens: set[str] = set()

    def issue_nonce(self, username: str) -> dict:
        nonce = secrets.token_hex(16)  # 32 hex chars, as in the firmware
        now = time.time()
        with self.lock:
            self.nonces = {
                n: t for n, t in self.nonces.items() if now - t <= self.NONCE_TTL_S
            }
            self.nonces[nonce] = now
        # An unknown user still gets a stable decoy salt, so the endpoint cannot
        # be used to enumerate accounts.
        salt = self.salt if username == self.USERNAME else _sha256(
            f"sim:nosuchuser:{username}"
        )[:16]
        return {"nonce": nonce, "salt": salt, "ttlMs": self.NONCE_TTL_S * 1000}

    def login(self, username: str, nonce: str, response: str) -> str | None:
        now = time.time()
        with self.lock:
            issued = self.nonces.pop(nonce, None)  # one-shot
        if issued is None or now - issued > self.NONCE_TTL_S:
            return None
        if username != self.USERNAME:
            return None
        if response != _sha256(f"{nonce}:{self.password_hash}"):
            return None

        token = secrets.token_hex(32)  # 64 hex chars
        with self.lock:
            self.tokens.add(token)
        return token

    def set_password(self, password: str) -> None:
        with self.lock:
            self.password_hash = _sha256(f"{self.salt}:{password}")
            self.tokens.clear()  # existing sessions die with the old credential

    def logout(self, token: str) -> None:
        with self.lock:
            self.tokens.discard(token)

    def valid(self, token: str) -> bool:
        with self.lock:
            return token in self.tokens


# Mirror of g_configSecretMask in src/web.cpp. Secrets are replaced by this on
# GET and restored from the stored document when POSTed back unchanged.
CONFIG_SECRET_MASK = "********"
CONFIG_SECRET_PATHS = [
    ("wifi", "password"),
    ("ota", "password"),
    ("thingSpeak", "apiKey"),
    ("talkBack", "apiKey"),
    ("mqtt", "password"),
]

SIM_CONFIG = {
    "version": 2,
    "id": "1a2b",
    "hostname": "espgarden-sim",
    "timezone": "<-03>3",
    "wifi": {"ssid": "sim-wifi", "password": "sim-wifi-password"},
    "ota": {"username": "admin", "password": "admin"},
    "thingSpeak": {"apiKey": "SIMKEY0000000000", "channel": 1348790},
    "talkBack": {"apiKey": "SIMTALK000000000", "channel": 42661},
    "mqtt": {
        "clientID": "sim-client",
        "username": "sim-user",
        "password": "sim-password",
        "server": "mqtt3.thingspeak.com",
        "port": 8883,
        "cacert": "/thingspeak.pem",
    },
    "log": {"level": 4},
    "history": {"records": 1440, "periodSec": 60},
    "io": {
        "button": 0,
        "relays": [
            {"pin": 19, "on": 0, "name": "Watering"},
            {"pin": 16, "on": 0, "name": "Relay 2"},
            {"pin": 17, "on": 0, "name": "Relay 3"},
            {"pin": 18, "on": 0, "name": "Relay 4"},
        ],
        "dht": 23,
        "soilMoisture": [36, 34],
        "luminosity": 39,
    },
}


def config_masked() -> dict:
    doc = json.loads(json.dumps(SIM_CONFIG))  # deep copy
    for section, key in CONFIG_SECRET_PATHS:
        if key in doc.get(section, {}):
            doc[section][key] = CONFIG_SECRET_MASK
    return doc


# Mirrors ConfigFile::loadFile()'s tail. A document that violates this is
# rejected by the device at boot, so the write path must refuse it too.
CONFIG_MIN_STRING_LENGTH = 4
CONFIG_REQUIRED = [
    ("", "hostname"),
    ("wifi", "ssid"),
    ("wifi", "password"),
    ("ota", "username"),
    ("ota", "password"),
    ("thingSpeak", "apiKey"),
    ("talkBack", "apiKey"),
]


def config_apply(incoming: dict) -> tuple[int, str]:
    """Returns (http_status, message). Mirrors handleConfigPost in src/web.cpp."""
    if not isinstance(incoming, dict):
        return 400, "config is not a JSON object"
    if str(incoming.get("id", "")).lower() != str(SIM_CONFIG["id"]).lower():
        return 400, "config id does not match this device"

    new_ota_password = None
    for section, key in CONFIG_SECRET_PATHS:
        sec = incoming.get(section)
        if not isinstance(sec, dict) or key not in sec:
            continue
        if sec[key] == CONFIG_SECRET_MASK:
            stored = SIM_CONFIG.get(section, {}).get(key)
            if stored:
                sec[key] = stored
        elif (section, key) == ("ota", "password"):
            new_ota_password = sec[key]

    # A mask that survived the restore would be written over a real credential.
    for section, key in CONFIG_SECRET_PATHS:
        sec = incoming.get(section)
        if isinstance(sec, dict) and sec.get(key) == CONFIG_SECRET_MASK:
            return 500, "secret restore failed"

    for section, key in CONFIG_REQUIRED:
        node = incoming if section == "" else incoming.get(section, {})
        value = node.get(key) if isinstance(node, dict) else None
        if not isinstance(value, str) or len(value) < CONFIG_MIN_STRING_LENGTH:
            field = key if section == "" else f"{section}.{key}"
            return 400, f"'{field}' must have at least {CONFIG_MIN_STRING_LENGTH} characters"

    SIM_CONFIG.clear()
    SIM_CONFIG.update(incoming)

    # The login password lives outside the config on the device too, so a
    # changed ota.password has to reach the auth store or the simulator's login
    # stops matching the firmware's.
    if new_ota_password:
        AUTH.set_password(new_ota_password)

    return 200, "saved"

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
    "/devices.js",
    "/jquery.js",
    "/spark-md5.js",
    "/favicon.ico",
}

# Mirrors UserStore. Password hashes and salts are deliberately absent: the
# device never serves them, so neither does the simulator.
SIM_USERS = [{"username": "admin", "role": 2}]

# Same cap as g_historyMaxResponse in src/web.cpp: the device cannot render a
# larger reply without running out of DRAM, so the simulator must not either.
HISTORY_MAX_RESPONSE = 200

STATE = DeviceState()
AUTH = AuthSim()



def users_apply(params: dict) -> tuple[int, str, bool]:
    """Mirrors handleUsersPost in src/web.cpp. Returns (status, message, reauth)."""
    action = params.get("action", "")
    username = params.get("username", "").strip()
    password = params.get("password", "")
    try:
        role = int(params.get("role", 1))
    except ValueError:
        role = 0

    if not username:
        return 400, "Missing username", False
    if role not in (1, 2):
        return 400, "Invalid role", False

    index = next((i for i, u in enumerate(SIM_USERS)
                  if u["username"] == username), -1)
    admins = sum(1 for u in SIM_USERS if u["role"] == 2)

    if action == "delete":
        if index < 0:
            return 404, "User not found", False
        if SIM_USERS[index]["role"] == 2 and admins <= 1:
            return 400, "Cannot delete the last admin", False
        SIM_USERS.pop(index)
        # The device stores a user INDEX in each session, so removing an entry
        # forces every session to be dropped. Mirrored here or the simulator
        # would let a stale token keep working.
        AUTH.tokens.clear()
        return 200, f"User '{username}' deleted", True

    if action == "upsert":
        if index >= 0 and SIM_USERS[index]["role"] == 2 and role != 2 and admins <= 1:
            return 400, "Cannot demote the last admin", False
        if not password:
            if index < 0:
                return 400, "Password required for a new user", False
            SIM_USERS[index]["role"] = role
        else:
            if len(password) < CONFIG_MIN_STRING_LENGTH:
                return (400,
                        f"Password must have at least {CONFIG_MIN_STRING_LENGTH} characters",
                        False)
            if index >= 0:
                SIM_USERS[index]["role"] = role
            else:
                SIM_USERS.append({"username": username, "role": role})
            if username == AUTH.USERNAME:
                AUTH.set_password(password)
        return 200, f"User '{username}' saved with role {role}", False

    return 400, "Invalid action", False


def _ticker() -> None:
    while True:
        STATE.tick()
        time.sleep(1)


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
            self._send_json(config_masked())
        elif path == "/users.json":
            self._send_json(SIM_USERS)
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
            with STATE.lock:
                everything = list(STATE.history)
                if raw_offset.isdigit():
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
            # Pretend to accept; do nothing real.
            STATE.log("info", f"[OTA] Received {length} bytes (simulated)")
            self._send(HTTPStatus.OK, b"OK")
        else:
            self._send(HTTPStatus.NOT_FOUND, b"not found")

    # ---------- control parser ----------
    def _handle_control(self, params: dict[str, str]) -> None:
        if "relay" in params:
            try:
                index = int(params["relay"])
                ms = int(params.get("relayTime", 5000))
            except ValueError:
                index, ms = -1, 0
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
    args = parser.parse_args()

    if not DATA_DIR.is_dir():
        raise SystemExit(f"data/ not found at {DATA_DIR}")

    threading.Thread(target=_ticker, daemon=True).start()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    url = f"http://{args.host}:{args.port}/"
    print(f"ESP Garden simulator serving {DATA_DIR} on {url}")
    print(f"Login: {AuthSim.USERNAME} / {AuthSim.PASSWORD}")
    print("Endpoints: /  /nonce  /login  /logout  /data.json  /logs"
          "  /control  /updateEnable  /update")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
