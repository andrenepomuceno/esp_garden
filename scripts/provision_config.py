"""Turn a board template into a device's own data/config.json.

Written because every field this fills is one that BRICKS the board when it is
wrong, and the day a new board arrives is the worst day to be editing JSON by
hand. `ConfigFile::loadFile()` rejects the WHOLE document on any of them, and a
rejected document means compiled defaults, `ssid "undefined"`, and a device that
cannot associate or be reached to fix -- so every check here refuses before the
file is written rather than after it is flashed.

  python scripts/provision_config.py --env espgarden_s3 --port COM7
  python scripts/provision_config.py --env espgarden_s3 --id 1a2b \
      --set wifi.ssid=garden --set mqtt.username=<token>

Secrets are prompted for without echo and never printed back.
"""
import argparse
import getpass
import io
import json
import os
import re
import shutil
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "data", "config.json")

# src/config.cpp: g_configMinStringLength, and the exact seven fields the
# length check covers. thingSpeak and talkBack are in it even though both are
# compiled out since 2.12.0 -- the PARSE was deliberately left untouched so a
# field device's document loads the same whatever the build contains, so
# clearing those keys "because the feature is off" rejects the document.
MIN_STRING = 4
LENGTH_CHECKED = [
    "hostname",
    "wifi.ssid",
    "wifi.password",
    "ota.username",
    "ota.password",
    "thingSpeak.apiKey",
    "talkBack.apiKey",
]

# Two pem files, one per backend. Crossing them fails silently: every connect
# returns -9984 while the dashboard still reports MQTT enabled, which is how
# channel 1348790 received nothing for three years.
BACKEND_CA = {"thingsboard": "/thingsboard.pem", "thingspeak": "/thingspeak.pem"}

# Values a template ships with that are not usable on any real device.
PLACEHOLDERS = {
    "1a2b",
    "wifi name",
    "wifi password",
    "ota update username",
    "ota update password",
    "thingsboard device access token",
}


def shown(path):
    """A path to print. relpath() RAISES across Windows drives, so a --out on
    another volume killed the script with a traceback after the file had
    already been written successfully -- which reads as a failure."""
    try:
        return os.path.relpath(path, REPO)
    except ValueError:
        return path


def get(doc, path):
    node = doc
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def put(doc, path, value):
    parts = path.split(".")
    node = doc
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


def read_id_from_serial(port, baud=115200, timeout=40):
    """Read the chip's own `ID:` boot line.

    That line is BY DEFINITION the number loadFile() compares against
    (ESP.getEfuseMac() % 0x10000), which is why it is read here rather than
    derived from esptool. Opening the port asserts DTR and resets the board,
    which is what produces the boot we want -- nothing here toggles the lines
    by hand, because doing so has left this board in DOWNLOAD_BOOT before:
    off the network, silent on serial, looking exactly like a hang.
    """
    try:
        import serial  # PlatformIO ships pyserial
    except ImportError:
        raise SystemExit("pyserial is not importable; pass --id instead")

    print("opening %s and waiting for the boot line..." % port)
    with serial.Serial(port, baud, timeout=1) as ser:
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = ser.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            print("  | " + line[:100])
            found = re.search(r"\bID:\s*([0-9a-fA-F]{1,4})\b", line)
            if found:
                return found.group(1).lower().zfill(4)
    raise SystemExit("no `ID:` line in %d s. Is the board running this "
                     "firmware, and is the port right?" % timeout)


def refusals(doc):
    """Every reason this document would be refused, named. Empty means usable."""
    out = []

    dev_id = doc.get("id")
    if not isinstance(dev_id, str) or not re.fullmatch(r"[0-9a-f]{1,4}", dev_id):
        out.append("id %r is not 1-4 lowercase hex digits" % (dev_id,))
    elif dev_id in PLACEHOLDERS:
        out.append("id is still the template placeholder %r -- loadFile() "
                   "rejects the document and the board comes up on compiled "
                   "defaults it cannot associate with" % dev_id)

    for path in LENGTH_CHECKED:
        value = get(doc, path)
        if not isinstance(value, str) or len(value) < MIN_STRING:
            out.append("%s must be at least %d characters (config.cpp rejects "
                       "the whole document otherwise)" % (path, MIN_STRING))
        elif value in PLACEHOLDERS:
            out.append("%s is still the template placeholder" % path)

    backend = get(doc, "mqtt.backend")
    cacert = get(doc, "mqtt.cacert")
    if backend not in BACKEND_CA:
        out.append("mqtt.backend %r is neither thingsboard nor thingspeak" % backend)
    elif cacert != BACKEND_CA[backend]:
        out.append("mqtt.backend is %r but mqtt.cacert is %r; it should be %r. "
                   "A crossed CA fails TLS SILENTLY -- the dashboard keeps "
                   "saying MQTT enabled while nothing is ever accepted"
                   % (backend, cacert, BACKEND_CA[backend]))

    if backend == "thingsboard" and get(doc, "mqtt.username") in ("", None):
        out.append("mqtt.username is empty; ThingsBoard carries the device "
                   "access token there, with an empty password")

    relays = get(doc, "io.relays") or []
    for i, probe in enumerate(doc.get("moisture") or []):
        relay = probe.get("relay", -1)
        if relay >= len(relays):
            out.append("moisture[%d].relay is %d but only %d relays are "
                       "declared; -1 means no pump feeds this probe"
                       % (i, relay, len(relays)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--env", default="espgarden_s3",
                    help="picks templates/config.<env>.json")
    ap.add_argument("--id", help="device id in hex, e.g. 1a2b")
    ap.add_argument("--port", help="serial port to read the ID: boot line from")
    ap.add_argument("--set", action="append", default=[], metavar="path=value",
                    help="set a field, e.g. --set wifi.ssid=garden")
    ap.add_argument("--out", default=OUT)
    ap.add_argument("--interactive", action="store_true",
                    help="prompt for whatever is still a placeholder. OFF by "
                         "default: getpass() on Windows reads the console "
                         "rather than stdin, so under a pipe it does not fail, "
                         "it hangs waiting for a keypress nobody will give")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing config.json (a backup is "
                         "written either way)")
    args = ap.parse_args()

    template = os.path.join(REPO, "templates", "config.%s.json" % args.env)
    if not os.path.exists(template):
        fallback = os.path.join(REPO, "data", "config.template.json")
        if args.env in ("", "default") and os.path.exists(fallback):
            template = fallback
        else:
            raise SystemExit("no template at %s" % template)
    doc = json.load(io.open(template, encoding="utf-8"))
    print("template: %s" % shown(template))

    if args.port and args.id:
        raise SystemExit("--id and --port both given; pick one")
    if args.port:
        doc["id"] = read_id_from_serial(args.port)
        print("id from the board: %s" % doc["id"])
    elif args.id:
        doc["id"] = args.id.lower().lstrip("0x").zfill(4)

    for pair in args.set:
        if "=" not in pair:
            raise SystemExit("--set wants path=value, got %r" % pair)
        path, value = pair.split("=", 1)
        put(doc, path, value)

    # Anything still a placeholder is asked for, secrets without echo -- but
    # ONLY when there is a console to ask. getpass() on Windows reads the
    # console directly rather than stdin, so under a pipe or in CI it does not
    # fail, it HANGS waiting for a keypress nobody is there to give. A tool
    # that hangs is worse than one that refuses, so a non-interactive run falls
    # straight through to refusals(), which names every field that is missing.
    if args.interactive:
        for path in LENGTH_CHECKED + ["mqtt.username"]:
            value = get(doc, path)
            if value in PLACEHOLDERS or value in ("", None):
                secret = any(w in path.lower()
                             for w in ("password", "key", "token", "username"))
                prompt = "%s: " % path
                got = (getpass.getpass(prompt) if secret
                       else raw_input_compat(prompt))
                if got:
                    put(doc, path, got)
    else:
        print("non-interactive: pass --interactive to be asked for what is "
              "missing, or supply it with --set")

    bad = refusals(doc)
    if bad:
        print("\nREFUSED -- not writing anything:")
        for reason in bad:
            print("  - %s" % reason)
        return 1

    if os.path.exists(args.out):
        backup = "%s.%s.bak" % (args.out, time.strftime("%Y%m%d-%H%M%S"))
        shutil.copy2(args.out, backup)
        print("backed up the existing config to %s"
              % shown(backup))
        if not args.force:
            print("REFUSED: %s exists. Re-run with --force once you have "
                  "checked that backup -- it holds a real device's credentials "
                  "and a masked GET cannot restore them."
                  % shown(args.out))
            return 1

    io.open(args.out, "w", encoding="utf-8", newline="\n").write(
        json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
    print("\nwrote %s" % shown(args.out))
    print("  id %s | hostname %s | backend %s | cacert %s"
          % (doc["id"], doc["hostname"], doc["mqtt"]["backend"],
             doc["mqtt"]["cacert"]))
    print("  %d relays, %d probes" % (len(doc["io"]["relays"]),
                                      len(doc["io"]["soilMoisture"])))
    print("\nNext: `pio run -e %s -t buildfs` -- the pre: hook's config_guard "
          "re-checks the pins against this template before packing an image."
          % args.env)
    return 0


def raw_input_compat(prompt):
    sys.stdout.write(prompt)
    sys.stdout.flush()
    return sys.stdin.readline().strip()


if __name__ == "__main__":
    sys.exit(main())
