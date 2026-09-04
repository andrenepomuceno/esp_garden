"""The simulated device itself — sensors, relays, the log ring, the I/O history
buffer and the once-a-second tick that advances them.

Mirrors src/tasks.cpp and src/io_history.cpp, and builds the payload
src/web_data.cpp builds. Split out of dev_server.py, which crossed the
1000-line limit scripts/check_lines.py enforces.
"""

from __future__ import annotations

import math
import random
import threading
import time
from collections import deque
from datetime import datetime

from sim_config import SIM_CONFIG
from sim_moisture import (moisture_models, moisture_state, probe_names,
                          resolve_scenario)


class DeviceState:
    """In-memory simulation of the ESP Garden device state."""

    LOG_CAPACITY = 400

    # Mirrors USE_THINGSPEAK in include/BuildConfig.h, which ships at 0.
    #
    # The simulator is a second implementation of the device's HTTP contract,
    # and that contract now depends on a BUILD flag: with ThingSpeak compiled
    # out the firmware omits the "Channel" key from /data.json entirely rather
    # than sending 0 or "". Flip this to True to mirror a firmware rebuilt with
    # the flag on; leaving it out of step is exactly the silent drift this
    # mirror exists to prevent.
    USE_THINGSPEAK = False

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.boot_time = time.time()
        self.hostname = "espgarden-sim"
        self.channel = "1348790"
        self.mqtt_enabled = True
        # Bumped by a simulated firmware upload. /update.html judges the
        # outcome by what the DEVICE reports afterwards, not by the upload's
        # return value, so a mock that never changes version would leave the
        # page waiting on something that cannot happen.
        self.firmware = "2.0.0"
        self.packages_sent = 0
        self.last_publish = 0  # epoch of the last accepted publish, 0 = never
        self.watering_cycles = 0
        self.connection_loss_count = 0
        self.dht_total_reads = 0
        self.dht_read_errors = 0
        self.logs: deque[str] = deque(maxlen=self.LOG_CAPACITY)

        # Mirrors the on-device history in src/io_history.cpp. The device
        # now drops a whole SEGMENT at a time rather than one record, so its
        # retention swings between 7/8 and 8/8 of capacity; a deque with
        # maxlen drops one at a time, which is close enough for a frontend
        # mock and is noted here so nobody reads it as the device's rule.
        # The simulator gets the same drop-oldest
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
            "Soil Moisture 4": _Sensor(62.0, amplitude=5.0, period=1500, noise=1.5),
            "Luminosity":      _Sensor(55.0, amplitude=35.0, period=120, noise=4.0),
            "Temperature":     _Sensor(25.5, amplitude=3.0, period=1800, noise=0.4),
            "Air Humidity":    _Sensor(70.0, amplitude=8.0, period=2400, noise=1.5),
            "Water Level":     _Sensor(6.0, amplitude=2.0, period=3600, noise=0.2),
            "Flow":            _Sensor(0.0, amplitude=0.0, period=60, noise=0.0),
        }
        # Cumulative litres and the reservoir float. Neither fits the
        # val/avg/var accumulator shape, so both are built by hand in
        # snapshot() exactly as src/web_data.cpp builds them.
        self.flow_total_litres = 0.0
        self.float_raised = True

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
        # Fill the whole buffer. 400 records is 6.7 h at the default period, so
        # the 12 h / 1 d / 7 d / 30 d window buttons all collapsed onto the same
        # data and decimation never engaged — the one behaviour the window
        # selector exists to exercise.
        count = self.history_capacity
        # The live counter continues from where the seed left off. Restarting
        # it at zero made the cumulative total jump backwards at the seam, so
        # the "Water Delivered" chart showed a drop no meter can produce.
        seeded_total = 0.0
        for i in range(count, 0, -1):
            t = now - i * self.history_period_s
            phase = i / 30.0
            self.history.append({
                "t": int(t),
                "relays": (1 if i % 37 == 0 else 0) | (8 if i % 211 == 0 else 0),
                "moisture": [
                    round(45 + 6 * _math.sin(phase), 2),
                    round(38 + 4 * _math.sin(phase + 1), 2),
                    round(52 + 5 * _math.sin(phase + 2), 2),
                    round(62 + 4 * _math.sin(phase + 3), 2),
                ],
                "lum": round(max(0.0, 55 + 40 * _math.sin(i / 60.0)), 2),
                "temp": round(25 + 3 * _math.sin(phase / 2), 2),
                "hum": round(70 + 8 * _math.sin(phase / 3), 2),
                "water": round(6 + 1.5 * _math.sin(i / 90.0), 2),
                # Flow only runs while a pump does, and the total only climbs —
                # a flat line here would hide the one shape that matters.
                "flow": 2.4 if i % 37 == 0 else 0.0,
                "flowTotal": round(seeded_total, 3),
                "float": 1,
            })
            if i % 37 == 0:
                seeded_total += 0.4
        self.flow_total_litres = seeded_total

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
        # g_relayMaxTime in src/relays.cpp, raised from 20 s in 545643a. A stale
        # ceiling here refuses activations the device accepts, so UI work on the
        # watering-time field is validated against a limit that does not exist.
        if duration_ms <= 0 or duration_ms > 30_000:
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

    def stop_relay(self, index: int) -> None:
        """Mirrors stopRelay() in src/relays.cpp, which returns wasRunning.

        Stopping an idle relay is deliberately not an error: the dashboard
        polls at 1 Hz, so a stop can always arrive a tick after the relay
        expired on its own, and refusing it would make a harmless race look
        like a fault.
        """
        if not 0 <= index < len(self.relays):
            self.log("error", f"Invalid relay index: {index}")
            return
        # self.log() takes the same non-reentrant lock; keep it outside.
        with self.lock:
            was_running = bool(self.relays[index]["on"])
            self.relays[index]["on"] = 0
            self.relays[index]["until"] = 0.0

        if was_running:
            self.log("info", f"Stopping {self.relay_names[index]}")
        else:
            self.log("info", f"{self.relay_names[index]} was already idle.")

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
            self.last_publish = 0
            self.watering_cycles = 0
            self.connection_loss_count = 0
            for relay in self.relays:
                relay["on"] = 0
                relay["until"] = 0.0

    # ----- sensors -----
    def moisture_sensor(self, index: int):
        """The accumulator behind probe `index`, or None when the simulator has
        no sensor for it. Probes are addressed by INDEX here, never by label:
        the label is configurable and the firmware keys g_soilMoisture[] by
        index for exactly the same reason.
        """
        return self._sensors.get(f"Soil Moisture {index + 1}")

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

            # A flow meter reads zero unless something is pumping. Faking a
            # constant trickle would hide the one thing the sensor is for.
            pumping = any(relay["on"] for relay in self.relays)
            self._sensors["Flow"].baseline = 2.4 if pumping else 0.0
            if pumping:
                self.flow_total_litres += 2.4 / 60.0
            # Mqtt publishes a "package" every 30s when enabled.
            if self.mqtt_enabled and int(now - self.boot_time) % 30 == 0:
                self.packages_sent += 1
                self.last_publish = int(time.time())
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
                        round(self._sensors["Soil Moisture 4"].average, 2),
                    ],
                    "lum": round(self._sensors["Luminosity"].average, 2),
                    "temp": round(self._sensors["Temperature"].average, 2),
                    "hum": round(self._sensors["Air Humidity"].average, 2),
                    "water": round(self._sensors["Water Level"].average, 2),
                    "flow": round(self._sensors["Flow"].average, 2),
                    "flowTotal": round(self.flow_total_litres, 3),
                    # Three states: null is "no float switch fitted", which the
                    # device distinguishes with IO_HISTORY_FLAG_FLOAT_VALID.
                    "float": 1 if self.float_raised else 0,
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
                "Firmware": self.firmware,
                "Date/Time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "Uptime": f"{days}d {hours}h {minutes}m {seconds}s",
                "Internet": "online",
                "Signal Strength": f"{random.randint(70, 95)}%",
                "Ping": f"{random.randint(15, 60)}ms",
                "Connection Loss Count": str(self.connection_loss_count),
                "MQTT": "enabled" if self.mqtt_enabled else "disabled",
                # Two different questions on the device: "MQTT" is the
                # operator's switch, "MQTT Link" is whether the broker is
                # actually reachable. The simulator has no broker, so the link
                # simply follows the switch -- but the KEY has to be here, or
                # index.js is styled against a payload the device does not send.
                #
                # DECLARED DRIFT: the firmware has a third value here,
                # "unsupported backend '<x>' -- not in this build", produced
                # when config.json selects a backend the image was not built
                # with. It has no mirror because the simulator has neither build
                # flags nor a backend to mismatch. Recorded rather than fixed
                # quietly; index.js renders the row as free text either way.
                "MQTT Link": "connected" if self.mqtt_enabled else "disabled",
                "Last Publish": (f"{int(time.time()) - self.last_publish}s ago"
                                 if self.mqtt_enabled and self.last_publish
                                 else ("never" if self.mqtt_enabled else "n/a")),
                "Packages Sent": str(self.packages_sent),
                "Watering Cycles": str(self.watering_cycles),
                # LittleFS reports the whole 512 KB partition where SPIFFS
                # reported 463 KB of usable space.
                "Filesystem": "320 / 512 KB",
            }
            # ET0 is present only once a complete day has closed, exactly as
            # web_data.cpp gates it -- and only when et0.enabled is on. The mock
            # reports a fixed plausible day so the row can be styled at all; the
            # device computes it from its own extremes.
            if SIM_CONFIG.get("et0", {}).get("enabled"):
                status["ET0"] = "4.21 mm/day (range 11.3 K)"
            if self.dht_total_reads:
                rate = self.dht_read_errors / self.dht_total_reads * 100
                status["DHT Error Rate"] = f"{rate:.2f}"

            # Only FITTED sensors, decided the way the device decides it: by
            # which keys exist in the config's io block. Without this the
            # simulator cannot reproduce a board with no DHT or no probes,
            # which is exactly what /devices.html is for editing.
            io_cfg = SIM_CONFIG.get("io", {})
            # Probe labels come from the config, exactly as web_data.cpp takes
            # them from config.soilMoistureName[] — the pre-2.0 scalar shape and
            # the single-probe label included. Hardcoding "Soil Moisture N" here
            # made the simulator disagree with the device the moment anyone
            # named a probe in /devices.html.
            probe_labels = probe_names()
            fitted = []
            if "luminosity" in io_cfg:
                fitted.append("Luminosity")
            if "dht" in io_cfg:
                fitted += ["Temperature", "Air Humidity"]
            if "waterLevel" in io_cfg:
                fitted.append("Water Level")

            # Moisture first, then the rest in _sensors order, which is the
            # order web_data.cpp writes them in.
            inputs = {}
            models = moisture_models(resolve_scenario())
            for i, label in enumerate(probe_labels):
                sensor = self.moisture_sensor(i)
                if sensor is None:
                    continue
                entry = {
                    "val": f"{sensor.value:.2f}",
                    "avg": f"{sensor.average:.2f}",
                    "var": f"{sensor.variance:.4f}",
                }
                # Carried only when non-empty, exactly as web_data.cpp gates it:
                # an uncalibrated probe with no model shows no badge at all.
                state = moisture_state(i, sensor.average, models)
                if state:
                    entry["state"] = state
                # Same gate as the device: sent only when there IS a fault, so
                # the badge's presence is the whole message. Probe 1 is the
                # unplugged one in this mock and probe 2 the wildly noisy one,
                # which is what the dashboard has to be styled against.
                fault = {1: "floating", 2: "noisy"}.get(i)
                if fault:
                    entry["fault"] = fault
                inputs[label] = entry
            for name, s in self._sensors.items():
                if name not in fitted:
                    continue
                inputs[name] = {
                    "val": f"{s.value:.2f}",
                    "avg": f"{s.average:.2f}",
                    "var": f"{s.variance:.4f}",
                }
                # The sky state rides the luminosity entry, gated exactly as
                # web_data.cpp gates it: present only when cloud.enabled is on
                # AND the model has an answer. The mock cycles the three states
                # off the same clock the sensor uses, because a dashboard badge
                # that only ever renders one value is a badge nobody styled.
                if name == "Luminosity" and SIM_CONFIG.get("cloud", {}).get(
                    "enabled"
                ):
                    inputs[name]["state"] = ["clear", "partly cloudy", "overcast"][
                        int(time.time() / 30) % 3
                    ]
            if "flow" in io_cfg:
                inputs["Flow"] = {
                    "val": f"{self._sensors['Flow'].value:.2f}",
                    "avg": f"{self._sensors['Flow'].average:.2f}",
                    "var": f"{self._sensors['Flow'].variance:.4f}",
                }
            # A running total has no window, so it does not fit val/avg/var.
            if "flow" in io_cfg:
              inputs["Flow Total"] = {
                "val": f"{self.flow_total_litres:.3f}",
                "avg": f"{self.flow_total_litres:.3f}",
                "var": "0",
            }
            # Binary, and the only input outside moisture that carries a state
            # badge — which is unreachable off-hardware without this entry.
            if "floatSwitch" in io_cfg:
              inputs["Float Switch"] = {
                "val": "1" if self.float_raised else "0",
                "avg": "1" if self.float_raised else "0",
                "var": "0",
                "state": "Raised" if self.float_raised else "Lowered",
            }

            relay_cfg = io_cfg.get("relays")
            if not isinstance(relay_cfg, list):
                # Legacy io.watering scalar: exactly one relay, as loadRelays().
                relay_cfg = ([{"name": "Watering"}]
                             if "watering" in io_cfg else [])
            names = [r.get("name", f"Relay {i + 1}")
                     for i, r in enumerate(relay_cfg)]
            outputs = {
                name: str(self.relays[i]["on"])
                for i, name in enumerate(names) if i < len(self.relays)
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
                for i, name in enumerate(names) if i < len(self.relays)
            ]

            payload = {
                "Status": status,
                "Inputs": inputs,
                "Outputs": outputs,
                "Relays": relays,
            }
            # Absent, not empty: see USE_THINGSPEAK above and the same #if in
            # src/web_data.cpp. index.js hides the ThingSpeak link when the key
            # is missing, so a simulator that always sent it would show a link
            # the real device does not.
            if self.USE_THINGSPEAK:
                payload["Channel"] = self.channel
            return payload


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
