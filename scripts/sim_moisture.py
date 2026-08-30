"""The ported Gaussian moisture model, the scenario table that stands in for a
training run, and the /moisture.json payload built from them.

Mirrors src/moisture_classifier.cpp, src/moisture_model.cpp and
src/web_moisture.cpp. Split out of dev_server.py, which crossed the 1000-line
limit scripts/check_lines.py enforces.
"""

from __future__ import annotations

import math

from sim_config import SIM_CONFIG


# ---------------------------------------------------------------------------
# Moisture model -- mirror of src/moisture_classifier.cpp, src/moisture_model.cpp
# and src/web_moisture.cpp.
# ---------------------------------------------------------------------------
#
# The arithmetic is a port, not an approximation. The priors are
# weight / total weight, the separation is Fisher's
# J = (mu_wet - mu_dry)^2 / (var_wet + var_dry), and the confidence is the
# softmax of the log posteriors normalised against the winner. Getting any of
# those subtly different is exactly the drift this file exists to prevent: the
# page would render numbers no device ever produces, and it would look right.
#
# WHAT IS DELIBERATELY NOT MIRRORED: the training run. Fitting the model from
# the simulator's seeded history would put all three classes on the same mean,
# because the seeded moisture curve is a sinusoid unrelated to the seeded relay
# firings -- every probe would land on "bands overlap" and the states that
# matter (a classified probe, a probe short of watering events) would be
# unreachable off hardware, which is the one thing the simulator is for. So the
# SUFFICIENT STATISTICS come from a deterministic per-probe scenario table, and
# every number derived from them goes through the ported arithmetic below.
#
# Which probes exist, what they are called, which relay feeds them and their
# two-point anchors all come from SIM_CONFIG, so a probe deleted or renamed in
# /devices.html disappears or is renamed here too.

MOISTURE_MAX = 4  # BuildConfig.h

GAUSSIAN_VARIANCE_FLOOR = 0.01        # g_gaussianVarianceFloor
MOISTURE_MIN_WEIGHT_PER_CLASS = 20.0  # g_moistureMinWeightPerClass
MOISTURE_MIN_SEPARATION = 4.0         # g_moistureMinSeparation
MOISTURE_MIN_EVENTS = 6               # g_moistureMinEvents
MOISTURE_DECAY_PER_RUN = 0.93         # g_moistureDecayPerRun

# The JSON keys web_moisture.cpp writes, in its order, and moistureClassName().
MOISTURE_CLASSES = ("dry", "humid", "wet")
MOISTURE_CLASS_NAMES = {"dry": "Dry", "humid": "Humid", "wet": "Wet"}


def gaussian_stats(mean: float, sd: float, weight: float) -> dict:
    """GaussianStats from readable parameters.

    The scenario table is written as (mean, sd, weight) because that is what a
    person can check; the payload has to carry the sufficient statistics the
    device carries, so everything downstream reads them back out through
    gaussian_mean()/gaussian_variance() rather than trusting the inputs.
    """
    if weight <= 0.0:
        return {"weight": 0.0, "sum": 0.0, "sumSq": 0.0}
    return {
        "weight": float(weight),
        "sum": float(mean) * weight,
        "sumSq": (float(sd) * sd + float(mean) * mean) * weight,
    }


def gaussian_mean(stats: dict) -> float:
    if stats["weight"] <= 0.0:
        return 0.0
    return stats["sum"] / stats["weight"]


def gaussian_variance(stats: dict) -> float:
    if stats["weight"] <= 0.0:
        return GAUSSIAN_VARIANCE_FLOOR
    mean = stats["sum"] / stats["weight"]
    variance = (stats["sumSq"] / stats["weight"]) - (mean * mean)
    # Written as `not (v > floor)` for the same reason the firmware is: it also
    # catches the NaN that catastrophic cancellation can produce.
    if not (variance > GAUSSIAN_VARIANCE_FLOOR):
        variance = GAUSSIAN_VARIANCE_FLOOR
    return variance


def moisture_separation(classes: dict) -> float:
    dry, wet = classes["dry"], classes["wet"]
    if dry["weight"] <= 0.0 or wet["weight"] <= 0.0:
        return 0.0
    delta = gaussian_mean(wet) - gaussian_mean(dry)
    pooled = gaussian_variance(wet) + gaussian_variance(dry)
    if pooled <= 0.0:
        return 0.0
    return (delta * delta) / pooled


def moisture_model_is_usable(classes: dict, min_weight: float,
                             min_separation: float) -> bool:
    for key in MOISTURE_CLASSES:
        if classes[key]["weight"] < min_weight:
            return False
    if moisture_separation(classes) < min_separation:
        return False
    # Humid must lie BETWEEN dry and wet, in either direction: the probe's
    # polarity is not assumed here any more than it is in the firmware.
    dry = gaussian_mean(classes["dry"])
    humid = gaussian_mean(classes["humid"])
    wet = gaussian_mean(classes["wet"])
    return (dry < humid < wet) or (dry > humid > wet)


def moisture_classify(classes: dict, value, min_weight: float,
                      min_separation: float):
    """Returns (class key or None, confidence). Mirrors moistureClassify()."""
    if value is None or not math.isfinite(value):
        return None, 0.0
    if not moisture_model_is_usable(classes, min_weight, min_separation):
        return None, 0.0

    total_weight = sum(classes[k]["weight"] for k in MOISTURE_CLASSES)
    if total_weight <= 0.0:
        return None, 0.0

    # Log domain throughout, as on the device: these likelihoods differ by many
    # orders of magnitude and computing them directly underflows every class to
    # zero at once, which reads as a tie.
    log_posterior = {}
    best = None
    for key in MOISTURE_CLASSES:
        prior = classes[key]["weight"] / total_weight
        mean = gaussian_mean(classes[key])
        variance = gaussian_variance(classes[key])
        delta = value - mean
        log_posterior[key] = (math.log(prior)
                              - 0.5 * math.log(2.0 * math.pi * variance)
                              - (delta * delta) / (2.0 * variance))
        # Strictly greater, so a tie keeps the earlier class -- the firmware
        # loop breaks ties the same way, and dry/humid/wet is its index order.
        if best is None or log_posterior[key] > log_posterior[best]:
            best = key

    # Softmax normalised against the winner: the largest exponent is exp(0) = 1
    # and nothing overflows.
    total = sum(math.exp(log_posterior[k] - log_posterior[best])
                for k in MOISTURE_CLASSES)
    return best, (1.0 / total if total > 0.0 else 0.0)


# --- what the device would hold after a training run -----------------------
#
# Deterministic per-probe scenarios, NOT a fitted model. Each is the state a
# probe's MoistureProbeModel would be in, so everything the endpoint reports is
# computed from them the way web_moisture.cpp computes it.
MOISTURE_SCENARIOS = {
    # Well separated and ordered: J = 34^2 / (6.25 + 6.25) = 92.5, far above the
    # gate, so this probe classifies with a high posterior.
    "good": {
        "classes": {"dry": (28.0, 2.5, 480.0),
                    "humid": (45.0, 3.0, 360.0),
                    "wet": (62.0, 2.5, 240.0)},
        "events": 14,
        "response": 33.0,
        "tau": 420.0,
    },
    # Ordered and well fed with evidence, but the bands sit on top of each
    # other: J = 4^2 / (36 + 36) = 0.22. Refused on separation.
    "overlap": {
        "classes": {"dry": (41.0, 6.0, 400.0),
                    "humid": (43.0, 6.5, 380.0),
                    "wet": (45.0, 6.0, 300.0)},
        "events": 11,
        # 45 - 41: the response follows from the class means, and inventing a
        # smaller one would send this scenario down the "does not respond"
        # branch and stop it exercising the separation gate it exists for.
        "response": 4.0,
        "tau": 540.0,
    },
    # A probe that answers nothing: watered eleven times and the wet readings
    # sit where the dry ones do. Disconnected, in the wrong pot, or downstream
    # of a pump that never runs. The device says so BEFORE any statistical
    # gate, because this names a cause a person can act on; separation would
    # refuse the same probe days later and name only the symptom.
    "noResponse": {
        "classes": {"dry": (52.6, 0.1, 400.0),
                    "humid": (52.6, 0.1, 380.0),
                    "wet": (52.7, 0.1, 300.0)},
        "events": 11,
        "response": 0.1,
        "tau": 0.0,
    },
    # A model that would pass every statistical gate, fitted to three watering
    # cycles. Refused on the event count, which is the gate that says "this
    # describes those three cycles, not this soil".
    "fewEvents": {
        "classes": {"dry": (30.0, 2.5, 90.0),
                    "humid": (46.0, 3.0, 70.0),
                    "wet": (60.0, 2.5, 50.0)},
        "events": 3,
        "response": 29.0,
        "tau": 300.0,
    },
    # Humid outside the dry..wet interval: the labels disagree with the physics
    # that produced them. Reachable only with ?scenario=unordered.
    "unordered": {
        "classes": {"dry": (30.0, 2.5, 300.0),
                    "humid": (66.0, 3.0, 300.0),
                    "wet": (58.0, 2.5, 300.0)},
        "events": 14,
        "response": 27.0,
        "tau": 600.0,
    },
    # Separated, ordered, enough events -- but the humid class has almost no
    # accumulated weight. The device refuses it (moistureModelIsUsable checks
    # the per-class weight first) and then reports "class means are not
    # ordered", which is the wrong reason: see the note in moisture_snapshot().
    "lowWeight": {
        "classes": {"dry": (28.0, 2.5, 300.0),
                    "humid": (45.0, 3.0, 12.0),
                    "wet": (62.0, 2.5, 300.0)},
        "events": 14,
        "response": 33.0,
        "tau": 480.0,
    },
    # Nothing accumulated: a fresh probe, or one whose relay is -1.
    "empty": {
        "classes": {"dry": (0.0, 0.0, 0.0),
                    "humid": (0.0, 0.0, 0.0),
                    "wet": (0.0, 0.0, 0.0)},
        "events": 0,
        "response": 0.0,
        "tau": 0.0,
    },
}

# Which scenario each probe gets by default, so the four states worth looking at
# are all on the page at once without touching a query string. Probe 3 is
# overridden to "empty" anyway because SIM_CONFIG gives it relay -1.
DEFAULT_PROBE_SCENARIOS = ("good", "overlap", "fewEvents", "good")

# Forces every probe onto one scenario, or "untrained" for a device that has
# never run training. Set by --moisture-scenario; ?scenario= overrides it for a
# single request without changing it.
MOISTURE_SCENARIO = ""


def moisture_scenario_names() -> list:
    return sorted(MOISTURE_SCENARIOS) + ["untrained"]


def probe_nodes() -> list:
    """io.soilMoisture in every shape loadSoilMoisture() accepts.

    Array of {pin, name}, array of bare pins, or -- pre-2.0 -- a single bare
    pin. A device in the field has to survive a firmware update, so all three
    keep working here too.
    """
    probes = SIM_CONFIG.get("io", {}).get("soilMoisture")
    if isinstance(probes, list):
        return probes[:MOISTURE_MAX]
    if probes is None:
        return []
    return [probes]


def probe_names() -> list:
    """config.soilMoistureName[], mirroring loadSensor() + applySingleProbeLabel."""
    nodes = probe_nodes()
    names = []
    for i, node in enumerate(nodes):
        default = "Soil Moisture " + str(i + 1)
        name = node.get("name") if isinstance(node, dict) else None
        # An absent or EMPTY name keeps the default rather than blanking the
        # label the dashboard renders.
        names.append(name if isinstance(name, str) and name else default)
    # With exactly one probe and no name given, the label keeps its old spelling
    # -- it is the /data.json key dashboards have read for years.
    if len(names) == 1 and names[0] == "Soil Moisture 1":
        names[0] = "Soil Moisture"
    return names


def relay_count() -> int:
    io_cfg = SIM_CONFIG.get("io", {})
    relays = io_cfg.get("relays")
    if isinstance(relays, list):
        return len(relays)
    return 1 if "watering" in io_cfg else 0


def moisture_relays() -> list:
    """config.moistureRelay[], including probeRelay()'s validation."""
    entries = SIM_CONFIG.get("moisture")
    relays = []
    count = relay_count()
    for i in range(len(probe_nodes())):
        # The compiled default is probe i fed by relay i.
        relay = i
        entry = entries[i] if (isinstance(entries, list) and i < len(entries)
                               and isinstance(entries[i], dict)) else None
        if entry is not None and "relay" in entry:
            try:
                value = int(entry["relay"])
            except (TypeError, ValueError):
                value = None
            # Out of range is IGNORED, not clamped: loadFile() logs and keeps
            # the previous value.
            if value is not None and -1 <= value < count:
                relay = value
        # A default pointing past the relays this board actually has is dropped,
        # or every reading would be labelled against an event that never fires.
        if relay >= count:
            relay = -1
        relays.append(relay)
    return relays


def moisture_calibration() -> list:
    """config.moistureDry[] / moistureWet[]."""
    entries = SIM_CONFIG.get("moisture")
    out = []
    for i in range(len(probe_nodes())):
        entry = entries[i] if (isinstance(entries, list) and i < len(entries)
                               and isinstance(entries[i], dict)) else {}

        def number(key, node=entry):
            try:
                return float(node.get(key, 0.0))
            except (TypeError, ValueError):
                return 0.0

        out.append({"dry": number("dry"), "wet": number("wet")})
    return out


def resolve_scenario(scenario: str = None) -> str:
    """Which scenario this request runs under.

    ?scenario= when it names one, otherwise whatever --moisture-scenario set,
    otherwise "" -- the per-probe table. An unknown name is ignored rather than
    answered with a 400: the device has no such parameter at all, so there is no
    firmware behaviour to mirror, and a typo should not look like an outage.
    """
    if scenario and scenario != "untrained" and scenario not in MOISTURE_SCENARIOS:
        scenario = ""  # an unknown name is treated as if it had not been given
    return MOISTURE_SCENARIO if not scenario else scenario


def moisture_models(forced: str = "") -> list:
    """One MoistureProbeModel per configured probe, gates already evaluated.

    `forced` is an ALREADY-RESOLVED scenario name: resolving it here as well
    would let a request whose ?scenario= was rejected still pick up the CLI
    flag, so the payload's header and its probes would describe different runs.
    """
    relays = moisture_relays()
    models = []
    for i, relay in enumerate(relays):
        if forced == "untrained":
            name = "empty"
        elif forced:
            name = forced
        elif i < len(DEFAULT_PROBE_SCENARIOS):
            name = DEFAULT_PROBE_SCENARIOS[i]
        else:
            name = "good"
        # moistureModelTrain() skips a probe with no relay before folding
        # anything in, so no statistics ever accumulate for it.
        if relay < 0:
            name = "empty"

        spec = MOISTURE_SCENARIOS[name]
        classes = {k: gaussian_stats(*spec["classes"][k])
                   for k in MOISTURE_CLASSES}
        events = spec["events"]
        usable = (relay >= 0 and events >= MOISTURE_MIN_EVENTS
                  and moisture_model_is_usable(classes,
                                               MOISTURE_MIN_WEIGHT_PER_CLASS,
                                               MOISTURE_MIN_SEPARATION))
        models.append({
            "scenario": name,
            "classes": classes,
            "wateringEvents": events,
            "separation": moisture_separation(classes),
            "response": spec["response"],
            "tauSec": spec["tau"],
            "usable": usable,
        })
    return models


def moisture_model_classify(model: dict, value):
    """Mirrors moistureModelClassify(): the stored usable flag gates the call."""
    if not model["usable"]:
        return None, 0.0
    return moisture_classify(model["classes"], value,
                             MOISTURE_MIN_WEIGHT_PER_CLASS,
                             MOISTURE_MIN_SEPARATION)


def moisture_state(index: int, value: float, models: list = None) -> str:
    """Mirrors moistureState() in src/sensors.cpp.

    The trained model first, the two-point anchors second, nothing third -- and
    "nothing" is a real answer: an uncalibrated probe shows no badge rather than
    a fabricated one.
    """
    models = moisture_models(resolve_scenario()) if models is None else models
    if index >= len(models):
        return ""

    key, _ = moisture_model_classify(models[index], value)
    if key is not None:
        return MOISTURE_CLASS_NAMES[key]

    calibration = moisture_calibration()[index]
    dry, wet = calibration["dry"], calibration["wet"]
    span = wet - dry
    if abs(span) < 1e-3:
        return ""

    # Ordering is not assumed, exactly as in the firmware.
    fraction = min(1.0, max(0.0, (value - dry) / span))
    if fraction < 1.0 / 3.0:
        return "Dry"
    if fraction < 2.0 / 3.0:
        return "Humid"
    return "Wet"


def moisture_snapshot(scenario: str = None) -> dict:
    """GET /moisture.json -- mirrors handleMoistureJson() in src/web_moisture.cpp."""
    # Imported here rather than at module scope: sim_state imports this module
    # for the probe accessors and the model, so a top-level import back would
    # be a cycle.
    from sim_state import STATE

    forced = resolve_scenario(scenario)
    models = moisture_models(forced)
    names = probe_names()
    relays = moisture_relays()
    calibration = moisture_calibration()

    with STATE.lock:
        scanned = len(STATE.history)
        readings = []
        for i in range(len(models)):
            sensor = STATE.moisture_sensor(i)
            readings.append(sensor.average if sensor is not None else None)

    if forced == "untrained":
        # trainedAt == 0 is the device that has never run training: the
        # last-run counters are zero because there has been no last run.
        trained_at, scanned = 0, 0
    else:
        # Backdated so the page's "N h ago" branch is exercised; the real device
        # trains once a day.
        trained_at = int(STATE.boot_time - 5 * 3600)

    labelled = sum(1 for relay in relays if relay >= 0)
    # Illustrative counts, derived from the buffer rather than hardcoded so they
    # move when history.records does. On the device these come from the last run
    # only, and count samples across every probe.
    samples = int(scanned * 0.62) * labelled
    outliers = int(samples * 0.004)

    payload = {
        "trainedAt": trained_at,
        "recordsScanned": scanned,
        "samplesUsed": samples,
        "outliersDropped": outliers,
        # Sent rather than hardcoded in the page, so a reader can see WHY a
        # probe reports nothing.
        "gates": {
            "minWeightPerClass": MOISTURE_MIN_WEIGHT_PER_CLASS,
            "minSeparation": MOISTURE_MIN_SEPARATION,
            "minEvents": MOISTURE_MIN_EVENTS,
            "decayPerRun": MOISTURE_DECAY_PER_RUN,
        },
        "probes": [],
    }

    for i, model in enumerate(models):
        classes = model["classes"]
        total_weight = sum(classes[k]["weight"] for k in MOISTURE_CLASSES)

        probe = {
            "index": i,
            "name": names[i],
            "relay": relays[i],
            "usable": model["usable"],
            "separation": model["separation"],
            "wateringEvents": model["wateringEvents"],
            "response": model["response"],
            "tauSec": model["tauSec"],
            # The settling test: how much of the previous ADC channel this pin
            # carries into its first conversion. A real sensor is a stiff
            # source and couples ~0; a floating pin is dragged. Probe 1 is the
            # unplugged one here, so the page is exercised against a board that
            # has one of each.
            "health": {
                "verdict": ("noisy" if i == 2 else
                            ("floating" if i == 1 else "connected")),
                "sd": 1830.0 if i == 2 else 6.0,
                "couplingSlope": 0.31 if i == 1 else 0.004,
                "t": 44.0 if i == 1 else 0.6,
                "samples": 600,
            },
            "classes": {
                key: {
                    "weight": classes[key]["weight"],
                    "mean": gaussian_mean(classes[key]),
                    "sd": math.sqrt(gaussian_variance(classes[key])),
                    "prior": (classes[key]["weight"] / total_weight
                              if total_weight > 0.0 else 0.0),
                }
                for key in MOISTURE_CLASSES
            },
        }

        reading = readings[i]
        sampled = reading is not None
        if sampled:
            probe["reading"] = reading

        key, confidence = (moisture_model_classify(model, reading)
                           if sampled else (None, 0.0))
        if key is not None:
            probe["inferred"] = MOISTURE_CLASS_NAMES[key]
            probe["confidence"] = confidence
            probe["source"] = "model"
        else:
            # Which gate refused it. "No badge" with no reason is what makes a
            # classifier impossible to debug from the outside.
            #
            # The ORDER is the contract, not just the set of branches. The
            # response check comes before every statistical gate because it
            # names a physical cause -- probe out of the pot, pump not running
            # -- where separation would report "bands overlap" days later and
            # name only the symptom. Keep this chain in step with
            # handleMoistureRequest(); an earlier version of this file was a
            # copy of an older firmware and reported the wrong gate for
            # ?scenario=lowWeight long after the device had stopped doing so.
            weakest = min(model["classes"][k]["weight"]
                          for k in MOISTURE_CLASSES)
            wiring = probe["health"]["verdict"]
            if not sampled:
                reason = "no reading from this probe yet"
            elif wiring == "floating":
                # Ahead of every statistical gate, exactly as on the device:
                # it names a physical cause instead of a symptom.
                reason = ("nothing appears to be connected: this pin follows "
                          "the ADC channel read before it (%.1f%% coupling), "
                          "which a real sensor does not"
                          % (probe["health"]["couplingSlope"] * 100.0))
            elif wiring == "noisy":
                reason = ("this reading swings %.0f ADC counts inside one "
                          "window, which soil cannot do - the sensor is not "
                          "measuring anything" % probe["health"]["sd"])
            elif wiring == "railed":
                reason = ("this pin sits at a rail: shorted, or nothing is "
                          "dividing the supply into it")
            elif wiring == "stuck":
                reason = ("this reading has not moved at all: the sensor is "
                          "driving a level but no longer measuring")
            elif model["wateringEvents"] >= 2 and abs(model["response"]) < 0.5:
                reason = ("this probe does not respond to its pump (rise of "
                          "%.2f across waterings): check the probe, the pot it "
                          "is in, and whether the pump runs" % model["response"])
            elif relays[i] < 0:
                reason = "no relay assigned: nothing labels this probe"
            elif trained_at == 0:
                # The message a fresh device sends for every probe, and the one
                # the simulator could not produce at all until now -- it jumped
                # straight to the event count and reported "only 0 of 6", a
                # sentence the device never says in that state. Anyone styling
                # moisture.js against the simulator would never have seen it.
                reason = "not trained yet"
            elif model["wateringEvents"] < MOISTURE_MIN_EVENTS:
                reason = ("only %d of %d watering events seen so far"
                          % (model["wateringEvents"], MOISTURE_MIN_EVENTS))
            elif weakest < MOISTURE_MIN_WEIGHT_PER_CLASS:
                reason = ("thinnest class has %.1f of the %d weight needed"
                          % (weakest, MOISTURE_MIN_WEIGHT_PER_CLASS))
            elif model["separation"] < MOISTURE_MIN_SEPARATION:
                reason = ("bands overlap: separation J=%.1f below %.1f"
                          % (model["separation"], MOISTURE_MIN_SEPARATION))
            else:
                reason = "class means are not ordered dry..humid..wet"
            probe["blockedBy"] = reason

            # The fallback the dashboard is actually showing, if any.
            fallback = moisture_state(i, reading, models) if sampled else ""
            probe["source"] = "two-point calibration" if fallback else "none"
            if fallback:
                probe["inferred"] = fallback

        probe["calibration"] = {"dry": calibration[i]["dry"],
                                "wet": calibration[i]["wet"]}
        payload["probes"].append(probe)

    return payload
