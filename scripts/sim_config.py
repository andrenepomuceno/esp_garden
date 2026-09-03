"""The two documents the device persists — /config.json and /users.json — plus
the validation it runs before writing either.

Mirrors ConfigFile and documentPinsAreUsable() in src/config.cpp, and the
handleConfigPost / handleUsersPost handlers in src/web.cpp. Split out of
dev_server.py, which crossed the 1000-line limit scripts/check_lines.py
enforces. Both documents live here because the two write paths share
CONFIG_MIN_STRING_LENGTH and both reach into the auth store.
"""

from __future__ import annotations

import json

from sim_auth import AUTH


# Mirror of g_configSecretMask in src/web.cpp. Secrets are replaced by this on
# GET and restored from the stored document when POSTed back unchanged.
CONFIG_SECRET_MASK = "********"
CONFIG_SECRET_PATHS = [
    ("wifi", "password"),
    ("ota", "password"),
    ("thingSpeak", "apiKey"),
    ("talkBack", "apiKey"),
    ("mqtt", "password"),
    # The ThingsBoard access token lives here; it is a credential on both
    # backends.
    ("mqtt", "username"),
]

SIM_CONFIG = {
    "version": 2,
    "id": "1a2b",
    "hostname": "espgarden-sim",
    "timezone": "<-03>3",
    "wifi": {"ssid": "sim-wifi", "password": "sim-wifi-password"},
    "ota": {"username": "admin", "password": "admin"},
    "thingSpeak": {"apiKey": "SIMKEY0000000000", "channel": 1348790},
    "talkBack": {"apiKey": "SIMTALK000000000", "channel": 42661},
    "mqtt": {
        "clientID": "sim-client",
        "username": "sim-user",
        "password": "sim-password",
        "server": "mqtt3.thingspeak.com",
        "port": 8883,
        "cacert": "/thingspeak.pem",
        "backend": "thingspeak",
        "useTLS": True,
        "rpc": True,
        "fwUpdate": True,
        "fwTitle": "esp-garden",
        "publishSec": 300,
        "heartbeatSec": 900,
    },
    "cloud": {"enabled": True},
    "log": {"level": 4},
    "history": {"records": 1440, "periodSec": 60},
    "io": {
        "button": 0,
        "relays": [
            {"pin": 19, "on": 0, "name": "Watering"},
            {"pin": 16, "on": 0, "name": "Relay 2"},
            {"pin": 17, "on": 0, "name": "Relay 3"},
            {"pin": 18, "on": 0, "name": "Relay 4"},
        ],
        "dht": 23,
        # Four probes, which is MOISTURE_MAX, so every state /moisture.json can
        # report is on the page at once. The third carries an XSS-shaped name on
        # purpose: probe names are admin-editable through /devices.html and are
        # copied verbatim into /data.json and /moisture.json, so the pages that
        # render them are the ones that have to escape them.
        "soilMoisture": [
            # A bare pin: still a shape a field device may carry, and the one
            # that has to keep loading after a firmware update.
            36,
            # Power-gated, which is what a resistive probe needs to survive:
            # energised only around the reading, so it is not an electrolysis
            # cell the rest of the time.
            {"pin": 34, "name": "Bed 2", "powerPin": 25, "powerOn": 1,
             "settleMs": 30},
            {"pin": 32, "name": "<img src=x onerror=\"window.__xss=1\">Bed 3"},
            {"pin": 33, "name": "Bed 4 (no pump)"},
        ],
        "luminosity": 39,
        "waterLevel": 35,
        "flow": {"pin": 27, "name": "Flow", "pulsesPerLitre": 450},
        "floatSwitch": {"pin": 26, "name": "Float Switch", "activeLevel": 0,
                        "interlock": False, "fillRelay": 3},
    },
    # Parallel to io.soilMoisture, exactly as ConfigFile::loadFile() reads it.
    # `dry`/`wet` are the two-point anchors (probe in air / probe submerged) and
    # `relay` is which pump feeds this probe -- the key the Bayesian model needs,
    # because it labels a reading by its distance from a watering EVENT. -1 means
    # no pump feeds it, so nothing ever labels its readings and it can only ever
    # fall back to the anchors.
    #
    # Probe 2 is left uncalibrated (dry == wet) so the "no classification at all"
    # branch is reachable; probe 3 is the -1 case.
    # `invert` is the probe's polarity and `kind` a free label; both are part of
    # the trained model's IDENTITY on the device, so changing either discards
    # that probe's statistics rather than letting a new sensor inherit the old
    # one's bands. Probe 1 is deliberately the other polarity, so the page is
    # exercised with a board carrying one of each.
    "moisture": [
        {"dry": 6.0, "wet": 86.0, "relay": 0, "invert": True,
         "kind": "capacitive-v2"},
        {"dry": 5.0, "wet": 84.0, "relay": 1, "invert": False,
         "kind": "wd-38"},
        {"dry": 0, "wet": 0, "relay": 2, "invert": True},
        {"dry": 4.0, "wet": 88.0, "relay": -1, "invert": True},
    ],
    # Both disabled, matching the fail-safe default the device now applies to a
    # schedule whose "enabled" key is absent.
    "schedules": [
        {"name": "Morning zone 1", "relay": 0, "hour": 6, "minute": 30,
         "days": 127, "durationMs": 10000, "enabled": False},
        {"name": "Fill reservoir", "relay": 3, "hour": 6, "minute": 0,
         "days": 127, "durationMs": 30000, "enabled": False},
    ],
}


def config_masked() -> dict:
    doc = json.loads(json.dumps(SIM_CONFIG))  # deep copy
    for section, key in CONFIG_SECRET_PATHS:
        # An empty field is not masked, mirroring handleConfigGet: there is
        # nothing to hide, and a mask the POST handler cannot restore from an
        # empty stored value would make every save fail.
        if doc.get(section, {}).get(key):
            doc[section][key] = CONFIG_SECRET_MASK
    return doc


# Mirrors ConfigFile::loadFile()'s tail. A document that violates this is
# rejected by the device at boot, so the write path must refuse it too.
CONFIG_MIN_STRING_LENGTH = 4
CONFIG_REQUIRED = [
    ("", "hostname"),
    ("wifi", "ssid"),
    ("wifi", "password"),
    ("ota", "username"),
    ("ota", "password"),
    ("thingSpeak", "apiKey"),
    ("talkBack", "apiKey"),
]


def _pin_of(node):
    """Pin out of an io entry that is a bare number or {pin, ...}, or None."""
    if isinstance(node, (int, float)) and not isinstance(node, bool):
        return int(node)
    if isinstance(node, dict) and "pin" in node:
        try:
            return int(node["pin"])
        except (TypeError, ValueError):
            return None
    return None


def document_pins_problem(doc: dict):
    """Mirrors documentPinsAreUsable() in src/config.cpp.

    Without this the simulator accepts pin maps the device answers 400 to, so
    the one place the UI's validation is exercised disagrees with the firmware
    it is validating for.
    """
    def is_flash(p): return 6 <= p <= 11
    def is_adc1(p): return 32 <= p <= 39 and p not in (37, 38)
    def is_input_only(p): return 34 <= p <= 39

    io_cfg = doc.get("io")
    if not isinstance(io_cfg, dict):
        return None

    for i, entry in enumerate(io_cfg.get("relays") or []):
        pin = _pin_of(entry)
        if pin is None:
            continue
        if is_flash(pin):
            return f"io.relays[{i}] on GPIO {pin} (SPI flash)"
        if is_input_only(pin):
            return f"io.relays[{i}] on GPIO {pin} (input-only)"

    probes = io_cfg.get("soilMoisture")
    if isinstance(probes, list):
        for i, entry in enumerate(probes):
            pin = _pin_of(entry)
            if pin is not None and not is_adc1(pin):
                return f"io.soilMoisture[{i}] on GPIO {pin} (not an ADC1 channel)"
    else:
        pin = _pin_of(probes)
        if pin is not None and not is_adc1(pin):
            return f"io.soilMoisture on GPIO {pin} (not ADC1)"

    for key, analog in (("luminosity", True), ("waterLevel", True),
                        ("dht", False), ("flow", False), ("floatSwitch", False)):
        if key not in io_cfg:
            continue
        pin = _pin_of(io_cfg[key])
        if pin is None:
            continue
        if is_flash(pin):
            return f"io.{key} on GPIO {pin} (SPI flash)"
        if analog and not is_adc1(pin):
            return (f"io.{key} on GPIO {pin} (not an ADC1 channel; "
                    "ADC2 cannot be read with WiFi on)")
        if not analog and is_input_only(pin):
            return f"io.{key} on GPIO {pin} (no internal pull-up)"

    return None


def config_apply(incoming: dict) -> tuple[int, str]:
    """Returns (http_status, message). Mirrors handleConfigPost in src/web.cpp."""
    if not isinstance(incoming, dict):
        return 400, "config is not a JSON object"
    if str(incoming.get("id", "")).lower() != str(SIM_CONFIG["id"]).lower():
        return 400, "config id does not match this device"

    new_ota_password = None
    for section, key in CONFIG_SECRET_PATHS:
        sec = incoming.get(section)
        if not isinstance(sec, dict) or key not in sec:
            continue
        if sec[key] == CONFIG_SECRET_MASK:
            stored = SIM_CONFIG.get(section, {}).get(key)
            if stored:
                sec[key] = stored
        elif (section, key) == ("ota", "password"):
            new_ota_password = sec[key]

    # A mask that survived the restore would be written over a real credential.
    for section, key in CONFIG_SECRET_PATHS:
        sec = incoming.get(section)
        if isinstance(sec, dict) and sec.get(key) == CONFIG_SECRET_MASK:
            return 500, "secret restore failed"

    for section, key in CONFIG_REQUIRED:
        node = incoming if section == "" else incoming.get(section, {})
        value = node.get(key) if isinstance(node, dict) else None
        if not isinstance(value, str) or len(value) < CONFIG_MIN_STRING_LENGTH:
            field = key if section == "" else f"{section}.{key}"
            return 400, f"'{field}' must have at least {CONFIG_MIN_STRING_LENGTH} characters"

    problem = document_pins_problem(incoming)
    if problem:
        return 400, problem

    SIM_CONFIG.clear()
    SIM_CONFIG.update(incoming)

    # The login password lives outside the config on the device too, so a
    # changed ota.password has to reach the auth store or the simulator's login
    # stops matching the firmware's.
    if new_ota_password:
        AUTH.set_password(new_ota_password)

    return 200, "saved"


# Mirrors UserStore. Password hashes and salts are deliberately absent: the
# device never serves them, so neither does the simulator.
SIM_USERS = [{"username": "admin", "role": 2}]


def users_apply(params: dict) -> tuple[int, str, bool]:
    """Mirrors handleUsersPost in src/web.cpp. Returns (status, message, reauth)."""
    action = params.get("action", "")
    username = params.get("username", "").strip()
    password = params.get("password", "")
    try:
        role = int(params.get("role", 1))
    except ValueError:
        role = 0

    if not username:
        return 400, "Missing username", False
    if role not in (1, 2):
        return 400, "Invalid role", False

    index = next((i for i, u in enumerate(SIM_USERS)
                  if u["username"] == username), -1)
    admins = sum(1 for u in SIM_USERS if u["role"] == 2)

    if action == "delete":
        if index < 0:
            return 404, "User not found", False
        if SIM_USERS[index]["role"] == 2 and admins <= 1:
            return 400, "Cannot delete the last admin", False
        SIM_USERS.pop(index)
        # The device stores a user INDEX in each session, so removing an entry
        # forces every session to be dropped. Mirrored here or the simulator
        # would let a stale token keep working.
        AUTH.tokens.clear()
        return 200, f"User '{username}' deleted", True

    if action == "upsert":
        if index >= 0 and SIM_USERS[index]["role"] == 2 and role != 2 and admins <= 1:
            return 400, "Cannot demote the last admin", False
        if not password:
            if index < 0:
                return 400, "Password required for a new user", False
            SIM_USERS[index]["role"] = role
        else:
            if len(password) < CONFIG_MIN_STRING_LENGTH:
                return (400,
                        f"Password must have at least {CONFIG_MIN_STRING_LENGTH} characters",
                        False)
            if index >= 0:
                SIM_USERS[index]["role"] = role
            else:
                SIM_USERS.append({"username": username, "role": role})
            if username == AUTH.USERNAME:
                AUTH.set_password(password)
        return 200, f"User '{username}' saved with role {role}", False

    return 400, "Invalid action", False
