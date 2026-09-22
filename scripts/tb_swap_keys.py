#!/usr/bin/env python3
"""Exchanges two ThingsBoard telemetry keys over a closed past window.

WHY THIS EXISTS, AND WHY IT IS NOT "REWRITING HISTORY"

On 2026-09-21 two soil probes on the `esp-garden-hardware` carrier were found
to have been plugged into each other's terminals: the probe standing in the
Zona 3 pot was in the terminal the firmware reads as index 0. The netlist and
the firmware were never wrong -- CLAUDE.md's 2026-09-21 entry establishes that
from `production/netlist.ipc` -- so for a known stretch of time ThingsBoard
filed each probe's readings under the OTHER probe's key.

THE VALUES ARE ALL CORRECT. ONLY THE LABEL IS WRONG. That distinction is the
whole licence for this tool and it deserves saying plainly, because the next
reader will otherwise conclude that this repository started editing its own
archive:

  * INTERPOLATING a gap invents a measurement nobody took. tb_import.py refuses
    to do it, reports every seam it finds and closes none of them, and this
    tool does not change that rule by one word.
  * RELABELLING moves a measurement somebody DID take from the wrong name to
    the right one. Nothing is created, nothing is destroyed, and the set of
    (value, timestamp) pairs on the device is bit-for-bit the same afterwards.
    It is a permutation of names, and it is its own inverse.

The second is what happens here, and the tool is built so that the claim is
checkable rather than asserted: the operation is a permutation, applying it
twice restores the original exactly, and `--probe` proves that on a throwaway
device before anybody points it at the garden.

WHAT IT IS AFRAID OF

The same thing tb_import.py is afraid of: a partial write that reports success.
So the verdict is never an HTTP status. Both keys are COUNTED on both sides and
a sample of values -- first, last, middle, and the LONGEST literal each key ever
held -- is fetched back and compared to the pre-swap text CHARACTER BY
CHARACTER.

THE TYPING RULE IS THE SAME ONE THE MIGRATION HIT, AND IT IS THE SUBTLE PART

ThingsBoard types a datapoint from the JSON literal the publisher sent: `0` is a
long, `66.62857055664062` a double, and `0.47609522938728333` -- seventeen
significant digits -- is a STRING. On this device 764 of 909 `moisture1Sd`
points are in that third class. So a value is read back NON-STRICT, which
returns the server's own stringification, and re-emitted VERBATIM as a raw JSON
literal through `tb_client.json_literal()`. Parsing to a Python float and
formatting it back would shorten 17 digits to 16 and silently turn a string
series into a double one, part-way through, with nothing in the data to say
where -- the same defect class as renumbering a relay index.

That is also what makes character identity the right verification: a 17-digit
literal that had been re-typed as a double would read back 16 digits long.

WHY THERE IS NO "DO NOT RUN IT TWICE" FLAG

A write to the same (entity, key, ts) is an OVERWRITE on this instance, measured
rather than cited (`--probe` re-measures it). So `--apply` re-reads the window
first and classifies every timestamp as PENDING (it still holds the pre-swap
text), DONE (it already holds the swapped text) or FOREIGN (neither). It writes
only the pending ones and refuses outright on any foreign timestamp. Running
`--apply` twice therefore writes nothing the second time, which is a stronger
guarantee than a flag somebody can pass by accident -- and it is what makes a
run that dies half way simply re-runnable.

WHAT IT REFUSES

  * a timestamp outside the window;
  * a key set that is not the FULL set of pairs -- swapping a mean without its
    `Now` and `Sd` twins leaves the record inconsistent rather than merely
    wrong, which is worse than leaving it alone;
  * a timestamp present for one key of a pair and not the other; the count is
    reported and no point is invented;
  * a window boundary the stored series does not corroborate. The END of a
    crossed window is a CROSSOVER: the two series must exchange levels there.
    If they do not, the boundary is in the wrong place, and swapping past it
    un-corrects points that are already right. `--boundary-override` is the
    deliberate way past it and it has to be typed.
  * applying without a stored plan, because the pre-swap text is the only
    record of what the series held, and one written point destroys it.

WHERE THE CODE IS

`scripts/tb_swap_model.py` holds the permutation, every refusal, the boundary
evidence and the plan file; this file holds the session, the commands and the
printing. They were one file until it crossed the 1000-line gate, and the seam
is the one tb_import.py / tb_client.py and history_export.py /
history_archive.py already use. Nothing in the model file opens a socket, which
is the point: the two claims this correction is authorised on -- that it is a
clean permutation, and that the window's edges are where the record says they
are -- are settled by functions `--self-test` can call with no credential.

Run:  python scripts/tb_swap_keys.py --plan          # the DRY RUN. Default.
      python scripts/tb_swap_keys.py --probe         # throwaway device, 4 facts
      python scripts/tb_swap_keys.py --apply         # the only writing command
      python scripts/tb_swap_keys.py --verify        # count + compare, per key
      python scripts/tb_swap_keys.py --self-test     # offline, no credential

The credential is read from a file inside this process, never printed and never
put on a command line -- the rule tb_export.py states and tb_import.py repeats.
Default `<repo>/.chave_tb_selfhosted`, overridden with TB_IMPORT_KEY_FILE.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from tb_client import ApiError, Client, Fatal, batches, build_body
from tb_swap_model import (DEFAULT_FROM, DEFAULT_PAIRS, DEFAULT_TO,
                           EVIDENCE_SAMPLES, EVIDENCE_SETTLE,
                           alignment_refusal, boundary_refusal, cadence,
                           classify, crossover_evidence, fmt_ts,
                           gap_evidence, pair_refusal, plan_fingerprint,
                           plan_state, read_plan, replan_refusal,
                           sample_stamps, self_test,
                           swap_points, swap_state, window_bounds,
                           window_refusal, write_plan)

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_SERVER = "https://tb.espgarden.com.br"
DEFAULT_DEVICE = "espgarden-s3"
DEFAULT_KEY_FILE = ROOT / ".chave_tb_selfhosted"
DEFAULT_PLAN = ROOT / ".pio" / "tb_swap_keys.plan.json"

PROBE_DEVICE_PREFIX = "zz-swap-probe-"


# --------------------------------------------------------------------------
# Reading
# --------------------------------------------------------------------------

def read_window(client: Client, dev: str, keys, lower: int, upper: int) -> dict:
    """Every stored point for these keys in [lower, upper], NON-STRICT.

    Non-strict on purpose: it returns the server's own stringification, which
    is the text that must be re-emitted verbatim. A strict read hands back
    parsed JSON and the 17-digit strings would come back as Python floats,
    which is precisely the information this tool must not lose.
    """
    out = {}
    for key in keys:
        points = client.walk(dev, key, lower, upper + 1, strict=False)
        out[key] = {p["ts"]: p["value"] for p in points
                    if lower <= p["ts"] <= upper}
    return out


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------

def collect(client, dev, args, pairs):
    """The window, the evidence around its edges, and the refusals. No writes."""
    try:
        lower, upper = window_bounds(args.window_from, args.window_to)
    except ValueError as e:
        raise Fatal(str(e))
    keys_present = set(client.keys(dev))
    refusal = pair_refusal(pairs, keys_present)
    if refusal:
        raise Fatal(refusal)

    keys = [k for pair in pairs for k in pair]
    state = read_window(client, dev, keys, lower, upper)

    # Context either side, for the boundary test only. Three cadences of margin
    # plus the settle skip, and a whole day back so a hole in front of the
    # window is visible rather than being mistaken for the start of time.
    span = 86400 * 1000
    context = {}
    for a, b in pairs[:1]:
        context[a] = client.walk(dev, a, lower - span, upper + span)
        context[b] = client.walk(dev, b, lower - span, upper + span)
    return lower, upper, state, context


def evidence(pairs, state, context, lower, upper, samples, settle):
    a, b = pairs[0]
    ca = {p["ts"]: p["value"] for p in context[a]}
    cb = {p["ts"]: p["value"] for p in context[b]}
    stamps = sorted(ca)
    inside = [t for t in stamps if lower <= t <= upper]
    before = [t for t in stamps if t < lower]
    after = [t for t in stamps if t > upper]
    cad = cadence(stamps)

    start_ev = gap_evidence(before[-1] if before else None,
                            inside[0] if inside else lower, cad)
    start_ev["previous_ts"] = before[-1] if before else None
    start_ev["first_ts"] = inside[0] if inside else None
    start_ev["previous_values"] = [(t, ca.get(t), cb.get(t))
                                   for t in before[-samples:]]

    tail = inside[-samples:]
    far = after[settle:settle + samples]
    end_ev = crossover_evidence([ca.get(t) for t in tail],
                                [cb.get(t) for t in tail],
                                [ca.get(t) for t in far],
                                [cb.get(t) for t in far])
    end_ev["tail"] = tail
    end_ev["far"] = far
    end_ev["skipped"] = after[:settle]
    return start_ev, end_ev, cad


def cmd_plan(client, dev, args, pairs) -> int:
    lower, upper, state, context = collect(client, dev, args, pairs)
    keys = [k for pair in pairs for k in pair]
    counts = {k: len(state[k]) for k in keys}

    print("device      %s (%s) on %s" % (args.device, dev, args.server))
    print("window      %s .. %s   (inclusive, local)"
          % (fmt_ts(lower), fmt_ts(upper)))
    print("pairs       %s" % ", ".join("%s <-> %s" % p for p in pairs))
    print()
    print("%-16s %8s  %-22s %s" % ("key", "points", "longest literal", "span"))
    for k in keys:
        stamps = sorted(state[k])
        longest = max(state[k].values(), key=len) if stamps else ""
        print("%-16s %8d  %-22s %s .. %s"
              % (k, counts[k], longest,
                 fmt_ts(stamps[0]) if stamps else "-",
                 fmt_ts(stamps[-1]) if stamps else "-"))

    refusal = window_refusal(state, lower, upper)
    if refusal:
        print("\nREFUSED: %s" % refusal)
        return 1
    refusal = alignment_refusal(pairs, state)
    if refusal:
        print("\nREFUSED: %s" % refusal)
        return 1

    target = swap_state(state, pairs)
    total = sum(counts.values())
    stamps = sorted({t for k in keys for t in state[k]})

    start_ev, end_ev, cad = evidence(pairs, state, context, lower, upper,
                                     args.samples, args.settle)
    print("\n--- boundary evidence (median of %d samples, %d skipped after the "
          "end) ---" % (args.samples, args.settle))
    print("cadence     %.1f s between publishes" % cad)
    print("START  %s" % ("CORROBORATED" if start_ev["ok"] else "NOT CORROBORATED"))
    if start_ev.get("gap_sec") is not None:
        print("       the record has a %.1f min hole in front of the window "
              "(%.1fx the cadence)"
              % (start_ev["gap_sec"] / 60.0,
                 start_ev["gap_sec"] / cad if cad else 0))
    for t, va, vb in start_ev["previous_values"]:
        print("       before: %s  %s=%-10s %s=%s"
              % (fmt_ts(t), pairs[0][0], va, pairs[0][1], vb))
    print("END    %s" % ("CROSSOVER" if end_ev["ok"] else "NO CROSSOVER"))
    if end_ev.get("same") is not None:
        print("       inside  %s=%.2f  %s=%.2f   (last %d samples: %s)"
              % (pairs[0][0], end_ev["before_a"], pairs[0][1],
                 end_ev["before_b"], len(end_ev["tail"]),
                 ", ".join(fmt_ts(t)[11:] for t in end_ev["tail"])))
        print("       after   %s=%.2f  %s=%.2f   (skipping %s)"
              % (pairs[0][0], end_ev["after_a"], pairs[0][1],
                 end_ev["after_b"],
                 ", ".join(fmt_ts(t)[11:] for t in end_ev["skipped"]) or "none"))
        print("       same-name continuity %.2f, crossed continuity %.2f, "
              "margin %+.2f" % (end_ev["same"], end_ev["cross"],
                                end_ev["margin"]))

    print("\n--- what --apply would change ---")
    print("%d timestamps, %d datapoints, %d request(s)"
          % (len(stamps), total,
             sum(1 for _ in batches(swap_points(target, pairs, stamps)))))
    for a, b in pairs:
        sa, sb = sorted(state[a]), sorted(state[b])
        print("  %-14s %4d points -> filed under %-14s and back"
              % (a, len(sa), b))
    print("\nsamples (the four each key is verified on afterwards):")
    for a, b in pairs:
        for t in sample_stamps(sorted(state[a]), state[a]):
            print("  %s  %-14s %-22s -> %-22s"
                  % (fmt_ts(t), a, state[a][t], target[a][t]))
    body = build_body(swap_points(target, pairs, stamps))
    print("\nrequest bodies: %.1f KB" % (len(body) / 1024.0))

    refusal = boundary_refusal(start_ev, end_ev, args.boundary_override)
    if refusal:
        print("\nREFUSED: %s" % refusal)
        print("\nNOTHING HAS BEEN WRITTEN and no plan has been stored.")
        return 1

    fingerprint = plan_fingerprint(args.server, args.device, lower, upper,
                                   pairs, counts)
    existing = read_plan(Path(args.plan))
    refusal = replan_refusal(existing, fingerprint, args.replan)
    if refusal:
        print("\nREFUSED: %s" % refusal)
        return 1

    plan = {"fingerprint": fingerprint, "server": args.server,
            "device": args.device, "device_id": dev,
            "window": {"from_ms": lower, "to_ms": upper,
                       "from": fmt_ts(lower), "to": fmt_ts(upper)},
            "pairs": [list(p) for p in pairs], "counts": counts,
            "planned_at": fmt_ts(int(time.time() * 1000)),
            "applied": False, "applied_at": None,
            "before": {k: {str(t): v for t, v in state[k].items()}
                       for k in keys}}
    if args.write_plan:
        write_plan(Path(args.plan), plan)
        print("\nplan stored in %s (fingerprint %s)" % (args.plan, fingerprint))
    else:
        print("\nplan NOT stored (pass --write-plan). fingerprint %s"
              % fingerprint)
    print("Nothing has been written to %s. --apply is the only command that "
          "writes." % args.device)
    return 0


def cmd_apply(client, dev, args, pairs) -> int:
    plan = read_plan(Path(args.plan))
    if not plan:
        raise Fatal("no plan at %s. Run --plan --write-plan first: the "
                    "pre-swap text is the only record of what the series held, "
                    "and one written point destroys it." % args.plan)
    lower, upper = plan["window"]["from_ms"], plan["window"]["to_ms"]
    pairs = [tuple(p) for p in plan["pairs"]]
    keys = [k for pair in pairs for k in pair]
    if plan["device_id"] != dev or plan["server"] != args.server:
        raise Fatal("the stored plan is for device %s on %s, not %s on %s"
                    % (plan["device_id"], plan["server"], dev, args.server))

    before = plan_state(plan)
    fingerprint = plan_fingerprint(args.server, args.device, lower, upper,
                                   pairs, plan["counts"])
    if fingerprint != plan["fingerprint"]:
        raise Fatal("the stored plan's fingerprint does not match its own "
                    "contents (%s vs %s); it has been edited"
                    % (plan["fingerprint"], fingerprint))

    target = swap_state(before, pairs)
    current = read_window(client, dev, keys, lower, upper)
    verdict = classify(current, target, before, pairs)

    print("plan %s  %s .. %s" % (plan["fingerprint"], fmt_ts(lower),
                                 fmt_ts(upper)))
    print("%d pending, %d already swapped, %d identical either way, %d foreign"
          % (len(verdict["pending"]), len(verdict["done"]),
             len(verdict["identical"]), len(verdict["foreign"])))
    if verdict["foreign"]:
        for ts in verdict["foreign"][:5]:
            print("   foreign at %s: server has %s, plan recorded %s"
                  % (fmt_ts(ts),
                     {k: current.get(k, {}).get(ts) for k in keys},
                     {k: before.get(k, {}).get(ts) for k in keys}))
        raise Fatal("%d timestamp(s) hold neither the pre-swap text nor the "
                    "swapped text. Something else changed them and this tool "
                    "cannot tell which of the two it is looking at. Re-plan "
                    "against the current series, or fix them by hand."
                    % len(verdict["foreign"]))
    if not verdict["pending"]:
        print("\nNothing to write: every timestamp already holds the swapped "
              "text. The swap is a permutation and re-applying it is a no-op.")
        return 0
    if not args.yes:
        print("\nREFUSED: --apply needs --yes. This writes %d datapoints to a "
              "live device." % (len(verdict["pending"]) * len(keys)))
        return 1

    points = swap_points(target, pairs, verdict["pending"])
    plan_batches = list(batches(points))
    written, t0 = 0, time.time()
    path = "/api/plugins/telemetry/DEVICE/%s/timeseries/ANY" % dev
    for i, batch in enumerate(plan_batches):
        body = build_body(batch)
        client.call("POST", path, body)
        written += sum(len(v) for _, v in batch)
        print("  batch %d/%d  %d points  %.1f KB  %s..%s"
              % (i + 1, len(plan_batches), written, len(body) / 1024.0,
                 fmt_ts(batch[0][0]), fmt_ts(batch[-1][0])))
    plan["applied"] = True
    plan["applied_at"] = fmt_ts(int(time.time() * 1000))
    write_plan(Path(args.plan), plan)
    print("\nwrote %d datapoints in %.1f s. Nothing is proved yet: run --verify."
          % (written, time.time() - t0))
    return 0


def cmd_verify(client, dev, args, pairs) -> int:
    """Count both sides per key, then compare values character by character."""
    plan = read_plan(Path(args.plan))
    if not plan:
        raise Fatal("no plan at %s; there is nothing to verify against. The "
                    "pre-swap text lives only there." % args.plan)
    lower, upper = plan["window"]["from_ms"], plan["window"]["to_ms"]
    pairs = [tuple(p) for p in plan["pairs"]]
    keys = [k for pair in pairs for k in pair]
    before = plan_state(plan)
    target = swap_state(before, pairs)
    current = read_window(client, dev, keys, lower, upper)

    print("every point is compared, not a sample; `match` is exhaustive.")
    print("%-16s %9s %9s %9s" % ("key", "planned", "on server", "match"))
    bad = 0
    for k in keys:
        want, got = target[k], current.get(k, {})
        agree = sum(1 for t, v in want.items() if got.get(t) == v)
        flag = "" if agree == len(want) == len(got) else "   MISMATCH"
        if flag:
            bad += 1
        print("%-16s %9d %9d %9d%s" % (k, len(want), len(got), agree, flag))

    print("\nthe four samples each key is judged on -- first, last, middle, and\n"
          "the LONGEST literal, whose type hangs on a digit -- printed in full:")
    mism = 0
    for a, b in pairs:
        for src, dst in ((a, b), (b, a)):
            for t in sample_stamps(sorted(before[src]), before[src]):
                was, now = before[src][t], current.get(dst, {}).get(t)
                mark = "ok" if was == now else "DIFFERENT"
                if was != now:
                    mism += 1
                    print("   %s %s(before)=%r -> %s(now)=%r  %s"
                          % (fmt_ts(t), src, was, dst, now, mark))
                else:
                    print("   %s %s=%-22s -> %s  ok" % (fmt_ts(t), src, was, dst))

    # The whole point, stated as a check rather than as prose.
    same_multiset = (sorted((t, v) for k in keys for t, v in before[k].items())
                     == sorted((t, v) for k in keys
                               for t, v in current.get(k, {}).items()))
    if same_multiset:
        print("\nthe (timestamp, value) multiset across these keys is IDENTICAL "
              "before and after,\nwhich is the arithmetic of the word "
              "relabelling: nothing created, nothing destroyed.")
    else:
        print("\nthe (timestamp, value) multiset across these keys CHANGED. A "
              "relabelling cannot\ndo that, so something other than this swap "
              "has touched the window.")
    if bad or mism or not same_multiset:
        print("\nSOMETHING DISAGREES.")
        return 1
    print("\nEvery key agrees on both sides, every point matches, and every "
          "sampled value is\ncharacter-identical under its new name.")
    return 0


def cmd_probe(client, args) -> int:
    """The four facts this tool rests on, measured on a device it then deletes.

    Nothing here touches the garden. A citation is not a measurement, and the
    self-inverse property in particular is the cheapest possible proof that the
    operation is a clean permutation.
    """
    name = PROBE_DEVICE_PREFIX + str(int(time.time()))
    dev = client.call("POST", "/api/device",
                      json.dumps({"name": name, "type": "default"}).encode())
    pid = dev["id"]["id"]
    print("probe device %s (%s)" % (name, pid))
    path = "/api/plugins/telemetry/DEVICE/%s/timeseries/ANY" % pid
    ok = True
    try:
        base = 1789754695000  # 2026-09-18 15:04:55, this window's own start

        client.call("POST", path, build_body([(base, [("p", "1.0")])]))
        back = client.series(pid, "p", base - 1, base + 1)
        print("1. backdated write      -> %d point(s)  %s"
              % (len(back), "ACCEPTED" if back else "REFUSED"))
        ok &= bool(back)

        client.call("POST", path, build_body([(base, [("p", "2.0")])]))
        client.call("POST", path, build_body([(base, [("p", "3.0")])]))
        back = client.series(pid, "p", base - 1, base + 1)
        print("2. rewritten twice      -> %d point(s), value %r  %s"
              % (len(back), back[0]["value"] if back else None,
                 "OVERWRITE" if len(back) == 1 else "DUPLICATED"))
        ok &= len(back) == 1 and back[0]["value"] == "3.0"

        # 3. The typing round trip, on the six shapes this archive holds --
        #    including the 17-digit literal ThingsBoard stores as a STRING and
        #    the one this device's own moisture1Sd is full of.
        shapes = ["0", "-3", "66.62857055664062", "0.47609522938728333",
                  "true", "Zona \"1\"", "84.47058868408203",
                  "0.062019046396017075"]
        pts = [(base + 10 + i, [("t", s)]) for i, s in enumerate(shapes)]
        client.call("POST", path, build_body(pts))
        got = {p["ts"]: p["value"]
               for p in client.walk(pid, "t", base + 9, base + 20)}
        bad = [(s, got.get(ts)) for (ts, _), s in zip(pts, shapes)
               if got.get(ts) != s]
        print("3. typing round trip    -> %d/%d character-identical  %s"
              % (len(shapes) - len(bad), len(shapes),
                 "ok" if not bad else "MISMATCH %r" % bad))
        ok &= not bad

        # 4. The self-inverse property, against the real server: seed a pair,
        #    swap, check it crossed, swap again, check it is bit-for-bit what
        #    it started as. This is the property that makes the correction
        #    safe to authorise, and it is measured, not argued.
        pairs = [("a", "b")]
        seed = {"a": {base + 100 + i: "%d.5" % i for i in range(20)},
                "b": {base + 100 + i: "0.4760952293872833%d" % (i % 10)
                      for i in range(20)}}
        stamps = sorted(seed["a"])
        client.call("POST", path, build_body(swap_points(seed, pairs, stamps)))
        start = read_window(client, pid, ["a", "b"], base + 100, base + 200)
        once = swap_state(start, pairs)
        client.call("POST", path, build_body(swap_points(once, pairs, stamps)))
        after1 = read_window(client, pid, ["a", "b"], base + 100, base + 200)
        crossed = (after1["a"] == start["b"] and after1["b"] == start["a"])
        twice = swap_state(after1, pairs)
        client.call("POST", path, build_body(swap_points(twice, pairs, stamps)))
        after2 = read_window(client, pid, ["a", "b"], base + 100, base + 200)
        restored = after2 == start
        print("4. swap once            -> %s" % ("CROSSED" if crossed else "DID NOT CROSS"))
        print("   swap twice           -> %s"
              % ("IDENTICAL to the original" if restored else "NOT restored"))
        ok &= crossed and restored
    finally:
        client.call("DELETE", "/api/device/" + pid)
        print("probe device deleted")
    print("\n%s" % ("all four hold" if ok else "SOMETHING DID NOT HOLD"))
    return 0 if ok else 1


# --------------------------------------------------------------------------

# --------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--server", default=DEFAULT_SERVER)
    p.add_argument("--device", default=DEFAULT_DEVICE)
    p.add_argument("--key-file", default=str(DEFAULT_KEY_FILE))
    p.add_argument("--plan-file", dest="plan", default=str(DEFAULT_PLAN))
    p.add_argument("--from", dest="window_from", default=DEFAULT_FROM,
                   help="first instant to swap, local, inclusive")
    p.add_argument("--to", dest="window_to", default=DEFAULT_TO,
                   help="last instant to swap, local, INCLUSIVE of the whole "
                        "of that second -- a publish lands at .373, not .000")
    p.add_argument("--pair", action="append", metavar="A:B",
                   help="a key pair to exchange; repeatable. Defaults to the "
                        "three moisture1/moisture3 pairs")
    p.add_argument("--samples", type=int, default=EVIDENCE_SAMPLES,
                   help="samples either side of a boundary the evidence test "
                        "takes the median of")
    p.add_argument("--settle", type=int, default=EVIDENCE_SETTLE,
                   help="samples skipped just after the end boundary, where a "
                        "hand is still on the hardware")
    p.add_argument("--boundary-override", action="store_true",
                   help="proceed although the series does not corroborate the "
                        "window's edges")
    p.add_argument("--replan", action="store_true",
                   help="overwrite a plan already marked applied")
    p.add_argument("--write-plan", action="store_true",
                   help="store the plan; --plan alone prints and stores "
                        "nothing")
    p.add_argument("--yes", action="store_true",
                   help="required by --apply; without it --apply is a report")
    p.add_argument("--plan", dest="do_plan", action="store_true")
    p.add_argument("--apply", action="store_true")
    p.add_argument("--verify", action="store_true")
    p.add_argument("--probe", action="store_true")
    p.add_argument("--self-test", action="store_true")
    args = p.parse_args()

    if args.self_test:
        return self_test()

    pairs = DEFAULT_PAIRS
    if args.pair:
        pairs = []
        for spec in args.pair:
            if spec.count(":") != 1:
                raise Fatal("a pair is written A:B, not %r" % spec)
            a, b = spec.split(":")
            pairs.append((a.strip(), b.strip()))

    if not (args.do_plan or args.apply or args.verify or args.probe):
        args.do_plan = True  # the dry run is the default, and it writes nothing

    client = Client(args.server, Path(args.key_file))
    client.login()
    me = client.whoami()
    print("%s as %s (%s)\n" % (args.server, me.get("email"),
                               me.get("authority")))
    if args.probe:
        return cmd_probe(client, args)

    dev = client.device_by_name(args.device)["id"]
    if args.do_plan:
        return cmd_plan(client, dev, args, pairs)
    if args.apply:
        return cmd_apply(client, dev, args, pairs)
    if args.verify:
        return cmd_verify(client, dev, args, pairs)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fatal as e:
        print("tb_swap_keys: %s" % e, file=sys.stderr)
        sys.exit(1)
    except ApiError as e:
        print("tb_swap_keys: %s" % e, file=sys.stderr)
        sys.exit(1)
