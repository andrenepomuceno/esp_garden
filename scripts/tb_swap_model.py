#!/usr/bin/env python3
"""The permutation, the refusals and the boundary evidence tb_swap_keys.py uses.

Split out of scripts/tb_swap_keys.py when the file crossed the 1000-line gate,
along the seam this repository already uses twice -- tb_import.py / tb_client.py
and history_export.py / history_archive.py / history_store.py: the tool that
takes the COMMANDS in one file, and in another the part that can be reasoned
about on its own.

Here that second part is everything that DECIDES, and it is decide-only in a
strict sense: nothing in this file opens a socket, reads a credential or knows
what ThingsBoard is. That is the whole reason it exists as a file. A relabelling
of a live garden's telemetry is authorised on the strength of two claims --
that the operation is a clean permutation, and that the window's edges are
where the record says they are -- and both are settled here, by functions a
host test can call with no network and no device.

`tb_swap_keys.py --self-test` runs `self_test()` below; there is deliberately
one entry point for it, on the tool the operator actually types, exactly as
`tb_import.py --self-test` is what covers tb_client.py.

Standard library only, like every other tool in scripts/.
"""

from __future__ import annotations

import hashlib
import json
import statistics
from datetime import datetime
from pathlib import Path

from tb_client import build_body, json_literal

# The six keys, as three pairs. ALL OR NONE: `moisture1` is the accumulator
# mean, `moisture1Now` the instantaneous twin and `moisture1Sd` the spread of
# the same window, so they are three views of ONE probe. Moving the mean and
# leaving the spread behind would produce a record where a probe's error bar
# belongs to a different pot -- inconsistent rather than merely mislabelled,
# and far harder for a later reader to notice or undo.
#
# There are deliberately no `moistureNState` / `moistureNFault` entries: this
# device has never published either, because the probes are uncalibrated so
# moistureState() returns empty and a healthy probe carries no fault key.
DEFAULT_PAIRS = [
    ("moisture1", "moisture3"),
    ("moisture1Now", "moisture3Now"),
    ("moisture1Sd", "moisture3Sd"),
]

# The window CLAUDE.md's 2026-09-21 entry establishes, as the default, so a
# bare `--plan` reproduces exactly the correction that was asked for -- refusal
# included, if the evidence disagrees with it.
DEFAULT_FROM = "2026-09-18 15:04:55"
DEFAULT_TO = "2026-09-21 19:34:00"

# How many samples either side of a boundary the evidence test averages, and
# how many it SKIPS on the far side. The skip is not a fudge: physically
# swapping two plugs is several operations with the board rebooted in the
# middle, so the samples immediately after the boundary describe a hand on the
# hardware, not either wiring. Medians, so one handling sample cannot carry the
# verdict. Both are arguments, and both are printed with the answer.
EVIDENCE_SAMPLES = 3
EVIDENCE_SETTLE = 3

# A START boundary is not a crossover -- before it there was no probe to cross.
# What corroborates it is that the record RESUMES there: the gap immediately
# before the first in-window sample must be this many times the median cadence.
# 3 is a judgement, not a measurement; the gap it is actually asked about is
# 174.8 min against a 5.0 min cadence, which is 35x, so nothing here turns on
# the exact factor.
START_GAP_FACTOR = 3.0


# --------------------------------------------------------------------------
# Time
# --------------------------------------------------------------------------

def parse_local(text: str) -> int:
    """`YYYY-MM-DD HH:MM:SS` in the workstation's own zone, as epoch ms.

    Local, not UTC, because every timestamp a human reads about this garden --
    in CLAUDE.md, in the serial log, on the dashboard -- is local, and a tool
    that quietly meant UTC would shift a window by three hours and still look
    right in its own output.
    """
    return int(datetime.strptime(text, "%Y-%m-%d %H:%M:%S").timestamp() * 1000)


def window_bounds(from_text: str, to_text: str) -> tuple:
    """The window as [lower, upper] in ms, with the END inclusive of its second.

    A publish lands at 18:44:06.373, not at 18:44:06.000, because the io tick
    has jitter in it. A boundary a human writes as `18:44:06` means "that
    publish", so the upper bound covers the whole of that second; without the
    999 ms the sample named in the argument is the first one EXCLUDED, which
    is a silent off-by-one-sample and the sample at a boundary is exactly the
    one somebody thought hard about.
    """
    lower, upper = parse_local(from_text), parse_local(to_text)
    if upper < lower:
        raise ValueError("the window runs backwards: %s is after %s"
                         % (from_text, to_text))
    return lower, upper + 999


def fmt_ts(ms: int) -> str:
    return datetime.fromtimestamp(ms / 1000.0).strftime("%Y-%m-%d %H:%M:%S")


# --------------------------------------------------------------------------
# The permutation -- pure, and the heart of the claim
# --------------------------------------------------------------------------

def pair_refusal(pairs, keys_present) -> str:
    """Why this key set may not be swapped, or None.

    Two failures, and the second is the one that matters. A pair naming a key
    the device does not hold is a typo. A key set that is a SUBSET of the
    declared pairs is the inconsistency this tool exists not to create.
    """
    if not pairs:
        return "no pairs to swap"
    flat = [k for pair in pairs for k in pair]
    if len(set(flat)) != len(flat):
        return ("a key appears in more than one pair (%s); the result would "
                "depend on the order the pairs were applied in"
                % ", ".join(sorted(k for k in set(flat)
                                   if flat.count(k) > 1)))
    missing = [k for k in flat if k not in keys_present]
    if missing:
        return ("the device does not hold %s; a pair must name two keys that "
                "exist, or the swap invents a series"
                % ", ".join(sorted(missing)))
    declared = {k for pair in DEFAULT_PAIRS for k in pair}
    asked = set(flat)
    if asked & declared and asked != declared:
        return ("this is a partial swap: %s would move and %s would not. The "
                "mean, its Now twin and its Sd are three views of one probe, "
                "so moving some of them leaves the record inconsistent rather "
                "than merely wrong. Swap all six or none."
                % (", ".join(sorted(asked & declared)),
                   ", ".join(sorted(declared - asked))))
    return None


def alignment_refusal(pairs, state) -> str:
    """Why the two halves of a pair cannot be exchanged point for point.

    A timestamp one key holds and the other does not cannot be swapped: there
    is nothing to put in its place, and inventing one is the thing this tool
    refuses to do. The counts are named so the operator can see how far apart
    they are rather than being told only that they differ.
    """
    problems = []
    for a, b in pairs:
        ta, tb = set(state.get(a, {})), set(state.get(b, {}))
        only_a, only_b = ta - tb, tb - ta
        if only_a or only_b:
            problems.append(
                "%s and %s do not line up: %d timestamp(s) only in %s, %d only "
                "in %s (%d in common). Nothing is invented for the odd ones."
                % (a, b, len(only_a), a, len(only_b), b, len(ta & tb)))
    return "; ".join(problems) if problems else None


def window_refusal(state, lower: int, upper: int) -> str:
    """Why the collected points are not all inside the window, or None.

    The read is bounded, so this can only fire on a programming error -- which
    is exactly why it is here: a swap that reached one sample past the rewire
    would un-correct a point that was already right, and nothing downstream
    would say so.
    """
    stray = []
    for key, points in state.items():
        for ts in points:
            if ts < lower or ts > upper:
                stray.append((key, ts))
    if not stray:
        return None
    stray.sort(key=lambda kv: kv[1])
    return ("%d point(s) lie outside the window %s .. %s, first %s at %s"
            % (len(stray), fmt_ts(lower), fmt_ts(upper),
               stray[0][0], fmt_ts(stray[0][1])))


def swap_state(state, pairs) -> dict:
    """The same points with each pair's names exchanged.

    Pure, total and self-inverse by construction: nothing is parsed, nothing is
    formatted, only the key a text is filed under changes. `--self-test`
    asserts the involution on synthetic data and `--probe` asserts it against
    the real server.
    """
    out = {key: dict(points) for key, points in state.items()}
    for a, b in pairs:
        out[a], out[b] = dict(state.get(b, {})), dict(state.get(a, {}))
    return out


def classify(current, target, before, pairs) -> dict:
    """Every timestamp as PENDING, DONE, FOREIGN or IDENTICAL.

    This is what replaces a ledger, and it is stronger than one. `before` is
    what the plan recorded, `target` is that permuted, `current` is what the
    server holds right now:

      pending    still the pre-swap text -- write it
      done       already the swapped text -- an earlier run got there
      identical  the two are the same text, so the swap is a no-op here
      foreign    neither; something else changed this point and the tool must
                 not guess which of the two it is looking at
    """
    keys = [k for pair in pairs for k in pair]
    stamps = sorted({ts for k in keys for ts in before.get(k, {})})
    out = {"pending": [], "done": [], "foreign": [], "identical": []}
    for ts in stamps:
        cur = tuple(current.get(k, {}).get(ts) for k in keys)
        was = tuple(before.get(k, {}).get(ts) for k in keys)
        want = tuple(target.get(k, {}).get(ts) for k in keys)
        if was == want:
            out["identical"].append(ts)
        elif cur == want:
            out["done"].append(ts)
        elif cur == was:
            out["pending"].append(ts)
        else:
            out["foreign"].append(ts)
    return out


def swap_points(target, pairs, stamps):
    """`[(ts, [(key, text), ...]), ...]` for build_body, one entry per instant.

    Both halves of every pair go in one entry, so the exchange at an instant is
    one write and cannot be half-applied by a dropped request.
    """
    keys = [k for pair in pairs for k in pair]
    points = []
    for ts in stamps:
        values = [(k, target[k][ts]) for k in keys if ts in target.get(k, {})]
        if values:
            points.append((ts, values))
    return points


# --------------------------------------------------------------------------
# The boundary evidence -- does the record agree with the window?
# --------------------------------------------------------------------------

def numeric(values):
    out = []
    for v in values:
        try:
            out.append(float(v))
        except (TypeError, ValueError):
            pass
    return out


def crossover_evidence(before_a, before_b, after_a, after_b) -> dict:
    """Do two series EXCHANGE levels across a boundary?

    The end of a crossed window is, by definition, the moment the wiring was
    fixed -- so the raw series must step past each other there. Compare the
    median level either side two ways:

      same   how far each key is from ITS OWN level across the boundary
      cross  how far each key is from the OTHER key's level

    A genuine crossover has cross << same. Continuity under `same` means the
    boundary is somewhere else, and a swap applied past it makes correct points
    wrong by roughly the amount it was meant to fix.
    """
    ba, bb = numeric(before_a), numeric(before_b)
    aa, ab = numeric(after_a), numeric(after_b)
    if not (ba and bb and aa and ab):
        return {"ok": False, "why": "not enough numeric samples either side",
                "same": None, "cross": None, "margin": None}
    mba, mbb = statistics.median(ba), statistics.median(bb)
    maa, mab = statistics.median(aa), statistics.median(ab)
    same = abs(maa - mba) + abs(mab - mbb)
    cross = abs(maa - mbb) + abs(mab - mba)
    return {"ok": cross < same, "why": None,
            "before_a": mba, "before_b": mbb, "after_a": maa, "after_b": mab,
            "same": same, "cross": cross, "margin": same - cross}


def gap_evidence(previous_ts, first_ts, cadence_sec) -> dict:
    """Does the record RESUME at the start of the window?

    The start of this window is not a crossover -- before it all four channels
    swing 0..93 while the operator is wiring, so nothing there measures
    anything and it is excluded rather than swapped. What can be checked is
    that there is a hole in front of it far larger than the publish cadence.
    """
    if previous_ts is None:
        return {"ok": True, "gap_sec": None, "cadence_sec": cadence_sec,
                "why": "nothing is stored before the window at all"}
    gap = (first_ts - previous_ts) / 1000.0
    return {"ok": cadence_sec > 0 and gap >= START_GAP_FACTOR * cadence_sec,
            "gap_sec": gap, "cadence_sec": cadence_sec, "why": None}


def cadence(stamps) -> float:
    if len(stamps) < 2:
        return 0.0
    deltas = [(b - a) / 1000.0 for a, b in zip(stamps, stamps[1:])]
    return statistics.median(deltas)


def boundary_refusal(start_ev, end_ev, override: bool) -> str:
    """Why the window's own edges say it is in the wrong place, or None."""
    if override:
        return None
    bad = []
    if not start_ev.get("ok"):
        bad.append("the window START is not preceded by a hole in the record "
                   "(gap %s s against a %s s cadence), so the samples before "
                   "it are ordinary data that the window silently excludes"
                   % (_num(start_ev.get("gap_sec")),
                      _num(start_ev.get("cadence_sec"))))
    if not end_ev.get("ok"):
        bad.append("the window END is not a crossover: the two series are "
                   "continuous under their OWN names there (same %s, crossed "
                   "%s). The exchange happened somewhere else, and swapping up "
                   "to this point would un-correct samples that are already "
                   "right" % (_num(end_ev.get("same")),
                              _num(end_ev.get("cross"))))
    if not bad:
        return None
    return ("; ".join(bad)
            + ". Move the boundary, or pass --boundary-override if the stored "
              "series genuinely cannot show it.")


def _num(v):
    return "n/a" if v is None else ("%.2f" % v)


# --------------------------------------------------------------------------
# The plan file
# --------------------------------------------------------------------------

def plan_fingerprint(server, device, lower, upper, pairs, counts) -> str:
    """What a stored plan is a plan OF.

    Device, server, window, the pairs and the per-key point counts. A plan
    reused across any of those is a plan for a different correction, and the
    `before` state inside it -- the only record of what the series held -- would
    be describing other points.
    """
    h = hashlib.sha256()
    h.update(("%s|%s|%d|%d|" % (server, device, lower, upper)).encode())
    h.update(("|".join("%s>%s" % p for p in pairs)).encode())
    h.update(("|".join("%s=%d" % (k, counts[k])
                       for k in sorted(counts))).encode())
    return h.hexdigest()[:16]


def write_plan(path: Path, plan: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(plan, indent=1, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def read_plan(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def plan_state(plan) -> dict:
    """The stored `before` back as {key: {int ts: text}}; JSON keys are text."""
    return {k: {int(ts): v for ts, v in points.items()}
            for k, points in plan["before"].items()}


def replan_refusal(existing, fingerprint: str, force: bool) -> str:
    """Why an existing plan must not be silently overwritten, or None.

    The same lesson tb_import.py's census carries, and here it is sharper. A
    plan records what the series held BEFORE the swap. Re-planning after a
    successful apply would record the SWAPPED state as `before`, and applying
    that plan would swap it straight back -- a restore wearing the name of a
    correction, with the plan file agreeing with itself at every step.
    """
    if not existing:
        return None
    if existing.get("fingerprint") != fingerprint:
        return None
    if existing.get("applied") and not force:
        return ("a plan with this fingerprint is already marked APPLIED. "
                "Re-planning would record the swapped series as the `before` "
                "state, and applying that would swap it back. Use --verify to "
                "check the applied one, or --replan if the stored plan is "
                "known to be wrong.")
    return None


def sample_stamps(stamps, values):
    """First, last, middle, and the timestamp holding the longest literal.

    The longest literal is the fragile one: its TYPE hangs on a digit, so it is
    the point that would betray a re-typed series when the other three could
    not.
    """
    if not stamps:
        return []
    picks = [stamps[0], stamps[-1], stamps[len(stamps) // 2]]
    picks.append(max(stamps, key=lambda t: (len(values.get(t, "")), t)))
    seen, out = set(), []
    for t in picks:
        if t not in seen:
            seen.add(t)
            out.append(t)
    return out


# --------------------------------------------------------------------------
# Self-test -- offline, no credential, no network
# --------------------------------------------------------------------------

def self_test() -> int:
    checks = []

    def ok(name, got, want):
        checks.append((name, got == want, got, want))

    pairs = [("a", "b"), ("aN", "bN")]
    state = {"a": {1: "10", 2: "11"}, "b": {1: "20", 2: "21"},
             "aN": {1: "1.5", 2: "1.6"}, "bN": {1: "2.5", 2: "2.6"}}

    # The permutation, and the property the whole correction rests on.
    once = swap_state(state, pairs)
    ok("a takes b's values", once["a"], {1: "20", 2: "21"})
    ok("b takes a's values", once["b"], {1: "10", 2: "11"})
    ok("the second pair moves too", once["aN"], {1: "2.5", 2: "2.6"})
    ok("swapping twice restores exactly", swap_state(once, pairs), state)
    ok("swapping does not mutate its input", state["a"], {1: "10", 2: "11"})
    ok("an unlisted key is carried through untouched",
       swap_state({**state, "z": {1: "9"}}, pairs)["z"], {1: "9"})
    ok("the text is never reformatted",
       swap_state({"a": {1: "0.47609522938728333"}, "b": {1: "0"}},
                  [("a", "b")])["b"], {1: "0.47609522938728333"})
    ok("an empty state swaps to an empty state", swap_state({}, [("a", "b")]),
       {"a": {}, "b": {}})

    # The multiset identity: relabelling creates and destroys nothing.
    flat = lambda s: sorted((t, v) for k in s for t, v in s[k].items())
    ok("the (ts, value) multiset is unchanged", flat(once), flat(state))

    # Pair refusals.
    present = {"moisture1", "moisture3", "moisture1Now", "moisture3Now",
               "moisture1Sd", "moisture3Sd", "temperature"}
    ok("the full six are allowed", pair_refusal(DEFAULT_PAIRS, present), None)
    ok("no pairs is refused", pair_refusal([], present) is not None, True)
    ok("a partial swap is refused",
       "partial swap" in (pair_refusal([("moisture1", "moisture3")], present)
                          or ""), True)
    ok("a mean without its Sd is refused",
       pair_refusal([("moisture1", "moisture3"),
                     ("moisture1Now", "moisture3Now")], present) is not None,
       True)
    ok("a missing key is refused",
       "does not hold" in (pair_refusal([("nope", "temperature")], present)
                           or ""), True)
    ok("a key in two pairs is refused",
       "more than one pair" in (pair_refusal([("x", "y"), ("x", "z")],
                                             {"x", "y", "z"}) or ""), True)
    ok("a declared key paired with an outsider is refused",
       pair_refusal([("temperature", "moisture1")],
                    present) is not None, True)
    ok("a pair of keys this tool never declared is allowed",
       pair_refusal([("temperature", "humidity")],
                    present | {"humidity"}), None)

    # Alignment: a timestamp on one side only.
    ok("aligned keys pass", alignment_refusal([("a", "b")], state), None)
    lop = {"a": {1: "1", 2: "2"}, "b": {1: "3"}}
    ok("a lopsided pair is refused",
       alignment_refusal([("a", "b")], lop) is not None, True)
    ok("the refusal counts both sides",
       "1 timestamp(s) only in a" in alignment_refusal([("a", "b")], lop), True)
    ok("a missing key reads as zero points",
       alignment_refusal([("a", "zz")], lop) is not None, True)

    # Window.
    ok("points inside the window pass", window_refusal(state, 0, 10), None)
    ok("a point past the window is refused",
       window_refusal(state, 0, 1) is not None, True)
    ok("a point before the window is refused",
       window_refusal(state, 2, 10) is not None, True)
    ok("the refusal names the first stray",
       "a at" in window_refusal({"a": {5: "x"}}, 10, 20), True)

    # classify(): the ledger replacement.
    before = {"a": {1: "10", 2: "11"}, "b": {1: "20", 2: "21"}}
    target = swap_state(before, [("a", "b")])
    ok("an untouched series is all pending",
       classify(before, target, before, [("a", "b")])["pending"], [1, 2])
    ok("a written series is all done",
       classify(target, target, before, [("a", "b")])["done"], [1, 2])
    half = {"a": {1: "20", 2: "11"}, "b": {1: "10", 2: "21"}}
    got = classify(half, target, before, [("a", "b")])
    ok("a half-written series splits", (got["done"], got["pending"]),
       ([1], [2]))
    alien = {"a": {1: "99", 2: "11"}, "b": {1: "20", 2: "21"}}
    ok("an unexpected value is foreign",
       classify(alien, target, before, [("a", "b")])["foreign"], [1])
    same = {"a": {1: "7"}, "b": {1: "7"}}
    ok("identical values are neither pending nor foreign",
       classify(same, swap_state(same, [("a", "b")]), same,
                [("a", "b")])["identical"], [1])
    ok("a point the server lost is foreign",
       classify({"a": {}, "b": {}}, target, before, [("a", "b")])["foreign"],
       [1, 2])

    # swap_points / build_body: the literal must survive to the wire.
    pts = swap_points(target, [("a", "b")], [1])
    body = build_body(pts)
    ok("both halves go in one entry", json.loads(body)[0]["values"],
       {"a": 20, "b": 10})
    ok("one entry per timestamp", len(json.loads(body)), 1)
    long_state = {"a": {1: "0.47609522938728333"}, "b": {1: "1"}}
    long_body = build_body(swap_points(swap_state(long_state, [("a", "b")]),
                                       [("a", "b")], [1]))
    ok("a 17-digit literal reaches the wire whole",
       b'"b":0.47609522938728333' in long_body, True)
    ok("float() would have shortened it",
       repr(float("0.47609522938728333")) != "0.47609522938728333", True)
    ok("json_literal leaves a plain double alone",
       json_literal("84.47058868408203"), "84.47058868408203")

    # Boundary evidence.
    cross = crossover_evidence([82.9, 82.9], [71.4, 71.4],
                               [71.1, 71.2], [83.5, 83.4])
    ok("an exchange of levels is a crossover", cross["ok"], True)
    ok("its margin is large", cross["margin"] > 20, True)
    flat_ev = crossover_evidence([72.2, 72.2], [79.3, 79.3],
                                 [71.1, 71.2], [83.5, 83.4])
    ok("a continuous pair is not a crossover", flat_ev["ok"], False)
    ok("a boundary with no numbers is not corroborated",
       crossover_evidence([], [], [], [])["ok"], False)
    ok("non-numeric samples are dropped, not crashed on",
       crossover_evidence(["x", "82.9"], ["71.4"], ["71.1"], ["83.5"])["ok"],
       True)
    ok("a resumed record corroborates a start",
       gap_evidence(1000, 1000 + 10 * 300000, 300.0)["ok"], True)
    ok("an unbroken record does not",
       gap_evidence(1000, 1000 + 300000, 300.0)["ok"], False)
    ok("a start with nothing before it is allowed",
       gap_evidence(None, 5, 300.0)["ok"], True)
    ok("a gap exactly at the factor passes",
       gap_evidence(0, int(START_GAP_FACTOR * 300 * 1000), 300.0)["ok"], True)
    ok("a gap one millisecond short of it does not",
       gap_evidence(0, int(START_GAP_FACTOR * 300 * 1000) - 1, 300.0)["ok"],
       False)
    ok("cadence is the median interval",
       cadence([0, 300000, 600000, 1500000]), 300.0)
    ok("cadence of a single sample is zero", cadence([7]), 0.0)

    good = {"ok": True}
    ok("two good boundaries pass", boundary_refusal(good, cross, False), None)
    ok("a bad end is refused",
       boundary_refusal(good, flat_ev, False) is not None, True)
    ok("the refusal says why",
       "not a crossover" in boundary_refusal(good, flat_ev, False), True)
    ok("a bad start is refused",
       boundary_refusal({"ok": False, "gap_sec": 1.0, "cadence_sec": 300.0},
                        cross, False) is not None, True)
    ok("--boundary-override lets both through",
       boundary_refusal({"ok": False, "gap_sec": 1.0, "cadence_sec": 300.0},
                        flat_ev, True), None)

    # The plan's fingerprint and the re-plan guard.
    counts = {"a": 2, "b": 2}
    fp = plan_fingerprint("s", "d", 1, 2, [("a", "b")], counts)
    ok("the same plan fingerprints the same",
       plan_fingerprint("s", "d", 1, 2, [("a", "b")], counts), fp)
    ok("a different device differs",
       plan_fingerprint("s", "e", 1, 2, [("a", "b")], counts) != fp, True)
    ok("a different window differs",
       plan_fingerprint("s", "d", 1, 3, [("a", "b")], counts) != fp, True)
    ok("a different pair differs",
       plan_fingerprint("s", "d", 1, 2, [("a", "c")], counts) != fp, True)
    ok("a different count differs",
       plan_fingerprint("s", "d", 1, 2, [("a", "b")], {"a": 2, "b": 3}) != fp,
       True)
    ok("no stored plan is not a refusal", replan_refusal(None, fp, False), None)
    ok("an unapplied plan may be replaced",
       replan_refusal({"fingerprint": fp, "applied": False}, fp, False), None)
    ok("an applied plan may not",
       replan_refusal({"fingerprint": fp, "applied": True}, fp, False)
       is not None, True)
    ok("--replan overrides it",
       replan_refusal({"fingerprint": fp, "applied": True}, fp, True), None)
    ok("an applied plan for another window is irrelevant",
       replan_refusal({"fingerprint": "other", "applied": True}, fp, False),
       None)

    # Sampling: the longest literal must always be one of the four.
    vals = {1: "1", 2: "0.47609522938728333", 3: "2", 4: "3", 5: "4"}
    picked = sample_stamps(sorted(vals), vals)
    ok("the longest literal is sampled", 2 in picked, True)
    ok("the first and last are sampled",
       1 in picked and 5 in picked, True)
    ok("sampling an empty key returns nothing", sample_stamps([], {}), [])
    ok("sampling never repeats a timestamp",
       len(sample_stamps([7], {7: "x"})), 1)

    # Time.
    ok("a window parses to a round second",
       parse_local("2026-09-18 15:04:55") % 1000, 0)
    ok("the two defaults are ordered",
       parse_local(DEFAULT_FROM) < parse_local(DEFAULT_TO), True)
    ok("formatting round-trips",
       fmt_ts(parse_local("2026-09-21 19:34:00")), "2026-09-21 19:34:00")
    lo, hi = window_bounds("2026-09-18 15:04:55", "2026-09-21 18:44:06")
    ok("the end covers its own second", hi - parse_local("2026-09-21 18:44:06"),
       999)
    ok("a sample at 18:44:06.373 is inside",
       parse_local("2026-09-21 18:44:06") + 373 <= hi, True)
    ok("a sample at 18:44:07.000 is outside",
       parse_local("2026-09-21 18:44:07") <= hi, False)
    ok("the start is the top of its second",
       lo, parse_local("2026-09-18 15:04:55"))
    ok("a one-second window is legal, not empty",
       window_bounds("2026-09-21 19:00:00", "2026-09-21 19:00:00")[1]
       - window_bounds("2026-09-21 19:00:00", "2026-09-21 19:00:00")[0], 999)
    try:
        window_bounds("2026-09-21 19:00:00", "2026-09-21 18:00:00")
        ok("a backwards window raises", False, True)
    except ValueError:
        ok("a backwards window raises", True, True)

    bad_checks = [c for c in checks if not c[1]]
    for name, good_, got, want in checks:
        if not good_:
            print("FAIL %-46s got %r, want %r" % (name, got, want))
    print("tb_swap_keys --self-test: %d checks, %d failed"
          % (len(checks), len(bad_checks)))
    return 1 if bad_checks else 0

