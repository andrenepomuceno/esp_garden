#!/usr/bin/env python3
"""Fits the soil-moisture parameters on the workstation, from the full archive.

    python scripts/moisture_fit.py                       # fit, report, propose
    python scripts/moisture_fit.py --out proposal.json   # and write the JSON
    python scripts/moisture_fit.py --config data/config.json   # no device at all
    python scripts/moisture_fit.py --self-test           # the pure logic only

WHY THE FIT MOVED OFF THE DEVICE

The board holds 24 h of history in 512 KB of flash and trains once a day inside
a cooperative background task. The archive here holds every point the device has
ever published, and this machine has no memory limit and no watchdog. So the
expensive half - deciding what the parameters ARE - belongs here, and the device
keeps the cheap half it is good at: folding today into what it already knows.

WHAT IS SHIPPED, AND WHAT IS DELIBERATELY NOT

  SHIPPED      moisture[i].dry / .wet, through POST /config.json.
  NOT SHIPPED  /moisture_model.bin, the fitted Gaussians. See the block comment
               above `MODEL_FILE_DECLINED` for the five reasons, and for what
               would have to change first.

A seed is a starting point and never a freeze: `moistureModelTrain()` decays the
stored sufficient statistics by g_moistureDecayPerRun (0.93, a ten-day
half-life) and folds the new day into them, so anything pushed from here ages
out on its own as the device gathers its own evidence.

WHAT THIS TOOL REFUSES, AND WHY REFUSING IS THE POINT

CLAUDE.md records the device deliberately showing NO badge rather than a
fabricated one: `moistureState()` returns an empty string while dry == wet. A
calibration invented from a drifting anchor would replace that honest silence
with a confident wrong band, which is strictly worse than nothing. So every
number here has to clear an admission check, and the tool names the check that
refused it - the same contract /moisture.json keeps with `blockedBy`.

The four refusals, in the order they are applied:

  1. IDENTITY.  A slot in `io.soilMoisture` is not a probe. The archive keys its
     series positionally (`moisture1`..`moisture4`) and its relays positionally
     too (`relay1`..`relay4`), so deleting a sensor renumbers everything after
     it and the same key silently starts describing a different pot. The
     (`relay`, `relayName`) event pair is the one place the archive states that
     binding outright, and it is what the seam detector reads.
  2. HANDLING.  A probe that has just been moved, re-seated or watered by hand
     is equilibrating: a sustained drift with a small residual spread. Precise
     and wrong. Those samples cannot anchor anything.
  3. DISCONTINUITY.  An unexplained step - a jump no watering of this probe's
     own pump accounts for - means the probe or the pot changed. Evidence from
     before it describes a different configuration, exactly as `sourceTag` in
     MoistureProbeModel exists to say.
  4. THE GATES.  The same four the firmware applies: six watering events,
     20 accumulated weight per class, Fisher's J >= 4, and dry/humid/wet
     ordered. Reimplemented here rather than approximated, so a fit refused
     here would have been refused there.

The statistics live in scripts/moisture_stats.py beside this; what stays here
is the device, the archive and the report. Standard library only, same as
tb_export.py and cloud_fit.py next door. Reads backups/telemetry.sqlite
read-only and GETs the device's config; it writes to the device only under
--push, which is off by default and has never been run.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import sqlite3
import sys
import urllib.parse
import urllib.request
from pathlib import Path

# Plain sibling module: this file is run as a script, so scripts/ is sys.path[0]
# — the same arrangement dev_server.py has with its sim_* siblings. The split is
# statistics on one side and the device, the archive and the report on the
# other, so the half with the arithmetic in it can be exercised on its own.
from moisture_stats import (CLASSES, MAX_GAP_SEC, MIN_SEPARATION,
                            analyse_probe, build_proposal, local, self_test)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_DEVICE = "192.168.1.55"
DEFAULT_CREDENTIALS = ROOT / "data" / "config.json"
DEFAULT_TZ_HOURS = -3


# ---------------------------------------------------------------------------
# WHY /moisture_model.bin IS NOT WRITTEN FROM HERE
# ---------------------------------------------------------------------------
#
# Either path was allowed. This one is declined, and these are the reasons
# rather than a shrug:
#
#   1. There is nothing to seed. Both probes fail the separation gate by two
#      orders of magnitude, so the file would carry statistics the device would
#      correctly refuse to use. Shipping a refused model buys nothing and costs
#      a reboot.
#   2. The variances are not on the same scale. The device fits from its own
#      60 s history records; this fits from ThingsBoard's `moistureN`, which is
#      the accumulator MEAN over one publish period - 300 s since firmware
#      2.8.0. Averaging shrinks variance, and J = (mu_wet - mu_dry)^2 /
#      (var_wet + var_dry) is a RATIO to it. A model fitted here would pass or
#      fail the device's gate for the wrong reason, in the optimistic direction.
#      Fixing it means fitting from GET /history.json instead, which IS the
#      device's own 60 s record - the right shape for a seed, and the first
#      thing to change when there is finally something to seed.
#   3. `consumedUntil` has no correct value from another data source. It is the
#      epoch of the newest watering event already folded in, and the device
#      skips its own history at or before it. Zero makes the device
#      double-count the day it already has; a current epoch throws that day
#      away. Either is invisible until a gate flips.
#   4. The struct layout would have to be verified byte-for-byte against a file
#      the device wrote. `sizeof(MoistureModelState)` is checked on load, and
#      the MOI2 magic exists precisely because two fields once fitted inside
#      padding and the size check did not notice. That verification needs a read
#      of /spiffs/moisture_model.bin, which this task's constraints exclude.
#   5. A push has to be upload-then-reboot. The device loads the model at boot
#      and saves its own at each training run, so an upload into a running
#      device is overwritten by the in-RAM model at the next save. The reboot is
#      the expensive half: every reset floats the GPIOs and these relays are
#      active-low, so it pulses every pump.
#
# Reasons 2 and 3 are structural and would survive the data getting better; 1
# and 4 are circumstantial. None of them is a claim that the path cannot work.
MODEL_FILE_DECLINED = True


# ---------------------------------------------------------------------------
# The archive
# ---------------------------------------------------------------------------


def open_archive(path):
    if not path.exists():
        raise SystemExit(f"no archive at {path}; run scripts/tb_export.py first")
    return sqlite3.connect(f"file:{path.as_posix()}?mode=ro", uri=True)


def read_series(conn, key):
    rows = conn.execute(
        "SELECT ts, value FROM telemetry WHERE key = ? ORDER BY ts", (key,)
    )
    samples = []
    for timestamp, value in rows:
        try:
            samples.append((timestamp / 1000.0, float(value)))
        except (TypeError, ValueError):
            continue
    return samples


def read_relay_events(conn):
    """Every relay transition, carrying the NAME the device published with it.

    The name is the only stable identity in the archive. `relay` is an index
    into io.relays and is renumbered by any deletion - measured here: the
    reservoir pump published as index 3 until 2026-09-02 13:41 local and as
    index 2 afterwards, with `relay3Event` therefore meaning two different pumps
    on either side of that minute.
    """
    names = dict(
        conn.execute("SELECT ts, value FROM telemetry WHERE key = 'relayName'")
    )
    indices = dict(
        conn.execute("SELECT ts, value FROM telemetry WHERE key = 'relay'")
    )
    durations = dict(
        conn.execute("SELECT ts, value FROM telemetry WHERE key = 'durationMs'")
    )
    events = []
    for timestamp, name in sorted(names.items()):
        index = indices.get(timestamp)
        events.append(
            {
                "at": timestamp / 1000.0,
                "name": name,
                "index": int(index) if index is not None else None,
                # A start carries the duration it was asked for; a stop does not.
                "started": timestamp in durations,
            }
        )
    return events


def relay_seams(events):
    """Every moment the archive's relay index -> name binding changed.

    This is the identity seam. It is derived rather than configured because the
    tool has to survive the next one, and the next one will not be announced.
    """
    bound = {}
    seams = []
    for event in events:
        if event["index"] is None:
            continue
        previous = bound.get(event["index"])
        if previous is not None and previous != event["name"]:
            seams.append(
                {
                    "at": event["at"],
                    "index": event["index"],
                    "was": previous,
                    "now": event["name"],
                }
            )
        bound[event["index"]] = event["name"]
    return seams


def probe_slot_seams(conn, slots):
    """Slots whose series stopped being published - a probe was deleted."""
    seams = []
    newest = conn.execute("SELECT MAX(ts) FROM telemetry").fetchone()[0] or 0
    for slot in range(slots, 4):
        row = conn.execute(
            "SELECT MAX(ts) FROM telemetry WHERE key = ?", (f"moisture{slot + 1}",)
        ).fetchone()
        if row[0] is None:
            continue
        # Only a series that stopped well before the newest point in the archive
        # is a deletion rather than the tail of a live one.
        if newest - row[0] > MAX_GAP_SEC * 1000:
            seams.append({"at": row[0] / 1000.0, "key": f"moisture{slot + 1}"})
    return seams


# ---------------------------------------------------------------------------
# The device, read-only unless --push
# ---------------------------------------------------------------------------


class Device:
    """Nonce + SHA-256 login, exactly as src/custom_login.cpp implements it.

    ONE login and ONE request per run, and a logout at the end. The board serves
    HTTP from a single async_tcp task and keeps four session slots, so a tool
    that polls it competes with the browser and evicts whoever is using it.
    """

    def __init__(self, host, timeout=15.0):
        self.base = f"http://{host}"
        self.timeout = timeout
        self.token = None

    def _request(self, path, data=None):
        request = urllib.request.Request(self.base + path, data=data)
        if self.token:
            request.add_header("Authorization-Token", self.token)
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            return response.read().decode("utf-8")

    def login(self, username, password):
        challenge = json.loads(
            self._request("/nonce?username=" + urllib.parse.quote(username))
        )
        password_hash = hashlib.sha256(
            f"{challenge['salt']}:{password}".encode()
        ).hexdigest()
        answer = hashlib.sha256(
            f"{challenge['nonce']}:{password_hash}".encode()
        ).hexdigest()
        body = urllib.parse.urlencode(
            {
                "username": username,
                "nonce": challenge["nonce"],
                "response": answer,
            }
        ).encode()
        session = json.loads(self._request("/login", body))
        self.token = session["token"]
        return session

    def config(self):
        return json.loads(self._request("/config.json"))

    def post_config(self, document):
        """UNEXERCISED. Only reachable under --push; see push_proposal().

        The document goes in a FORM FIELD called `config`, not as a raw JSON
        body: `handleConfigPost` reads `request->getParam("config", true)` and
        answers 400 to anything else. `data/config.js`, `devices.js` and
        `schedules.js` all post it the same way. Read out of src/web_config.cpp
        rather than guessed, since nothing here has ever exercised it.
        """
        body = urllib.parse.urlencode(
            {"config": json.dumps(document, separators=(",", ":"))}
        ).encode()
        request = urllib.request.Request(
            self.base + "/config.json", data=body, method="POST"
        )
        request.add_header("Content-Type", "application/x-www-form-urlencoded")
        request.add_header("Authorization-Token", self.token)
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            return response.read().decode("utf-8")

    def logout(self):
        if not self.token:
            return
        try:
            self._request("/logout", b"")
        except OSError:
            pass
        self.token = None


def load_config(args):
    """The device's live config, or a saved document when --config is given."""
    if args.config:
        return json.loads(Path(args.config).read_text(encoding="utf-8")), None
    credentials = json.loads(
        Path(args.credentials).read_text(encoding="utf-8")
    ).get("ota", {})
    device = Device(args.device)
    device.login(credentials.get("username", ""), credentials.get("password", ""))
    document = device.config()
    return document, device


# ---------------------------------------------------------------------------
# Per-probe analysis
# ---------------------------------------------------------------------------


def probe_identity(document, index):
    sensors = document.get("io", {}).get("soilMoisture", [])
    entry = sensors[index] if index < len(sensors) else {}
    if not isinstance(entry, dict):
        entry = {"pin": entry}
    calibration = document.get("moisture", [])
    tuning = calibration[index] if index < len(calibration) else {}
    return {
        "index": index,
        "key": f"moisture{index + 1}",
        "name": entry.get("name", f"probe {index}"),
        "pin": entry.get("pin"),
        "relay": tuning.get("relay", -1),
        "invert": bool(tuning.get("invert", True)),
        "dry": float(tuning.get("dry", 0.0)),
        "wet": float(tuning.get("wet", 0.0)),
    }


def usable_from(identity, document, seams, slot_seams):
    """The earliest epoch this slot's series can be trusted to be THIS probe.

    One rule: the boundary is the MOST RECENT archive seam of any kind. A relay
    that changed name under a fixed index, or a `moistureN` series that stopped
    publishing, is evidence that config.json's `io` block was edited - and an
    edit to the io block is exactly the event MoistureProbeModel::sourceTag
    exists to detect, for exactly this reason: the slot survives, the sensor in
    it may not.

    IT IS DELIBERATELY BLUNT, and applies to every slot rather than the ones the
    archive can prove were renumbered. Measured here: on 2026-09-02 13:41 the
    reservoir pump went from index 3 to index 2 and `moisture3` stopped
    publishing, so a relay and a probe were deleted in the same edit. The
    archive states which relay INDEX changed name, but nothing anywhere says
    which PROBE slot went - the moisture keys carry no name - so `moisture2`
    before that minute is the pot Zona 2 waters and after it the pot Zona 3
    waters, with nothing in the series to mark the change.

    The tempting shortcut - trust a slot whose series crosses the seam without a
    step - was rejected. Two similar pots produce two similar readings, so that
    test admits evidence on the strength of a coincidence, and it admits it in
    the direction that fabricates a parameter rather than the direction that
    refuses one. --since can narrow this window; nothing on the command line can
    widen it.
    """
    reasons = []
    boundary = 0.0
    relays = document.get("io", {}).get("relays", [])
    relay_index = identity["relay"]
    current_name = None
    if 0 <= relay_index < len(relays):
        current_name = relays[relay_index].get("name")

    for seam in seams:
        if seam["at"] > boundary:
            boundary = seam["at"]
            reasons = [
                f"relay{seam['index'] + 1} was renamed {seam['was']!r} -> "
                f"{seam['now']!r}, so io.relays was edited"
            ]
    for seam in slot_seams:
        if seam["at"] > boundary:
            boundary = seam["at"]
            reasons = [
                f"{seam['key']} stopped publishing, so io.soilMoisture was "
                "edited and every slot may have been renumbered"
            ]

    return boundary, current_name, reasons


# ---------------------------------------------------------------------------
# Report and proposal
# ---------------------------------------------------------------------------


def report(document, findings, seams, slot_seams, tz_hours, out):
    def line(text=""):
        print(text, file=out)

    line(f"device {document.get('id')}   probes {len(findings)}")
    line()
    line("ARCHIVE SEAMS - where a positional key changed meaning")
    if not seams and not slot_seams:
        line("  none: every relay kept its name and every series kept publishing")
    for seam in seams:
        line(
            f"  {local(seam['at'], tz_hours)}  relay{seam['index'] + 1} "
            f"{seam['was']!r} -> {seam['now']!r}"
        )
    for seam in slot_seams:
        line(f"  {local(seam['at'], tz_hours)}  {seam['key']} stopped publishing")
    line()

    for finding in findings:
        identity = finding["identity"]
        line(
            f"--- probe {identity['index']}  {identity['name']}  "
            f"pin {identity['pin']}  relay {identity['relay']} "
            f"({finding.get('relayName')})  invert {identity['invert']}"
        )
        if finding.get("windowReasons"):
            for reason in finding["windowReasons"]:
                line(f"    window starts at the seam: {reason}")
        if finding.get("from"):
            line(
                f"    {finding['samples']} samples  "
                f"{local(finding['from'], tz_hours)} .. "
                f"{local(finding['to'], tz_hours)}"
            )
        else:
            line(f"    {finding['samples']} samples")

        drifts = finding.get("drifts", [])
        line(f"    transients: {len(drifts)} drift segments")
        for segment in drifts[-3:]:
            line(
                f"      {local(segment['from'], tz_hours)} .. "
                f"{local(segment['to'], tz_hours)}  "
                f"{segment['change']:+.2f} points, "
                f"{segment['slopePer5min']:+.3f}/5min, "
                f"residual sd {segment['residualSd']:.3f}"
            )
        unexplained = finding.get("unexplainedSteps", [])
        line(
            f"    steps: {len(finding.get('steps', []))}, "
            f"{len(unexplained)} unexplained by this probe's pump"
        )
        for step in unexplained[-3:]:
            line(
                f"      {local(step['at'], tz_hours)}  {step['delta']:+.1f} points"
            )

        model = finding["model"]
        if "classes" in model:
            line(f"    model: {model['events']} watering events")
            if any(model["classes"][name]["weight"] > 0.0 for name in CLASSES):
                for name in CLASSES:
                    entry = model["classes"][name]
                    line(
                        f"      {name:5s} n={entry['n']:5d} "
                        f"weight={entry['weight']:8.1f} "
                        f"mean={entry['mean']:7.2f} sd={entry['sd']:6.2f}"
                    )
                line(
                    f"      J={model['separation']:.3f} "
                    f"(needs {MIN_SEPARATION:.0f}), "
                    f"{model['outliersDropped']} outliers dropped"
                )
            else:
                # An empty class is not a class at 0.00 +- 0.10; that is the
                # variance floor talking. Printing it would invite somebody to
                # read a parameter off a Gaussian nothing was fitted to.
                line("      no labelled samples: nothing bounds a watering cycle")
        line(f"    model REFUSED: {model['blockedBy']}" if model["blockedBy"] else
             "    model ACCEPTED")

        two_point = finding["twoPoint"]
        if two_point["proposed"]:
            proposed = two_point["proposed"]
            line(
                f"    two-point ACCEPTED: dry {proposed['dry']} "
                f"(sd {proposed['drySd']:.2f}, "
                f"{local(proposed['dryFrom'], tz_hours)}), "
                f"wet {proposed['wet']} (sd {proposed['wetSd']:.2f}, "
                f"{local(proposed['wetFrom'], tz_hours)})"
            )
        else:
            line(f"    two-point REFUSED: {two_point['blockedBy']}")
        line()

    fitted = sum(1 for finding in findings if finding["twoPoint"]["proposed"])
    if fitted:
        line(f"{fitted} of {len(findings)} probes have a proposed calibration.")
    else:
        line(
            "Nothing was fitted. That is an outcome, not a failure: the archive "
            "does not yet contain what these parameters are made of."
        )


def push_proposal(device, document, calibration):
    """UNEXERCISED - written, never run against the garden.

    POST /config.json replaces the whole file, and the handler restores every
    still-masked secret from the document on disk before writing - so what goes
    up is the masked document that came down with `moisture` replaced, and
    sending the eight asterisks back verbatim is exactly what the mask is for.
    A restore that FAILS is a 500 rather than a write, which is the guard that
    keeps a masked backup from becoming eight asterisks in a credential.

    The change lands at the NEXT BOOT - the response says `restartRequired` -
    and nothing here reboots the board, because every reset floats the GPIOs and
    these relays are active-low.
    """
    if device is None:
        raise SystemExit("--push needs the live device; drop --config")
    outgoing = dict(document)
    outgoing["moisture"] = calibration
    return device.post_config(outgoing)


# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=str(DEFAULT_DB))
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument(
        "--config", help="read a saved config document instead of the device"
    )
    parser.add_argument("--credentials", default=str(DEFAULT_CREDENTIALS))
    parser.add_argument("--tz", type=int, default=DEFAULT_TZ_HOURS)
    parser.add_argument("--out", help="write the proposal JSON here as well")
    parser.add_argument(
        "--since", help="ignore archive data before this date (YYYY-MM-DD)"
    )
    parser.add_argument(
        "--push",
        action="store_true",
        help="POST the proposal to the device. OFF by default and never yet run",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    document, device = load_config(args)
    try:
        conn = open_archive(Path(args.db))
        relay_events = read_relay_events(conn)
        seams = relay_seams(relay_events)
        sensors = document.get("io", {}).get("soilMoisture", [])
        slot_seams = probe_slot_seams(conn, len(sensors))

        floor = 0.0
        if args.since:
            zone = dt.timezone(dt.timedelta(hours=args.tz))
            floor = dt.datetime.strptime(args.since, "%Y-%m-%d").replace(
                tzinfo=zone
            ).timestamp()

        findings = []
        for index in range(len(sensors)):
            identity = probe_identity(document, index)
            boundary, relay_name, reasons = usable_from(
                identity, document, seams, slot_seams
            )
            start = max(boundary, floor)
            samples = [
                sample
                for sample in read_series(conn, identity["key"])
                if sample[0] >= start
            ]
            events = sorted(
                event["at"]
                for event in relay_events
                if event["started"]
                and event["name"] == relay_name
                and event["at"] >= start
            )
            finding = analyse_probe(identity, samples, events, args.tz)
            finding["relayName"] = relay_name
            finding["windowReasons"] = reasons
            findings.append(finding)

        report(document, findings, seams, slot_seams, args.tz, sys.stderr)

        calibration = build_proposal(document, findings)
        payload = {
            "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(
                timespec="seconds"
            ),
            "device": document.get("id"),
            "archive": args.db,
            "modelFile": {
                "proposed": not MODEL_FILE_DECLINED,
                "reason": "see MODEL_FILE_DECLINED in scripts/moisture_fit.py",
            },
            "probes": [
                {
                    "index": finding["identity"]["index"],
                    "name": finding["identity"]["name"],
                    "pin": finding["identity"]["pin"],
                    "relay": finding["identity"]["relay"],
                    "relayName": finding.get("relayName"),
                    "samples": finding["samples"],
                    "twoPoint": finding["twoPoint"],
                    "model": finding["model"],
                }
                for finding in findings
            ],
            "proposal": {"moisture": calibration} if calibration else None,
        }
        text = json.dumps(payload, indent=2, default=float)
        print(text)
        if args.out:
            Path(args.out).write_text(text + "\n", encoding="utf-8")

        if args.push:
            if not calibration:
                raise SystemExit(
                    "nothing was fitted, so there is nothing to push. "
                    "Lowering a threshold until something comes out is the one "
                    "thing this tool exists to refuse."
                )
            print(
                "PUSH: this path has never been run against the garden. "
                "It writes /config.json and the change lands at the next boot.",
                file=sys.stderr,
            )
            print(push_proposal(device, document, calibration), file=sys.stderr)
        return 0
    finally:
        if device is not None:
            device.logout()


if __name__ == "__main__":
    sys.exit(main())
