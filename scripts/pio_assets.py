"""PlatformIO pre-hook: regenerate the staging directory before it is packed,
and refuse to pack a config.json written for a different board.

`data_dir` points the filesystem image away from data/, so the device gets the
bundled, gzipped build while data/ keeps the plain sources. That only works if
the staging directory is current, and "remember to run the script first" is not
a mechanism: forgetting it ships a stale image, or an empty one, and the failure
looks like a device that lost its web UI.

The output path comes from $PROJECT_DATA_DIR rather than from build_assets.py's
own default, so the location is declared once — in platformio.ini — instead of
in two places that can drift into packing one directory while writing another.

It runs ONLY for the filesystem targets. Sharing [env] means every environment
inherits this hook, including [env:native], and `pio test -e native` is the gate
CI runs before the firmware matrix: it needs no board, no data/config.json and
no gzip, and a build_assets failure there would fail the unit tests for a reason
that has nothing to do with them. `-t clean` would also have rebuilt the very
directory it was asked to remove.

Everything above the SCons block at the bottom is plain Python with no SCons
names in it, so it can be imported and exercised without a build.
"""

import json
import subprocess
import sys
from pathlib import Path

FS_TARGETS = {"buildfs", "uploadfs", "uploadfsota"}

# Keys under `io` whose VALUE is a GPIO number, wherever they appear.
_PIN_KEYS = {"pin", "powerPin"}

# `io` entries that ConfigFile also accepts as a bare pin number instead of an
# object, or as an array of bare numbers. Every other integer under `io` is a
# level, a count or a calibration constant — `on`, `powerOn`, `settleMs`,
# `activeLevel`, `fillRelay`, `pulsesPerLitre` — and must not be read as a pin.
_BARE_PIN_KEYS = {"watering", "button", "dht", "luminosity", "waterLevel",
                  "soilMoisture", "flow", "floatSwitch"}


def declared_pins(document):
    """Every GPIO the `io` block of `document` assigns, as {pin: json path}.

    Handles the several shapes ConfigFile::loadFile() accepts, because a
    document in the field has to keep loading across a firmware update and this
    check has to see the same pins the firmware will.
    """
    found = {}

    def visit(node, path, bare_ok):
        if isinstance(node, bool):
            return
        if isinstance(node, int):
            # Negatives are the UI's "none" — config_io.cpp turns a powerPin
            # below zero into kNoPin — so they are an absence of a pin, not a
            # pin some board might lack.
            if bare_ok and node >= 0:
                found.setdefault(node, path)
            return
        if isinstance(node, list):
            for i, item in enumerate(node):
                visit(item, "%s[%d]" % (path, i), bare_ok)
            return
        if isinstance(node, dict):
            for key, value in node.items():
                child = "%s.%s" % (path, key)
                if key in _PIN_KEYS:
                    visit(value, child, True)
                elif key in _BARE_PIN_KEYS:
                    visit(value, child, True)
                elif isinstance(value, (dict, list)):
                    visit(value, child, False)

    visit(document.get("io", {}), "io", False)
    return found


def config_guard(env_name, root):
    """Why this env must not pack data/config.json, or None if it may.

    THE HAZARD. The filesystem image carries whatever data/config.json holds,
    and this repo has exactly one of those. CI provisions it per board — it
    branches on the environment and copies templates/config.<env>.json where
    one exists — but nothing in the PlatformIO env, this hook or
    build_assets.py knew that, so `pio run -e espgarden_s3 -t buildfs` on a
    workstation produced an S3 image carrying the WROOM-32 document: relays on
    GPIO 15-18, which are that carrier's flow input, float switch and user
    button, and probes on GPIO 32-36, which are its flash and PSRAM bus. That
    is precisely the outcome the CI comment says its branch exists to prevent,
    and CI is the one place a developer never runs.

    IT REFUSES AND NEVER WRITES. data/config.json is gitignored and holds a
    real device's Wi-Fi, OTA and MQTT credentials; a hook that "helpfully"
    copied a template over it would destroy them to fix a build. Copying is the
    operator's decision, with a backup in hand.

    THE TEST IS A SUBSET, NOT EQUALITY, and it restates no pin rule. The
    template describes a carrier PCB, so its pins are soldered: a document for
    this board may declare FEWER of them — a probe left unfitted — but it
    cannot invent one, because there is nothing else to wire to. That catches a
    foreign document decisively (a WROOM-32 config uses GPIO 17, 23, 26, 27, 32
    and 34-36, none of which the S3 carrier declares) without this file growing
    a third copy of pin_rules.h.

    Envs with no template under templates/ are not checked at all, so the five
    WROOM-32 boards are untouched and cannot be falsely accused.
    """
    template = root / "templates" / ("config.%s.json" % env_name)
    if not template.is_file():
        return None

    config = root / "data" / "config.json"
    how = (
        "templates/config.%s.json is the pin map for this board. CI copies it\n"
        "over data/config.json before building; a local build will not do that\n"
        "for you, because data/config.json is gitignored and holds a real\n"
        "device's Wi-Fi, OTA and MQTT credentials.\n"
        "\n"
        "Back that file up first (GET /config.json?secrets=1 from the device is\n"
        "the restorable form; a masked GET is not), then:\n"
        "    cp templates/config.%s.json data/config.json\n"
        "and put yours back afterwards." % (env_name, env_name))

    if not config.is_file():
        return ("%s packs data/config.json into its filesystem image, and there\n"
                "is no data/config.json here.\n\n%s" % (env_name, how))

    try:
        wanted = declared_pins(json.loads(template.read_text(encoding="utf-8")))
        have = declared_pins(json.loads(config.read_text(encoding="utf-8")))
    except (OSError, ValueError) as exc:
        return "could not read the config documents for %s: %s" % (env_name, exc)

    foreign = sorted(p for p in have if p not in wanted)
    if not foreign:
        return None

    lines = "\n".join("    GPIO %-3d %s" % (p, have[p]) for p in foreign)
    count = ("1 pin" if len(foreign) == 1 else "%d pins" % len(foreign))
    return ("data/config.json was not written for %s, and packing it would\n"
            "ship an image whose sensors and relays sit on that board's own\n"
            "wiring. It assigns %s the carrier does not offer:\n\n"
            "%s\n\n%s" % (env_name, count, lines, how))


def build_assets(root, out_dir):
    """Run scripts/build_assets.py into `out_dir`, or raise SystemExit."""
    builder = root / "scripts" / "build_assets.py"
    result = subprocess.run(
        [sys.executable, str(builder), "--out", out_dir],
        cwd=str(root), capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise SystemExit("build_assets.py failed; refusing to pack a stale image")
    print(result.stdout.strip())


# Only under PlatformIO/SCons, which injects Import/env/COMMAND_LINE_TARGETS
# into this module's globals. Guarded so the functions above stay importable.
if "Import" in globals():
    Import("env")  # noqa: F821  (injected by SCons)

    if FS_TARGETS.intersection(COMMAND_LINE_TARGETS):  # noqa: F821
        _root = Path(env.subst("$PROJECT_DIR"))  # noqa: F821

        # Before build_assets, so the refusal arrives instead of a staging
        # directory that looks ready to pack.
        _why = config_guard(env.subst("$PIOENV"), _root)  # noqa: F821
        if _why:
            raise SystemExit("\n[config] " + _why + "\n")

        build_assets(_root, env.subst("$PROJECT_DATA_DIR"))  # noqa: F821
