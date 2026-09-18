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
  5. THERMAL REGIME.  These probes are resistive and soil conduction is ionic,
     so a span or a class separation measured partly warm and partly cold has
     some of its length made of temperature rather than water. Two gates, in
     scripts/moisture_thermal.py. They REFUSE and never correct: the archive
     was asked for the coefficient a correction would need and declined to
     supply one - see that module's header for what it measured instead.

The statistics live in scripts/moisture_stats.py beside this, the thermal
confound in scripts/moisture_thermal.py and its archive-facing half in
scripts/moisture_thermal_report.py; what stays here is the device, the archive
and the per-probe report. Standard library only, same as tb_export.py and
cloud_fit.py next door. Reads backups/telemetry.sqlite read-only and GETs the
device's config; it writes to the device only under --push, which is off by
default and has never been run.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

# Plain sibling modules: this file is run as a script, so scripts/ is
# sys.path[0] — the same arrangement dev_server.py has with its sim_* siblings.
# The split is statistics on one side and the device, the archive and the
# report on the other, so the half with the arithmetic in it can be exercised
# on its own.
import history_archive as ha
import history_store as store
from device_http import Device
from history_export import identity_seams, latest_identity
from moisture_stats import (CLASSES, MAX_GAP_SEC, MIN_SEPARATION,
                            analyse_probe, build_proposal, local, self_test)
from moisture_thermal_report import report_thermal, thermal_findings

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DB = ROOT / "backups" / "telemetry.sqlite"
DEFAULT_HISTORY_DB = ROOT / "backups" / "history.sqlite"
# The mDNS name rather than the address this file used to carry; see the note
# on DEFAULT_DEVICE in scripts/history_export.py, where a stale 192.168.1.55
# was found by a run that timed out against it on 2026-09-17.
DEFAULT_DEVICE = "espgarden1.local"
DEFAULT_CREDENTIALS = ROOT / "data" / "config.json"
DEFAULT_TZ_HOURS = -3


# ---------------------------------------------------------------------------
# WHY /moisture_model.bin IS NOT WRITTEN FROM HERE
# ---------------------------------------------------------------------------
#
# REVISED 2026-09-16, after scripts/history_export.py made the device's own 60 s
# record available and after reasons 2 and 3 - the two this comment called
# STRUCTURAL - were checked against the firmware rather than against this
# comment. One of them was wrong. The verdict did not move.
#
#   1. STANDS, and it is the binding one. There is nothing to seed. Both probes
#      have ZERO watering events on their own pump, so the six-event gate is
#      failed before any statistic is computed, and the file would carry
#      statistics the device would correctly refuse to use. The new source does
#      not change this: `relayMask` in the history record says exactly what the
#      archive's relay events say, because no pump has run.
#
#   2. WITHDRAWN AS WRITTEN. It said the archive's `moistureN` is "the
#      accumulator MEAN over one publish period" while "the device fits from its
#      own 60 s history records", so "averaging shrinks variance" and a J fitted
#      here would pass the device's gate for the wrong reason.
#
#      Both series are the SAME statistic. src/tasks.cpp's historyTaskHandler()
#      writes `record.moisture[i] = g_soilMoisture[i].getAverage()`, and
#      src/telemetry.cpp's addContinuous() publishes `moistureN` from the same
#      getAverage() on the same accumulator - whose window is sized ONCE, in
#      sensorsSetup(), as mqttPublishPeriodMs() / g_ioTaskPeriod = 300 samples.
#      So the device's own 60 s record is a 300-second trailing mean read out
#      every 60 s, and the archive is that same mean read out every 300 s. The
#      archive is a 5x DECIMATION of the history, not a smoothed version of it,
#      and their marginal variances have the same expectation.
#
#      What really differs is the SAMPLE COUNT inside a class window, and it
#      cuts the other way from the claim: the wet window is 30 minutes, which is
#      30 samples at 60 s and 6 at 300 s. `weight` is a sum of per-sample
#      confidences, so the archive under-counts it about fivefold against
#      MIN_WEIGHT_PER_CLASS = 20 - the archive path is HARDER to pass, not
#      easier - and each class variance is estimated from five times fewer
#      points. The 60 s extra samples are also heavily autocorrelated, since
#      consecutive records share 240 s of the same underlying window, so most of
#      that extra weight is fictitious independence. That is fine and it is the
#      point: it is the SAME fictitious independence the device's own trainer
#      has, and matching the device's arithmetic is the whole reason to fit from
#      its own record.
#
#   3. RESOLVED by the new source, exactly as this comment predicted.
#      `consumedUntil` is MoistureModelState's "epoch of the newest watering
#      event already folded in", and moistureModelTrain() sets it to
#      scan->consumeUntil, the newest rising edge across every probe in the
#      records it just scanned. From the device's own records that is
#      computable - history_archive.consumed_until() computes it - and 0 is
#      correct precisely when no edge exists, which is this garden today.
#
#   4. STANDS, unchanged. The struct layout has to be verified byte-for-byte
#      against a file the device wrote. sizeof(MoistureModelState) is checked on
#      load and the MOI2 magic exists because two fields once fitted inside
#      padding and the size check did not notice. That needs a read of
#      /spiffs/moisture_model.bin, which nothing here has done.
#
#   5. STANDS, unchanged. A push has to be upload-then-reboot: the device loads
#      the model at boot and saves its own at each training run, so an upload
#      into a running device is overwritten at the next save. The reboot is the
#      expensive half - every reset floats the GPIOs and these relays are
#      active-low, so it pulses every pump.
#
# So the score is 1, 4 and 5 standing, 3 resolved, 2 withdrawn as a
# mis-statement of what the two series are. The gate is reason 1 and it is not a
# tooling problem: it is a garden whose pumps have not run. What would change it
# is six waterings on a probe's own relay, inside one collected window.
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
# The OTHER source: the device's own 60 s record
# ---------------------------------------------------------------------------


def history_document(conn, args):
    """A config-shaped document from the archive's own identity stamp.

    scripts/history_export.py records the (index -> name, pin, relay) binding
    with every collection run, so an archive built by it is self-describing and
    needs no device and no saved config to be read. --config still wins when it
    is given, because an operator comparing a proposal against a document they
    hold should get THAT document's probe list.
    """
    identity = latest_identity(conn)
    if identity is None:
        raise SystemExit(
            f"{args.history_db} has no collection session in it; run "
            "scripts/history_export.py against the device first"
        )
    return {
        "id": identity.get("id"),
        "io": {
            "soilMoisture": [
                {"pin": probe["pin"], "name": probe["name"]}
                for probe in identity["probes"]
            ],
            "relays": [
                {"pin": relay["pin"], "name": relay["name"]}
                for relay in identity["relays"]
            ],
        },
        "moisture": [
            {"relay": probe["relay"], "invert": probe["invert"],
             "kind": probe["kind"], "dry": probe["dry"], "wet": probe["wet"]}
            for probe in identity["probes"]
        ],
    }


def history_findings(conn, document, args):
    """Per-probe findings from the 60 s record, and what the archive itself says.

    The seam rule is the same one usable_from() applies and the evidence is
    better. There, an edit to io.soilMoisture has to be INFERRED from a relay
    index changing name or a `moistureN` series going quiet, and the docstring
    calls that "deliberately blunt". Here every collection session carries the
    binding it saw, so a seam is a STATED disagreement between two sessions,
    dated to the run that noticed it. The rule stays blunt anyway - the most
    recent seam of any kind bounds every probe - because the archive still
    cannot say which SLOT moved, only that the io block was edited.
    """
    records = store.read_records(conn)
    if not records:
        raise SystemExit(f"{args.history_db} holds no records yet")

    times = [record["t"] for record in records]
    period = ha.observed_period(times) or 60.0
    gaps = ha.coverage_gaps(times, period)

    boundary = 0.0
    reasons = []
    for seam in identity_seams(conn):
        if seam["at"] > boundary:
            boundary = float(seam["at"])
            reasons = [
                f"session {seam['session']} saw a different binding: "
                + "; ".join(seam["changed"])
            ]
    if args.since:
        zone = dt.timezone(dt.timedelta(hours=args.tz))
        floor = dt.datetime.strptime(args.since, "%Y-%m-%d").replace(
            tzinfo=zone).timestamp()
        boundary = max(boundary, floor)

    usable = [record for record in records if record["t"] >= boundary]
    temperature = ha.series_for_key(records, "temp")

    findings = []
    relays = document.get("io", {}).get("relays", [])
    for index in range(len(document.get("io", {}).get("soilMoisture", []))):
        identity = probe_identity(document, index)
        relay_name = None
        if 0 <= identity["relay"] < len(relays):
            relay_name = relays[identity["relay"]].get("name")
        samples = ha.samples_for_probe(usable, index)
        events = ha.events_for_relay(usable, identity["relay"], period)
        finding = analyse_probe(identity, samples, events, args.tz,
                                temperature, period_sec=period)
        finding["relayName"] = relay_name
        finding["windowReasons"] = reasons
        finding["samples_list"] = samples
        finding["samples_before"] = [
            sample for sample in ha.samples_for_probe(records, index)
            if sample[0] < boundary
        ]
        finding["events"] = ha.events_for_relay(
            records, identity["relay"], period)
        findings.append(finding)

    summary = {
        "records": len(records),
        "usable": len(usable),
        "periodSec": period,
        "from": times[0],
        "to": times[-1],
        "gaps": gaps,
        "missing": sum(gap["missing"] for gap in gaps),
        "consumedUntil": ha.consumed_until(
            usable, [probe["relay"] for probe in document.get("moisture", [])]),
    }
    return findings, summary


def report_history(summary, tz_hours, out):
    def line(text=""):
        print(text, file=out)

    line("SOURCE - the device's own record, not the ThingsBoard publish")
    line(f"  {summary['records']} records at {summary['periodSec']:.0f} s, "
         f"{local(summary['from'], tz_hours)} .. "
         f"{local(summary['to'], tz_hours)}")
    line(f"  {summary['usable']} of them inside the identity window")
    if summary["gaps"]:
        line(f"  {len(summary['gaps'])} gaps, ~{summary['missing']} records "
             "missing. A gap is EITHER records nobody collected OR records the "
             "device never wrote")
        for gap in summary["gaps"][-3:]:
            line(f"    {local(gap['from'], tz_hours)} .. "
                 f"{local(gap['to'], tz_hours)}  ~{gap['missing']} records")
    else:
        line("  no gaps: every record between those two stamps is here")
    if summary["consumedUntil"]:
        line(f"  consumedUntil would be {summary['consumedUntil']} "
             f"({local(summary['consumedUntil'], tz_hours)}) - the newest "
             "rising edge across every pump")
    else:
        line("  consumedUntil would be 0: no pump has run inside this window, "
             "which is also why nothing can be seeded")
    line()


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


def fit_from_history(args):
    """The whole run, against the device's own record. No network at all.

    Everything downstream of the source is shared with the ThingsBoard path -
    the same analyse_probe, the same gates, the same thermal checks, the same
    proposal shape - because the two differ in where the numbers come from and
    in nothing else. A second fitting routine would be a second set of
    thresholds to keep in step, and CLAUDE.md has a section about what happens
    when two implementations of one contract drift.
    """
    conn = store.open_archive(
        Path(args.history_db),
        create=False) if Path(args.history_db).exists() else None
    if conn is None:
        raise SystemExit(
            f"no archive at {args.history_db}; run "
            "scripts/history_export.py first")

    if args.config:
        document = json.loads(Path(args.config).read_text(encoding="utf-8"))
    else:
        document = history_document(conn, args)

    findings, summary = history_findings(conn, document, args)
    temperature = ha.series_for_key(store.read_records(conn), "temp")
    thermal = thermal_findings(findings, temperature, args.tz)

    report_history(summary, args.tz, sys.stderr)
    report(document, findings, [], [], args.tz, sys.stderr)
    report_thermal(thermal, sys.stderr)

    calibration = build_proposal(document, findings)
    payload = {
        "generatedAt": dt.datetime.now(dt.timezone.utc).isoformat(
            timespec="seconds"),
        "device": document.get("id"),
        "archive": args.history_db,
        "source": {
            "kind": "history",
            "periodSec": summary["periodSec"],
            "records": summary["records"],
            "gaps": len(summary["gaps"]),
            "missingRecords": summary["missing"],
        },
        "modelFile": {
            "proposed": not MODEL_FILE_DECLINED,
            # The value the device would need, computed rather than guessed.
            # It is reason 3 in MODEL_FILE_DECLINED, and it is the one this
            # source resolves - reported even while the file is declined,
            # because a resolved blocker that nothing prints is a blocker
            # somebody re-derives next year.
            "consumedUntil": summary["consumedUntil"],
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
        raise SystemExit(
            "--push needs the live device's own document, which --history-db "
            "does not read. Run the ThingsBoard path, or pass --config with "
            "the document you intend to write back."
        )
    conn.close()
    return 0


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
    parser.add_argument(
        "--history-db", nargs="?", const=str(DEFAULT_HISTORY_DB),
        help="fit from the device's OWN 60 s record, collected by "
             "scripts/history_export.py, instead of the 300 s ThingsBoard "
             "archive. Needs no device: the archive carries its own identity",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.history_db:
        return fit_from_history(args)

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

        # The whole temperature series, not the per-probe window: a placebo
        # needs days OUTSIDE the window it is a null for, and clipping it to
        # the seam would leave the check with nothing to shift onto.
        temperature = read_series(conn, "temperature")

        findings = []
        for index in range(len(sensors)):
            identity = probe_identity(document, index)
            boundary, relay_name, reasons = usable_from(
                identity, document, seams, slot_seams
            )
            start = max(boundary, floor)
            whole = read_series(conn, identity["key"])
            samples = [sample for sample in whole if sample[0] >= start]
            # Kept for the THERMAL section only, and never for a calibration:
            # the slot may have held a different probe before the seam, which
            # is exactly what usable_from() refuses to guess about.
            before = [sample for sample in whole if sample[0] < start]
            events = sorted(
                event["at"]
                for event in relay_events
                if event["started"]
                and event["name"] == relay_name
                and event["at"] >= start
            )
            finding = analyse_probe(identity, samples, events, args.tz, temperature)
            finding["relayName"] = relay_name
            finding["windowReasons"] = reasons
            finding["samples_list"] = samples
            finding["samples_before"] = before
            # Every start of this probe's pump, INCLUDING before the seam: the
            # thermal section looks at that era too, and a watering there
            # disturbs a window just as much as one after it.
            finding["events"] = sorted(
                event["at"]
                for event in relay_events
                if event["started"] and event["name"] == relay_name
            )
            findings.append(finding)

        thermal = thermal_findings(findings, temperature, args.tz)
        report(document, findings, seams, slot_seams, args.tz, sys.stderr)
        report_thermal(thermal, sys.stderr)

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
            # The per-window responses are dropped and only the verdicts kept:
            # a lag scan for every night of every probe is thousands of numbers
            # nothing downstream reads, and the report already printed the ones
            # a person would look at.
            "thermal": [
                {
                    "index": entry["index"],
                    "name": entry["name"],
                    "predictedSign": entry["predictedSign"],
                    "eras": [
                        {
                            "window": era["label"],
                            "verdict": era["whole"]["verdict"],
                            "summary": era["whole"]["summary"],
                            "nights": [
                                {
                                    "night": night["night"],
                                    "verdict": night["verdict"],
                                    "summary": night["summary"],
                                }
                                for night in era["nights"]
                            ],
                        }
                        for era in entry["eras"]
                    ],
                }
                for entry in thermal
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
