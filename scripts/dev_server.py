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
import json
import math
import os
import random
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
        self.watering_state = 0
        self.watering_until = 0.0
        self.logs: deque[str] = deque(maxlen=self.LOG_CAPACITY)

        # Sensor accumulators: rolling state to fake sensible averages/variance.
        self._sensors = {
            "Soil Moisture": _Sensor(45.0, amplitude=8.0, period=900, noise=1.5),
            "Luminosity":    _Sensor(55.0, amplitude=35.0, period=120, noise=4.0),
            "Temperature":   _Sensor(25.5, amplitude=3.0, period=1800, noise=0.4),
            "Air Humidity":  _Sensor(70.0, amplitude=8.0, period=2400, noise=1.5),
        }

        self.log("info", "Simulator booted")

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
    def start_watering(self, duration_ms: int) -> None:
        duration_ms = max(100, min(60_000, duration_ms))
        with self.lock:
            self.watering_state = 1
            self.watering_until = time.time() + duration_ms / 1000.0
            self.watering_cycles += 1
        self.log("info", f"Watering started for {duration_ms} ms")

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
            self.watering_state = 0
            self.watering_until = 0.0

    # ----- snapshot -----
    def tick(self) -> None:
        """Advance simulated state once per second."""
        now = time.time()
        with self.lock:
            if self.watering_state and now >= self.watering_until:
                self.watering_state = 0
                self.log("info", "Watering finished")
            for sensor in self._sensors.values():
                sensor.update()
            # Mqtt publishes a "package" every 30s when enabled.
            if self.mqtt_enabled and int(now - self.boot_time) % 30 == 0:
                self.packages_sent += 1
            # Random DHT reads
            self.dht_total_reads += 1
            if random.random() < 0.02:
                self.dht_read_errors += 1

    def snapshot(self) -> dict:
        with self.lock:
            uptime = int(time.time() - self.boot_time)
            days, rem = divmod(uptime, 86400)
            hours, rem = divmod(rem, 3600)
            minutes, seconds = divmod(rem, 60)

            status = {
                "Hostname": self.hostname,
                "Date/Time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "Uptime": f"{days}d {hours}h {minutes}m {seconds}s",
                "Internet": "online",
                "Signal Strength": f"{random.randint(70, 95)}%",
                "Ping": f"{random.randint(15, 60)}ms",
                "Connection Loss Count": str(self.connection_loss_count),
                "MQTT": "enabled" if self.mqtt_enabled else "disabled",
                "Packages Sent": str(self.packages_sent),
                "Watering Cycles": str(self.watering_cycles),
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

            return {
                "Status": status,
                "Inputs": inputs,
                "Outputs": {"Watering": str(self.watering_state)},
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


STATE = DeviceState()


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
    def do_GET(self) -> None:  # noqa: N802 - stdlib signature
        url = urlparse(self.path)
        path = url.path

        if path == "/data.json":
            self._send_json(STATE.snapshot())
        elif path == "/logs":
            body = STATE.logs_text().encode("utf-8")
            self._send(HTTPStatus.OK, body, "text/plain; charset=utf-8")
        else:
            self._serve_static(path)

    def do_POST(self) -> None:  # noqa: N802
        url = urlparse(self.path)
        path = url.path
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""

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
    print("Endpoints: /  /data.json  /logs  /control  /updateEnable  /update")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
