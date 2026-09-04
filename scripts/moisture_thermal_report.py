#!/usr/bin/env python3
"""The thermal confound applied to THIS archive, and the section that reports it.

moisture_thermal.py is the arithmetic - detrending, the partial slope, the
collinearity, the placebo null, the lag scan, the two admission gates - and is
free of any device, file, clock or sibling module. This is the half that knows
about the archive: which windows exist, which of them were disturbed, which
probes are peers of which, and what a person reading the report needs to see.

Split from moisture_fit.py along the same seam drying_fit.py and
drying_evidence.py use, and for the same reason: the pair crossed the 1000-line
gate check_lines.py enforces. moisture_fit.py keeps the device, the config, the
per-probe report and main(); this keeps the thermal section entire, because the
order of the checks inside it is load-bearing - the physical screen has to run
before any statistic, or a night containing sixteen waterings fits +8.0 pts/K
and passes every statistical check there is. That happened on the first run.

THE SCREEN IS NOT A NEW DETECTOR. `disturbance()` calls moisture_stats.py's
find_steps() and find_drift_segments(), which is why this module and not
moisture_thermal.py holds it: moisture_stats imports moisture_thermal, so the
dependency only runs one way from there, and a third detector written beside
the two that work is exactly what CLAUDE.md records deleting three probe-health
tests to avoid.
"""

from __future__ import annotations

import statistics

import moisture_thermal
from moisture_stats import WET_WINDOW_SEC, find_drift_segments, find_steps

MIN_NIGHT_HOURS = 6.0


def disturbance(samples, events):
    """Why this window cannot carry a slope at all, before any slope is fitted.

    Everything here is one of the detectors moisture_stats.py already owns,
    called rather than reimplemented - CLAUDE.md records three probe-health
    tests being deleted for accusing working hardware, and the way that is not
    repeated is by not writing a fourth detector beside the two that work.

    The order is physical-first, same as every other gate in this tool: a probe
    that was carried between pots is not additionally interesting for having
    been warm.
    """
    if not samples:
        return "no samples"
    values = [value for _, value in samples]
    if min(values) <= 0.01 or max(values) >= 99.99:
        return (
            f"railed: the reading touched {min(values):.2f}..{max(values):.2f}, "
            "so the probe was not reporting soil for part of this window"
        )
    if statistics.pstdev(values) < 0.05:
        return (
            f"stuck: the reading held {statistics.fmean(values):.2f} to within "
            "0.05 points, which is a series with no signal to explain"
        )
    watered = [event for event in events if samples[0][0] <= event <= samples[-1][0]]
    if watered:
        return (
            f"{len(watered)} watering(s) ran inside this window: a pump moves "
            "the reading by more than any temperature could"
        )
    unexplained = [
        step
        for step in find_steps(samples)
        if not any(0.0 <= step["at"] - event <= WET_WINDOW_SEC for event in events)
    ]
    if unexplained:
        largest = max(unexplained, key=lambda step: abs(step["delta"]))
        return (
            f"{len(unexplained)} unexplained step(s), the largest "
            f"{largest['delta']:+.1f} points: the probe or the pot changed"
        )
    drifts = find_drift_segments(samples)
    if drifts:
        steepest = max(drifts, key=lambda seg: abs(seg["slopePer5min"]))
        return (
            f"the probe was equilibrating ({steepest['slopePer5min']:+.3f} "
            "points/5 min): precise, and going somewhere that is not weather"
        )
    return None


def thermal_windows(samples, temperature, tz_hours, events):
    """The responses for one probe over one era: the whole span, then each night.

    Split from the verdicts so every probe's response exists before any verdict
    is reached, which is what the peer check needs - and it needs it per NIGHT
    as well as per era, or three of the four documented checks run on a night
    and the fourth quietly does not.
    """
    whole = moisture_thermal.thermal_response(samples, temperature)
    whole["disturbed"] = disturbance(samples, events)
    nights = {}
    for label, start, end in moisture_thermal.night_windows(samples, tz_hours):
        block = [sample for sample in samples if start <= sample[0] <= end]
        if len(block) < moisture_thermal.MIN_SAMPLES:
            continue
        if block[-1][0] - block[0][0] < MIN_NIGHT_HOURS * 3600.0:
            continue
        response = moisture_thermal.thermal_response(block, temperature)
        response["disturbed"] = disturbance(block, events)
        nights[label] = response
    return {"whole": whole, "nights": nights}


def usable_peers(responses, index):
    """The other probes' responses, minus the ones that are not evidence.

    A disturbed or unfittable window says nothing about anybody, so it must not
    be allowed to veto a peer by sloping the other way for a reason that has
    nothing to do with weather.
    """
    return [
        response
        for position, response in enumerate(responses)
        if position != index
        and response is not None
        and response["responses"]
        and not response.get("disturbed")
    ]


def thermal_era(windows, peer_windows, index):
    """One probe over one era: the whole span, then each night inside it.

    Both are reported because they fail DIFFERENTLY and both failures are the
    answer. The WHOLE era has a diurnal cycle running against a monotone drying
    trend, so it is identified in principle and refused in practice by the
    placebo or by the sign flipping with the trend order. Each NIGHT is the
    natural experiment somebody would try first, and it is refused by
    collinearity, because between 20:00 and 06:00 air temperature IS elapsed
    time to within a few per cent.
    """
    whole = windows["whole"]
    nights = []
    for label in sorted(windows["nights"]):
        response = windows["nights"][label]
        peers = usable_peers(
            [peer["nights"].get(label) for peer in peer_windows], index
        )
        nights.append(
            {
                "night": label,
                "verdict": moisture_thermal.thermal_verdict(response, peers=peers),
                "summary": moisture_thermal.thermal_summary(response),
                "response": response,
            }
        )
    return {
        "whole": {
            "verdict": moisture_thermal.thermal_verdict(
                whole, peers=usable_peers([peer["whole"] for peer in peer_windows], index)
            ),
            "summary": moisture_thermal.thermal_summary(whole),
        },
        "nights": nights,
        "response": whole,
    }


def thermal_findings(findings, temperature, tz_hours):
    """The temperature confound, per probe, per era.

    TWO ERAS, and the second one is why this is not simply the identity window.
    A calibration must not cross an archive seam, because the slot may hold a
    different probe on the other side. A SLOPE IN POINTS PER KELVIN NEED NOT:
    it is a property of whatever probe was in the slot over one window, so the
    span before the seam is a second, separately labelled measurement rather
    than contamination - and refusing to look at it would leave this tool with
    one night per probe and no way to reproduce its own headline. What is
    forbidden is a window that STRADDLES the seam, and neither of these does.

    The peer check needs every probe's whole-era response before any verdict,
    which is why the responses are computed in a first pass. Peers are taken
    WITHIN an era only: two probes are only evidence about each other while
    they were both in the ground at the same time.
    """
    if not temperature:
        return []
    eras = ("since the last archive seam", "before the last archive seam")
    keys = ("samples_list", "samples_before")
    computed = {
        key: [
            thermal_windows(
                finding.get(key) or [],
                temperature,
                tz_hours,
                finding.get("events") or [],
            )
            for finding in findings
        ]
        for key in keys
    }
    out = []
    for index, finding in enumerate(findings):
        entry = {
            "index": finding["identity"]["index"],
            "name": finding["identity"]["name"],
            "predictedSign": moisture_thermal.predicted_sign(
                finding["identity"]["invert"]
            ),
            "eras": [],
        }
        for label, key in zip(eras, keys):
            if len(finding.get(key) or []) < moisture_thermal.MIN_SAMPLES:
                continue
            era = thermal_era(computed[key][index], computed[key], index)
            era["label"] = label
            entry["eras"].append(era)
        out.append(entry)
    return out


def report_thermal(thermal, out):
    """The section that answers 'does temperature bias the reading', or refuses."""

    def line(text=""):
        print(text, file=out)

    line("THERMAL CONFOUND - these probes are resistive, so conduction is ionic")
    if not thermal:
        line("  no temperature series in the archive: the question cannot be asked")
        line()
        return
    accepted = 0
    examined = 0
    wrong_sign = 0

    def accept(response, sign):
        """Count an accepted window and say whether the physics predicted it."""
        nonlocal accepted, wrong_sign
        accepted += 1
        slope = response["responses"][0]["slope"] if response["responses"] else 0.0
        if slope * sign < 0.0:
            wrong_sign += 1
            return (
                f" (slope {slope:+.3f} has the sign ionic conduction does NOT "
                "predict for this probe's polarity)"
            )
        return ""

    for entry in thermal:
        line(f"--- probe {entry['index']}  {entry['name']}")
        if not entry["eras"]:
            line("    no window long enough to ask the question")
        for era in entry["eras"]:
            line(f"    [{era['label']}]")
            line(f"      whole span: {era['whole']['summary']}")
            examined += 1
            if era["whole"]["verdict"]:
                line(f"        REFUSED: {era['whole']['verdict']}")
            else:
                note = accept(era["response"], entry["predictedSign"])
                line(f"        ACCEPTED: this slope survived every check{note}")
            for night in era["nights"]:
                verdict = night["verdict"]
                examined += 1
                note = "" if verdict else accept(
                    night["response"], entry["predictedSign"]
                )
                line(
                    f"      night {night['night']}: "
                    + (f"REFUSED: {verdict}" if verdict else f"ACCEPTED{note}")
                )
                line(f"        {night['summary']}")
        line()
    line(f"{accepted} of {examined} windows produced a coefficient.")
    if wrong_sign:
        line(
            f"{wrong_sign} of those {accepted} carry the sign ionic conduction "
            "does NOT predict, which is evidence against a soil-thermal "
            "reading of them rather than for one."
        )
    if accepted:
        line(
            f"{accepted} window(s) produced a defensible temperature coefficient. "
            "It is REPORTED and NOT applied: correcting a reading needs the "
            "coefficient of SOIL temperature, and this device measures air "
            "behind thermal mass - see CLAUDE.md's evapotranspiration section."
        )
        line(
            "ACCEPTED here means 'not refused by these checks', and NOT 'this "
            "probe was in soil': the archive carries no probe-health verdict, "
            "so a floating pin - whose leakage current tracks temperature "
            "hard - passes every one of them. moistureNSd, shipped in 2.9.1 "
            "and not yet published, is the series that would close that gap."
        )
    else:
        line(
            "No window produced a temperature coefficient this archive can "
            "defend. That is an outcome, not a failure, and it is why nothing "
            "here corrects a reading: the two admission checks in the per-probe "
            "report above (anchor and class thermal regime) are what the "
            "finding bought."
        )
    line()
