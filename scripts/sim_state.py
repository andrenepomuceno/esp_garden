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

from history_archive import SegmentHistory
from sim_config import SIM_CONFIG
from sim_moisture import (moisture_models, moisture_state, probe_names,
                          resolve_scenario)

# How the simulated buffer behaves, overridable from dev_server.py's command
# line. The defaults are what the device's own config.json asks for; the knobs
# exist because the interesting behaviours - a segment rotation, a walk that
# needs paging, a hole in the record - take HOURS at a 60 s period, and a mirror
# whose failure modes are only reachable overnight is a mirror nobody exercises.
#
# CLAUDE.md records the cost of getting this wrong: the session-persistence
# entry had to be corrected because the mirror "could not reproduce the
# resurrection class of bug it exists to catch".
HISTORY_TUNING = {
    "period": None,    # seconds per record; None = config.history.periodSec
    "records": None,   # capacity; None = config.history.records
    "fill": 1.0,       # how full the buffer starts, as a fraction of capacity
    "gap": 0.0,        # seconds of missing record to punch into the seed
    "garden": False,   # seed a garden whose probes answer their own pumps
    "rotate_every": 0, # fault injection: recycle a segment every Nth read
}

# Records between waterings in the garden seed. FOUR HOURS at a 60 s period, and
# the number is set by the FIRMWARE's own windows rather than by taste: 30 min
# labelled wet plus 60 min labelled dry leaves 150 min of humid in a cycle this
# long, and six cycles in a day clears g_moistureMinEvents exactly once per
# buffer. A shorter cycle produces an EMPTY humid class and refuses on weight,
# which is a true answer to a question the seed was not asking.
GARDEN_CYCLE = 240

# Bound once: the seed's rise is first-order and math.exp is the only thing it
# needs from math that the sine-based seed did not already use.
_EXP = math.exp


def ambient_sensor_name() -> str:
    """Which air temperature/humidity part is fitted, or "" for none.

    Mirrors ConfigFile::ambientSensorName(), including the mutual exclusion:
    loadFile() clears dhtFitted when a document declares both io.dht and
    io.sht4x, so the SHT40 wins and the DHT is ignored. Presence is the key on
    both, exactly as it is for every other sensor.
    """
    io_cfg = SIM_CONFIG.get("io", {})
    if "sht4x" in io_cfg:
        return "SHT40"
    if "dht" in io_cfg:
        return "DHT11"
    return ""




# A SECOND implementation of segment::fitCapacity() in
# include/core/segment_index.h, and it is a second implementation on purpose:
# the point of the mirror is that a disagreement is visible. Nothing is
# preallocated on the device, so a capacity the partition cannot hold is
# accepted at boot, grows for days and then runs the filesystem out from inside
# append() -- the device therefore clamps at load and says so, and a simulator
# that reported the configured number would show a row the device never sends.
#
# Bytes, not records: LittleFS rounds every segment file up to a whole 4 KB
# erase block, and eight segments of slack is 32 KB.
FIT_SEGMENTS = 8
FIT_BLOCK_BYTES = 4096
FIT_HEADER_BYTES = 12   # sizeof(IoSegmentHeader)
FIT_RECORD_BYTES = 48   # sizeof(IoRecord)
FIT_RESERVE_FLOOR = 64 * 1024
FIT_RESERVE_DIVISOR = 8


def _fit_reserve_bytes(partition_bytes: int) -> int:
    return max(FIT_RESERVE_FLOOR, partition_bytes // FIT_RESERVE_DIVISOR)


def _fit_storage_bytes(capacity: int) -> int:
    """What `capacity` records occupy once every segment has filled."""
    if capacity <= 0:
        return 0
    per = -(-capacity // FIT_SEGMENTS)          # rounded UP, as the device does
    raw = FIT_HEADER_BYTES + per * FIT_RECORD_BYTES
    blocks = -(-raw // FIT_BLOCK_BYTES)
    return blocks * FIT_BLOCK_BYTES * FIT_SEGMENTS


def _fit_available_bytes(total_bytes: int, used_bytes: int,
                         history_bytes: int = 0) -> int:
    """What the history may be measured against: free space plus what the
    segments already hold.

    Mirrors `g_lastFit.availableBytes = free + onDisk` in src/io_history.cpp,
    and it is a named function rather than an expression because it is needed
    TWICE -- once to grant a capacity and once to report it in Status.History.
    Spelling it out in both places is how the two came to disagree: the row
    used a bare `total - used` and matched only because the simulator's
    `history_bytes` term happens to be 0.
    """
    return max(total_bytes - used_bytes, 0) + history_bytes


def _fit_history_capacity(requested: int, total_bytes: int,
                          used_bytes: int, history_bytes: int = 0) -> int:
    """The capacity actually granted: never more than asked, never more than
    fits. `history_bytes` is what the segments already hold, added back because
    a new capacity replaces the old rather than sitting beside it."""
    if requested <= 0 or total_bytes <= 0:
        return max(requested, 0)
    available = _fit_available_bytes(total_bytes, used_bytes, history_bytes)
    budget = available - _fit_reserve_bytes(total_bytes)
    if budget <= 0:
        return 0
    blocks = (budget // FIT_SEGMENTS) // FIT_BLOCK_BYTES
    usable = blocks * FIT_BLOCK_BYTES
    if usable <= FIT_HEADER_BYTES:
        return 0
    fits = ((usable - FIT_HEADER_BYTES) // FIT_RECORD_BYTES) * FIT_SEGMENTS
    return min(requested, fits)


class DeviceState:
    """In-memory simulation of the ESP Garden device state."""

    LOG_CAPACITY = 400

    # The partition /data.json reports, and what the fit check above measures
    # the history against. LittleFS reports the whole 512 KB partition where
    # SPIFFS reported 463 KB of usable space. 320 KB used is what a device with
    # the bundled, gzipped assets and no history sits at, so the free space the
    # simulator offers the history is the free space a real one does.
    FS_TOTAL_BYTES = 512 * 1024
    FS_USED_BYTES = 320 * 1024

    # ioHistoryBytesOnDisk() on the device: what the segments ALREADY hold, and
    # what the fit adds back because a new capacity replaces the old rather
    # than sitting beside it. Zero here, and that is a statement about
    # FS_USED_BYTES above rather than about the simulator's history: 320 KB is
    # defined as a device carrying the assets and NO history, so the free space
    # that subtraction leaves is already the post-delete figure the device
    # computes as `free + onDisk`.
    #
    # It is a named constant, not an omitted argument, because it has to be
    # passed to the same helper in both places that need it. Give the simulator
    # a used figure that DOES include its seeded history and this becomes
    # non-zero; leave one of the two call sites out of step and the row prints
    # a different number from the one the grant was made against.
    FS_HISTORY_BYTES = 0

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
        # Named for the ROLE and not for the part, mirroring
        # g_ambientReadErrors / g_ambientTotalReads: a board reads at most one
        # ambient sensor, a DHT11 or an SHT40, and the counters are the same
        # ones either way.
        self.ambient_total_reads = 0
        self.ambient_read_errors = 0
        self.logs: deque[str] = deque(maxlen=self.LOG_CAPACITY)

        # Mirrors the on-device history in src/io_history.cpp. Capacity and
        # period come from the same config block the device reads — hardcoding
        # them let the simulator report a capacity the device would never
        # return.
        #
        # IT USED TO BE A DEQUE, and that was a real drift rather than a
        # simplification. A deque with maxlen drops ONE record per append; the
        # device recycles a whole SEGMENT at once, so its retention swings
        # between 7/8 and 8/8 of capacity and every logical index — which is
        # what `?offset=` addresses — moves down by 375 in one step. The old
        # comment called that "close enough for a frontend mock", and it was,
        # right up until something had to PAGE this endpoint: an archiver that
        # trusts an offset across a rotation re-reads the newest records,
        # believes it has met known data and silently loses the segment in
        # between. A mock that cannot produce that shape cannot catch it.
        #
        # SegmentHistory lives in history_archive.py and is shared with
        # scripts/history_export.py's own tests, which means the mirror and the
        # archiver agree by construction — a wrong belief about the device's
        # rule would be invisible in both. That is stated here rather than
        # hidden: test_segment_index is what pins the FIRMWARE's arithmetic.
        # Built at the END of __init__, because the seed writes
        # `flow_total_litres` and the sensors it reads are set up below.

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

        self._build_history()
        self.log("info", "Simulator booted")

    def _build_history(self) -> None:
        """Builds the buffer from HISTORY_TUNING and reseeds it.

        Separate from __init__ because STATE is constructed at IMPORT time and
        dev_server.py's flags are parsed in main(), long afterwards.
        reconfigure_history() below is the door those flags come through.
        """
        history_cfg = SIM_CONFIG.get("history", {})
        self.history_requested = int(
            HISTORY_TUNING["records"] if HISTORY_TUNING["records"] is not None
            else history_cfg.get("records", 1440))
        granted = _fit_history_capacity(
            self.history_requested, self.FS_TOTAL_BYTES, self.FS_USED_BYTES,
            self.FS_HISTORY_BYTES)
        self.history = SegmentHistory(granted)
        # IoHistory::capacity() is segmentRecords * kSegments with
        # segmentRecords rounded UP, so a granted 1001 is reported as 1008.
        #
        # Since firmware 2.20.0 the device can also hold segments of DIFFERENT
        # sizes for a while: changing history.records no longer deletes the
        # segments written under the previous value, so capacity() is the sum
        # of the slots' own ceilings until the old ones have aged out. This
        # mirror cannot reach that state — it builds a fresh SegmentHistory on
        # every reconfigure and never re-adopts files across a capacity change,
        # which is exactly the boot the firmware now handles — so the product
        # above is the whole of what it can ever report. Said here rather than
        # papered over: a green simulator is not a statement about the C++.
        self.history_capacity = self.history.capacity
        # records = 0 means disabled on the device, where /history.json answers
        # 503. Coercing it to 1 here made that branch unreachable in the UI.
        self.history_enabled = self.history_capacity > 0
        # Floored at 1 s, and the floor is not arbitrary. The ticker runs at
        # 1 Hz so nothing below it can be delivered anyway, and a record is
        # stamped `int(time.time())` — two appends inside one second would share
        # a timestamp, which is the identity scripts/history_export.py keys on.
        # The DEVICE cannot produce that: historyTaskHandler() reschedules from
        # the end of its callback at `history.periodSec`, so a collision here
        # would be a property of this mock's stamping and would have the
        # archiver chasing a shape the firmware cannot make.
        self.history_period_s = max(1.0, float(
            HISTORY_TUNING["period"] if HISTORY_TUNING["period"] is not None
            else history_cfg.get("periodSec", 60)))
        self._seed_history()
        # One period from now, not immediately. At 0.0 the first live tick
        # appended on top of a seed that already ended at `now`, which on a full
        # buffer is an instant rotation: the simulator started 180 records short
        # of what it had just seeded, and a test asking for a full buffer got
        # 7/8 of one for reasons nothing on screen explained.
        self._history_next = time.time() + self.history_period_s
        self._history_reads = 0

    def history_read_hook(self) -> None:
        """Fault injection: recycle a segment between two /history.json reads.

        A rotation shifts every logical index down by a whole segment at once,
        and a paging client that trusts an offset it computed before one loses
        that segment WITHOUT NOTICING - it re-reads the newest records, meets
        data it already has, and stops. It is the single failure this mirror
        exists to reproduce for scripts/history_export.py.

        It cannot be reached by waiting. A real rotation lands every
        `recordsPerSegment` appends - 375 minutes on 6224 - while a walk takes
        milliseconds, so the window is about one in twenty thousand and no test
        that respects wall-clock time will ever hit it. So the mock is told to
        rotate on demand, which is what a mock is for, and the knob is off
        unless somebody asks for it.

        `--history-rotate-every 1` rotates between EVERY pair of reads, which is
        harsher than any device can be: the walk must give up and say so rather
        than loop or lie.
        """
        every = int(HISTORY_TUNING.get("rotate_every") or 0)
        if every <= 0:
            return
        self._history_reads += 1
        if self._history_reads % every:
            return
        # Fill the newest segment and append once more: the same path an
        # ordinary append takes when it finds the newest segment full.
        newest = self.history.all_records()[-1]
        room = self.history.per_segment - len(self.history._files[-1])
        for step in range(room + 1):
            self.history.append(
                {**newest, "t": int(newest["t"]) + step + 1})

    def reconfigure_history(self, **tuning) -> None:
        """Applies command-line history knobs and rebuilds the buffer."""
        for key, value in tuning.items():
            if value is not None:
                HISTORY_TUNING[key] = value
        with self.lock:
            self._build_history()

    def _history_status(self) -> str:
        """Status.History, mirroring the block in src/web_data.cpp.

        `stored` against CAPACITY and not against the configured value: the
        device drops a whole segment at a time, so the buffer touches capacity
        only in the moment before a rotation. The clamp is appended only while
        it applies, because a row that explains itself on every device trains
        the eye to skip the one that matters.
        """
        if not self.history_enabled:
            text = "disabled"
        else:
            text = f"{self.history.stored} / {self.history_capacity} records"
        if self.history_capacity < self.history_requested:
            # The same expression the grant was made with, not a second
            # subtraction that agrees with it by luck: src/web_data.cpp renders
            # fit.availableBytes, which io_history.cpp sets to free + onDisk.
            available = _fit_available_bytes(
                self.FS_TOTAL_BYTES, self.FS_USED_BYTES, self.FS_HISTORY_BYTES)
            reserve = _fit_reserve_bytes(self.FS_TOTAL_BYTES)
            text += (f" (config asked for {self.history_requested}; "
                     f"{available // 1024} KB available, "
                     f"{reserve // 1024} KB reserved)")
        return text

    def _seed_history(self) -> None:
        """Backdated records so the charts are testable immediately.

        Without this the buffer starts empty and gains one record per period:
        every path the history page adds — gap breaking, the crosshair index
        map, the relay run-length strip — is unreachable for hours, and so is
        every path an ARCHIVER adds, which needs more than 200 records before it
        pages at all.

        Three knobs, all off by default, all there because the behaviour they
        reach cannot be waited for:

          fill    a buffer that starts part-full, so the next appends ROTATE on
                  a schedule a test can predict.
          gap     a hole in the record, which is not hypothetical: the device
                  wrote nothing for 17.6 h on 2026-09-16 because
                  historyTaskHandler() refuses an unsynced clock.
          garden  probes that answer their own pumps, so the whole chain —
                  collect, archive, fit — can be exercised on a garden that
                  BEHAVES. A tool that has only ever refused has not been shown
                  able to accept.
        """
        if not self.history_enabled:
            return
        import math as _math
        now = time.time()
        fill = min(max(float(HISTORY_TUNING["fill"]), 0.0), 1.0)
        count = int(self.history_capacity * fill)
        gap_sec = float(HISTORY_TUNING["gap"])
        # The hole is punched in the MIDDLE, so the seeded series has records on
        # both sides of it. A gap at either end is indistinguishable from the
        # buffer simply starting or ending there.
        gap_at = count // 2 if gap_sec > 0 else -1
        # The live counter continues from where the seed left off. Restarting
        # it at zero made the cumulative total jump backwards at the seam, so
        # the "Water Delivered" chart showed a drop no meter can produce.
        seeded_total = 0.0
        shift = 0.0
        for i in range(count, 0, -1):
            if count - i == gap_at:
                shift = gap_sec
            t = now - i * self.history_period_s - (gap_sec - shift)
            phase = i / 30.0
            # A pump every 37 records, held for two so the mask is STICKY across
            # more than one — the rising-edge rule in collectEvents() has to
            # count that once, and a one-record pulse never tests it.
            #
            # The garden seed uses a FOUR-HOUR cycle instead, because the
            # firmware's own windows decide what a 37-minute one can produce:
            # 30 min wet plus 60 min dry leaves no room at all for HUMID, so
            # every probe fails the weight gate on an empty class and the
            # ACCEPT path stays unreachable. GARDEN_CYCLE records give six
            # cycles a day with 150 minutes of humid in each.
            if HISTORY_TUNING["garden"]:
                # TWO pumps, half a cycle apart, so relay 1 is not a spare bit
                # that never rises — a probe whose own pump never runs is the
                # case the live garden is already in, and the seed exists to
                # show the OTHER one.
                mask = (1 if (i % GARDEN_CYCLE) in (0, 1) else 0)
                offset = (i + GARDEN_CYCLE // 2) % GARDEN_CYCLE
                mask |= (2 if offset in (0, 1) else 0)
                pumping = bool(mask)
                moisture = self._garden_moisture(i)
            else:
                pumping = (i % 37 == 0) or ((i + 1) % 37 == 0)
                mask = (1 if pumping else 0) | (8 if i % 211 == 0 else 0)
                moisture = [
                    round(45 + 6 * _math.sin(phase), 2),
                    round(38 + 4 * _math.sin(phase + 1), 2),
                    round(52 + 5 * _math.sin(phase + 2), 2),
                    round(62 + 4 * _math.sin(phase + 3), 2),
                ]
            self.history.append({
                "t": int(t),
                "relays": mask,
                "moisture": moisture,
                "lum": round(max(0.0, 55 + 40 * _math.sin(i / 60.0)), 2),
                "temp": round(25 + 3 * _math.sin(phase / 2), 2),
                "hum": round(70 + 8 * _math.sin(phase / 3), 2),
                "water": round(6 + 1.5 * _math.sin(i / 90.0), 2),
                # Flow only runs while a pump does, and the total only climbs —
                # a flat line here would hide the one shape that matters.
                "flow": 2.4 if pumping else 0.0,
                "flowTotal": round(seeded_total, 3),
                "float": 1,
            })
            if pumping:
                seeded_total += 0.2
        self.flow_total_litres = seeded_total

    def _garden_moisture(self, countdown: int) -> list:
        """A probe that actually answers its pump, for the ACCEPT path.

        `countdown` runs from capacity down to 1, so a watering is at
        `countdown % GARDEN_CYCLE == 0` and the records since the last one are
        `GARDEN_CYCLE - 1 - (countdown % GARDEN_CYCLE)`. Probe 1 is the same
        curve half a cycle out of phase, on relay 1. Probes 2 and 3 are left
        FLAT, which is what an unresponsive probe looks like and is what the
        separation gate must go on refusing even here.

        THE RISE IS FIRST-ORDER, not a ramp, and that is the whole lesson of
        building this fixture. A LINEAR rise filling the firmware's 30-minute
        wet window puts the wet class at the MIDPOINT of the climb - measured
        here at 62.9 against humid's 66.3, so humid did not lie between dry and
        wet, J came out at 2.1 and moistureModelIsUsable() refused a garden that
        was behaving perfectly. Stretch the ramp to 60 records and it gets
        worse: wet 55.3 against humid 73.2.

        That is not the seed being unlucky. labelFor() calls the whole 30
        minutes after a pump "wet", and that is only true of soil that is NEAR
        SATURATION for most of it - which is what m(t) = baseline + rise *
        (1 - e^-t/tau) does and what a straight line cannot. It is the same
        assumption moistureTimeConstant() exists to measure and
        moistureAbsorptionConfidence() exists to discount, and on this device
        tau is 0 for every probe, so the 5-minute linear stand-in is what runs.

        tau = 8 records reaches 95 % of the rise in 24 minutes, inside the
        window. The first records of each watering do exceed the rate
        STEP_MIN_POINTS asserts soil cannot make - which is correct, a pump IS
        an event - and they are excluded as steps EXPLAINED by this probe's own
        pump, which is the mechanism that exists for exactly this.
        """
        tau = 8.0
        span = 25.0
        fall = GARDEN_CYCLE - 30

        def curve(since):
            if since < 30:
                return 50.0 + span * (1.0 - _EXP(-(since + 1) / tau))
            return 50.0 + span - span * min(1.0, (since - 30) / fall)

        first = curve(GARDEN_CYCLE - 1 - (countdown % GARDEN_CYCLE))
        second = curve(
            GARDEN_CYCLE - 1
            - ((countdown + GARDEN_CYCLE // 2) % GARDEN_CYCLE))
        return [round(first, 2), round(second - 5.0, 2), 52.0, 62.0]

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
            # Random ambient-sensor reads
            self.ambient_total_reads += 1
            if random.random() < 0.02:
                self.ambient_read_errors += 1

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
                "Filesystem": (f"{self.FS_USED_BYTES // 1024} / "
                               f"{self.FS_TOTAL_BYTES // 1024} KB"),
                "History": self._history_status(),
            }
            # ET0 is present only once a complete day has closed, exactly as
            # web_data.cpp gates it -- and only when et0.enabled is on. The mock
            # reports a fixed plausible day so the row can be styled at all; the
            # device computes it from its own extremes.
            if SIM_CONFIG.get("et0", {}).get("enabled"):
                status["ET0"] = "4.21 mm/day (range 11.3 K)"
            # Which part produced Temperature and Air Humidity, and how often
            # it failed. The LABEL is "Ambient ..." because an SHT40 board has
            # no DHT; the ThingsBoard TELEMETRY key stays `dhtErrorRate`,
            # which has stored history behind it.
            ambient = ambient_sensor_name()
            if ambient:
                status["Ambient Sensor"] = ambient
            if self.ambient_total_reads:
                rate = self.ambient_read_errors / self.ambient_total_reads * 100
                status["Ambient Error Rate"] = f"{rate:.2f}"

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
            # One pair of rows for either part. loadFile() clears dhtFitted
            # when a document declares both, so they are never both fitted.
            if ambient_sensor_name():
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
            # src/web_data.cpp, whose comment carries the reason the key is
            # still emitted at all. No page in data/ reads it -- the ThingSpeak
            # link that once did was removed -- so what a simulator that always
            # sent it would break is not the dashboard but the mirror: the
            # contract this file exists to reproduce is the one the firmware
            # sends, present-or-absent included.
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
