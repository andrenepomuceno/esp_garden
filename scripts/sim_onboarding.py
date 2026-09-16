"""The first-boot setup portal, mirrored.

This is the ONLY place the onboarding page will ever be rendered by a browser
until somebody builds a board and flashes it, so it is a first-class part of the
change rather than a checklist item. What it is evidence about is itself: the
firmware and this file are separate implementations, and a green mirror has
never been a statement about the C++.

TWO HALVES, DELIBERATELY DIFFERENT

  EXTRACTED, so it cannot drift: the page, the templates and the marker path.
  Those are compiled STRINGS in the firmware, and a Python copy of a 4 KB HTML
  document would be wrong within a week. They are read straight out of
  src/web_onboarding.cpp, include/core/onboarding_templates.h and
  include/core/onboarding.h, so the browser renders exactly the bytes the device
  serves and the dropdown offers exactly the boards it offers.

  REIMPLEMENTED, because that is the point of a mirror: the decision, the
  refusals and the merge. A second implementation is what makes a disagreement
  visible; a shared one only proves the sharing works.

WHAT THIS CANNOT SHOW. No radio, so no soft AP, no WPA2, no captive-portal DNS
and no phone deciding to open a sign-in sheet. The one thing the portal is
FOR -- being reachable at all on a board that is not on any network -- is
exactly the thing a localhost mirror cannot exercise.
"""

from __future__ import annotations

import io
import json
import os
import re

# The pairing that fails TLS silently when it is crossed. Imported rather than
# restated: provision_config.py is the other Python owner of this table and the
# C++ owner is onboarding::expectedCaFor(), which test_onboarding pins.
from provision_config import BACKEND_CA

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(REPO, "src", "web_onboarding.cpp")
TEMPLATES_H = os.path.join(REPO, "include", "core", "onboarding_templates.h")
ONBOARDING_H = os.path.join(REPO, "include", "core", "onboarding.h")

# Mirror of g_configMinStringLength. Restated rather than imported from
# sim_config so this module stays importable on its own.
MIN_STRING = 4

# The firmware's namespace names, which /onboarding.json reports as `chip`.
# dev_server's ESP_GARDEN_PIN_FAMILY uses the same two words for the same
# reason: one spelling, so a disagreement is a disagreement and not a rename.
FAMILIES = ("wroom32", "esp32s3")


def _read(path):
    return io.open(path, encoding="utf-8").read()


def _join_literals(chunk):
    """Concatenated C string literals -> the string the compiler builds."""
    out = []
    for piece in re.findall(r'"((?:[^"\\]|\\.)*)"', chunk):
        out.append(piece.replace('\\"', '"').replace("\\\\", "\\"))
    return "".join(out)


def marker_path():
    """onboarding::kMarkerPath, read from the header that declares it.

    Its ABSENCE is what keeps a board that has ever reached the network out of
    the portal for good, and the upload guard refuses it for the same reason --
    three readers of one string is exactly the shape that drifts.
    """
    text = _read(ONBOARDING_H)
    found = re.search(r'kMarkerPath\s*=\s*"([^"]+)"', text)
    if not found:
        raise SystemExit("could not find kMarkerPath in %s" % ONBOARDING_H)
    return found.group(1)


def page_html():
    """The R"PAGE(...)PAGE" literal out of src/web_onboarding.cpp."""
    text = _read(CPP)
    found = re.search(r'R"PAGE\((.*?)\)PAGE"', text, re.S)
    if not found:
        raise SystemExit("could not find the R\"PAGE(...)\" literal in %s" % CPP)
    return found.group(1)


def templates_for(family):
    """[{id, name, summary, json}] for one MCU family, out of the header.

    The firmware picks its namespace from CONFIG_IDF_TARGET_*; this picks it by
    name, because a simulator has no board. Offering the wrong family here would
    show a dropdown the device never shows -- the mirror's version of the hazard
    config_guard() refuses in scripts/pio_assets.py.
    """
    if family not in FAMILIES:
        raise SystemExit("unknown template family %r; known: %s"
                         % (family, ", ".join(FAMILIES)))

    text = _read(TEMPLATES_H)
    start = text.find("namespace %s {" % family)
    if start < 0:
        raise SystemExit("no namespace %s in %s" % (family, TEMPLATES_H))
    end = text.find("} // namespace %s" % family, start)
    block = text[start:end if end > 0 else len(text)]

    bodies = {}
    for name, chunk in re.findall(
            r"kJson(\w+)\s*=\s*(.*?);", block, re.S):
        bodies["kJson" + name] = _join_literals(chunk)

    table = re.search(r"kTemplates\[\]\s*=\s*\{(.*?)\n\};", block, re.S)
    if not table:
        raise SystemExit("no kTemplates[] in namespace %s" % family)

    out = []
    for entry in re.findall(r"\{(.*?)\}", table.group(1), re.S):
        body = re.search(r"(kJson\w+)", entry)
        if not body:
            continue
        # Split on the initialiser's OWN commas, then join each field's
        # adjacent literals the way the compiler does — a summary written
        # across two source lines is one string, and a comma inside one of them
        # is not a field separator.
        fields = [_join_literals(part) for part in _split_top_level(entry)]
        if len(fields) < 3:
            continue
        out.append({
            "id": fields[0],
            "name": fields[1],
            "summary": fields[2],
            "json": bodies.get(body.group(1), ""),
        })
    return out


def _split_top_level(entry):
    """Split a `{ "a", "b" "c", kJson }` initialiser on its own commas."""
    parts, depth, current, in_string, escaped = [], 0, "", False, False
    for ch in entry:
        if escaped:
            current += ch
            escaped = False
            continue
        if ch == "\\":
            current += ch
            escaped = True
            continue
        if ch == '"':
            in_string = not in_string
            current += ch
            continue
        if not in_string:
            if ch in "{[(":
                depth += 1
            elif ch in "}])":
                depth -= 1
            elif ch == "," and depth == 0:
                parts.append(current)
                current = ""
                continue
        current += ch
    parts.append(current)
    return parts


def decide(config_loaded, marker_present, associated):
    """A SECOND implementation of onboarding::decide().

    The property it carries is negative and therefore easy to lose: a board
    whose config loaded and which carries no marker is Normal unconditionally,
    with `associated` never read. The C++ half is host-tested in
    test/test_onboarding; this one exists so the simulator cannot quietly show a
    portal on a state the device would not.
    """
    if not config_loaded:
        return ("portal", "no-usable-config", False)
    if not marker_present:
        return ("normal", "configured", False)
    if associated:
        return ("normal", "configured", True)
    return ("portal", "never-associated", False)


REASON_TEXT = {
    "no-usable-config": ("/config.json is missing, unparseable, addressed at "
                         "another chip, or short of a required field"),
    "never-associated": ("the credentials written by the last setup have "
                         "never associated"),
    "configured": "configured",
}


def info(device_id, family, firmware, reason):
    """The /onboarding.json payload."""
    ssid = "espgarden-%s" % device_id
    return {
        "id": device_id,
        "chip": family,
        "firmware": firmware,
        "ap": ssid,
        "hostname": ssid,
        "reason": REASON_TEXT.get(reason, reason),
        "templates": [{"id": t["id"], "name": t["name"],
                       "summary": t["summary"]}
                      for t in templates_for(family)],
    }


def _pin_of(node):
    if isinstance(node, bool):
        return None
    if isinstance(node, (int, float)):
        return int(node)
    if isinstance(node, dict) and "pin" in node:
        try:
            return int(node["pin"])
        except (TypeError, ValueError):
            return None
    return None


def document_pins_problem(doc, rules):
    """Mirrors documentPinsAreUsable(), against the family dev_server selected.

    `rules` is dev_server.PIN_RULES[PIN_FAMILY]; it is passed in rather than
    restated here, because a second copy of the predicates in the mirror would
    be the mirror drifting from itself.
    """
    io_cfg = doc.get("io")
    if not isinstance(io_cfg, dict):
        return None

    flash, adc1 = rules["flash"], rules["adc1"]
    input_only, bonded = rules["input_only"], rules["bonded"]
    top = rules["max_gpio"]

    def plausible(pin):
        return 0 <= pin <= top

    for i, entry in enumerate(io_cfg.get("relays") or []):
        pin = _pin_of(entry)
        if pin is None:
            continue
        if not plausible(pin):
            return "io.relays[%d] on GPIO %d (no such pin on this chip)" % (i, pin)
        if flash(pin):
            return "io.relays[%d] on GPIO %d (SPI flash)" % (i, pin)
        if not bonded(pin):
            return ("io.relays[%d] on GPIO %d (not bonded out on this module)"
                    % (i, pin))
        if input_only(pin):
            return "io.relays[%d] on GPIO %d (input-only)" % (i, pin)

    probes = io_cfg.get("soilMoisture")
    if isinstance(probes, list):
        for i, entry in enumerate(probes):
            pin = _pin_of(entry)
            if pin is not None and not adc1(pin):
                return ("io.soilMoisture[%d] on GPIO %d (not an ADC1 channel)"
                        % (i, pin))
            if isinstance(entry, dict) and entry.get("powerPin", -1) >= 0:
                power = int(entry["powerPin"])
                if (not plausible(power) or not bonded(power)
                        or flash(power) or input_only(power)):
                    return ("io.soilMoisture[%d].powerPin GPIO %d cannot drive "
                            "an output" % (i, power))

    for key, analog in (("luminosity", True), ("waterLevel", True),
                        ("dht", False), ("flow", False),
                        ("floatSwitch", False)):
        if key not in io_cfg:
            continue
        pin = _pin_of(io_cfg[key])
        if pin is None:
            continue
        if not plausible(pin):
            return "io.%s on GPIO %d (no such pin on this chip)" % (key, pin)
        if flash(pin):
            return "io.%s on GPIO %d (SPI flash)" % (key, pin)
        if not bonded(pin):
            return ("io.%s on GPIO %d (not bonded out on this module)"
                    % (key, pin))
        if analog and not adc1(pin):
            return ("io.%s on GPIO %d (not an ADC1 channel; ADC2 cannot be "
                    "read with WiFi on)" % (key, pin))
        if not analog and input_only(pin):
            return "io.%s on GPIO %d (no internal pull-up)" % (key, pin)

    return None


def onboarding_apply(params, rules, device_id, family):
    """Mirrors handleSubmit(). Returns (status, message, merged document).

    Every refusal is named and nothing is written until they all pass, for the
    reason the firmware gives: the alternative is the user submitting, the board
    rebooting, the portal coming back and nothing saying why.
    """
    chosen = None
    for entry in templates_for(family):
        if entry["id"] == params.get("template", ""):
            chosen = entry
            break

    ssid = params.get("ssid", "")
    wifi_password = params.get("password", "")
    username = params.get("username", "")
    admin_password = params.get("adminPassword", "")
    hostname = params.get("hostname", "")

    if len(ssid) < MIN_STRING:
        return (400, "The Wi-Fi network name must be at least %d characters."
                % MIN_STRING, None)
    if len(wifi_password) < MIN_STRING:
        return (400, "The Wi-Fi password must be at least %d characters; this "
                "firmware cannot join an open network." % MIN_STRING, None)
    if len(username) < MIN_STRING or len(admin_password) < MIN_STRING:
        return (400, "The admin user and password must each be at least %d "
                "characters." % MIN_STRING, None)
    if hostname and len(hostname) < MIN_STRING:
        return (400, "The hostname must be at least %d characters, or left "
                "empty." % MIN_STRING, None)

    if chosen is None:
        return (400, "Unknown board template %r for a %s build."
                % (params.get("template", ""), family), None)

    try:
        doc = json.loads(chosen["json"])
    except ValueError as exc:
        return (500, "The compiled template %r does not parse: %s"
                % (chosen["id"], exc), None)

    # The six holes, and only these six.
    doc["id"] = device_id
    if hostname:
        doc["hostname"] = hostname
    doc["wifi"]["ssid"] = ssid
    doc["wifi"]["password"] = wifi_password
    doc["ota"]["username"] = username
    doc["ota"]["password"] = admin_password

    problem = merged_document_problem(doc, rules, device_id)
    if problem:
        return (400, "Refused before writing anything: " + problem, None)

    return (200, "saved", doc)


def merged_document_problem(doc, rules, device_id):
    """configDocumentIsUsable() plus the id and CA checks handleSubmit adds."""
    for section, key in (("", "hostname"), ("wifi", "ssid"),
                         ("wifi", "password"), ("ota", "username"),
                         ("ota", "password"), ("thingSpeak", "apiKey"),
                         ("talkBack", "apiKey")):
        node = doc if section == "" else doc.get(section, {})
        value = node.get(key) if isinstance(node, dict) else None
        if not isinstance(value, str) or len(value) < MIN_STRING:
            return key if section == "" else "%s.%s" % (section, key)

    problem = document_pins_problem(doc, rules)
    if problem:
        return problem

    if str(doc.get("id", "")).lower() != str(device_id).lower():
        return "the document's id %r is not this chip's" % doc.get("id")

    mqtt = doc.get("mqtt")
    if not isinstance(mqtt, dict):
        return "the template has no mqtt block"
    if not mqtt.get("useTLS", True):
        return None
    backend = mqtt.get("backend")
    if backend not in BACKEND_CA:
        return ("mqtt.backend %r is neither thingsboard nor thingspeak"
                % backend)
    if mqtt.get("cacert") != BACKEND_CA[backend]:
        return ("mqtt.backend is %r but mqtt.cacert is %r; it should be %r"
                % (backend, mqtt.get("cacert"), BACKEND_CA[backend]))
    return None
