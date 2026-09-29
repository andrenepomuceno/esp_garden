#!/usr/bin/env python3
"""Deploy a firmware image and changed web assets over HTTP, keeping the flash.

`-t uploadfs` and the filesystem OTA rewrite the whole partition, which takes
/config.json, hist0..7.bin and moisture_model.bin with them. This does neither:
assets go one file at a time through POST /spiffs/upload, which renames over
the target atomically, and the firmware goes through /updateEnable + /update,
which writes the other app slot and never touches the filesystem partition.

--plan is the default and a write needs --yes. See docs/deploy.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import flash_image
from device_http import Device

ASSET_DIR = pathlib.Path(".pio/assets")
BACKUP_DIR = pathlib.Path("backups")
DEFAULT_HOST = "espgarden-s3.local"
DEFAULT_CREDENTIALS = pathlib.Path("data/config.json")

NEVER_UPLOAD = {
    "config.json",
    "config.template.json",
    "users.json",
    "sessions.json",
    "provisioned.pending",
    "moisture_model.bin",
}

SHADOWED_PREFIXES = ("config", "users", "sessions")

CHIP_FAMILY = {0x0000: "esp32", 0x0009: "esp32s3"}
FAMILY_ADC1 = {"esp32": {32, 33, 34, 35, 36, 39}, "esp32s3": set(range(1, 11))}

IMAGE_MAGIC = flash_image.IMAGE_MAGIC


class Refused(Exception):
    pass


# ---------------------------------------------------------------- pure logic

def upload_target_refusal(name):
    if name in NEVER_UPLOAD:
        return "%s is never written by this tool" % name
    if ".." in name or name.startswith("/") or "\\" in name:
        return "%s is not a plain filename" % name
    if not name:
        return "empty filename"
    return None


def is_shadowed(name):
    return name.startswith(SHADOWED_PREFIXES)


def image_family(blob):
    if not blob or blob[0] != IMAGE_MAGIC:
        raise Refused("the image does not start with %#04x" % IMAGE_MAGIC)
    if len(blob) < 14:
        raise Refused("the image is too short to carry a header")
    chip = int.from_bytes(blob[12:14], "little")
    if chip not in CHIP_FAMILY:
        raise Refused("unknown chip id %#06x in the image header" % chip)
    return CHIP_FAMILY[chip]


def device_family(analog_pins):
    pins = set(int(p) for p in analog_pins)
    for family, expected in FAMILY_ADC1.items():
        if pins == expected:
            return family
    return None


def family_refusal(image, device):
    if device is None:
        return ("the device did not report an ADC1 set this tool recognises, "
                "so nothing here can say which family it is")
    if image != device:
        return ("the image is for %s and the device is %s; flashing it is a "
                "serial recovery" % (image, device))
    return None


def version_refusal(image_version, running):
    if image_version is None:
        return "no firmware version could be read out of the image"
    if running and image_version == running:
        return ("the image and the device both report %s, so the update would "
                "be a no-op the broker reports as success" % image_version)
    return None


def image_version(blob):
    ranked = flash_image.scan_versions(blob)
    return ranked[0][0] if ranked else None


def plan_uploads(local_md5, remote_md5, shadowed):
    upload, identical, unverifiable = [], [], []
    for name in sorted(local_md5):
        if upload_target_refusal(name):
            continue
        if name in shadowed:
            unverifiable.append(name)
            upload.append(name)
        elif remote_md5.get(name) == local_md5[name]:
            identical.append(name)
        else:
            upload.append(name)
    return upload, identical, unverifiable


# ---------------------------------------------------------------- transport

def connect(host, username, password, timeout):
    device = Device(host, timeout=timeout)
    device.login(username, password)
    return device


def credentials(path):
    document = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    ota = document.get("ota", {})
    return ota.get("username", ""), ota.get("password", "")


def get_bytes(device, path, timeout=30):
    request = urllib.request.Request(device.base + path)
    request.add_header("Authorization-Token", device.token)
    with device._opener.open(request, timeout=timeout) as response:
        return response.read()


def post(device, path, content_type, body, timeout=120):
    request = urllib.request.Request(device.base + path, data=body,
                                     method="POST")
    request.add_header("Authorization-Token", device.token)
    request.add_header("Content-Type", content_type)
    with device._opener.open(request, timeout=timeout) as response:
        return response.status, response.read().decode("utf-8", "replace")


def multipart(fields, filename, payload):
    boundary = "----espgarden" + hashlib.md5(payload).hexdigest()[:16]
    body = b""
    for key, value in fields.items():
        body += ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n'
                 '%s\r\n' % (boundary, key, value)).encode()
    body += ('--%s\r\nContent-Disposition: form-data; name="file"; '
             'filename="%s"\r\nContent-Type: application/octet-stream\r\n\r\n'
             % (boundary, filename)).encode()
    body += payload + b"\r\n" + ("--%s--\r\n" % boundary).encode()
    return "multipart/form-data; boundary=" + boundary, body


# ---------------------------------------------------------------- device state

def read_status(device):
    return json.loads(device._request("/data.json"))


def energised_relays(payload):
    return [r for r in payload.get("Relays", []) if int(r.get("on", 0))]


def read_capabilities(device):
    try:
        return json.loads(device._request("/capabilities.json"))
    except Exception:
        return {}


def back_up_config(device, host):
    BACKUP_DIR.mkdir(exist_ok=True)
    raw = device._request("/config.json?secrets=1")
    document = json.loads(raw)
    masked = [
        "%s.%s" % (section, key)
        for section, key in (("wifi", "password"), ("ota", "password"),
                             ("mqtt", "password"), ("thingSpeak", "apiKey"),
                             ("talkBack", "apiKey"))
        if document.get(section, {}).get(key) == "********"
    ]
    if masked:
        raise Refused("the backup came back masked in %s, so it could not be "
                      "restored from" % ", ".join(masked))
    stamp = time.strftime("%Y%m%d-%H%M%S")
    path = BACKUP_DIR / ("config-%s-%s.json" % (document.get("id", host), stamp))
    path.write_text(raw, encoding="utf-8")
    return path, document


def remote_md5s(device, names):
    found, shadowed = {}, set()
    for name in names:
        try:
            found[name] = hashlib.md5(
                get_bytes(device, "/spiffs/" + name)).hexdigest()
        except urllib.error.HTTPError as error:
            if error.code == 403:
                shadowed.add(name)
            elif error.code != 404:
                raise
        except urllib.error.URLError:
            raise
    return found, shadowed


# ---------------------------------------------------------------- operations

def send_assets(device, names, write):
    sent = 0
    for name in names:
        payload = (ASSET_DIR / name).read_bytes()
        digest = hashlib.md5(payload).hexdigest()
        if not write:
            print("    would send %-22s %7d B  md5 %s" %
                  (name, len(payload), digest[:12]))
            continue
        content_type, body = multipart({"MD5": digest}, name, payload)
        code, text = post(device, "/spiffs/upload", content_type, body)
        answer = json.loads(text) if text.strip().startswith("{") else {}
        if code != 200 or answer.get("ok") is not True:
            raise Refused("%s refused: HTTP %s %s" % (name, code, text[:160]))
        print("    sent       %-22s %7d B  free %s" %
              (name, len(payload), answer.get("free", "?")))
        sent += 1
    return sent


def verify_assets(device, names):
    mismatched, unreadable = [], []
    for name in names:
        local = hashlib.md5((ASSET_DIR / name).read_bytes()).hexdigest()
        try:
            remote = hashlib.md5(get_bytes(device, "/spiffs/" + name)).hexdigest()
        except urllib.error.HTTPError as error:
            if error.code == 403:
                unreadable.append(name)
                continue
            raise
        if remote != local:
            mismatched.append(name)
    return mismatched, unreadable


def send_firmware(device, blob, write):
    if not write:
        print("    would send firmware %d B  md5 %s" %
              (len(blob), hashlib.md5(blob).hexdigest()[:12]))
        return None
    code, text = post(device, "/updateEnable",
                      "application/x-www-form-urlencoded", b"")
    if code != 200:
        raise Refused("/updateEnable answered HTTP %s: %s" % (code, text[:160]))
    print("    /updateEnable -> %s %s" % (code, text.strip()[:80]))
    digest = hashlib.md5(blob).hexdigest()
    content_type, body = multipart({"MD5": digest}, "firmware", blob)
    started = time.time()
    try:
        code, text = post(device, "/update", content_type, body, timeout=420)
        print("    /update -> %s %s  (%.1f s)" %
              (code, text.strip()[:100], time.time() - started))
    except Exception as error:
        print("    the upload's own connection ended: %s %s" %
              (type(error).__name__, str(error)[:80]))
        print("    that is not a verdict - the board reboots and kills it")
    return started


def wait_for_reboot(device, previous_version, deadline=240):
    went_down = False
    started = time.time()
    while time.time() - started < deadline:
        try:
            request = urllib.request.Request(device.base + "/device.json")
            with device._opener.open(request, timeout=4) as response:
                response.read()
            if went_down:
                print("    serving again at %.0f s" % (time.time() - started))
                return True
        except Exception:
            if not went_down:
                print("    the board went down at %.0f s" %
                      (time.time() - started))
                went_down = True
        time.sleep(2)
    print("    deadline reached, went down: %s" % went_down)
    return went_down


def confirm_version(device, username, password, expected, deadline=120):
    started = time.time()
    while time.time() - started < deadline:
        try:
            fresh = connect(device.base.split("//", 1)[1], username, password,
                            15)
            running = read_status(fresh).get("Status", {}).get("Firmware")
            if running == expected:
                return True, running, fresh
            print("    still reporting %s" % running)
        except Exception:
            pass
        time.sleep(5)
    return False, None, None


# ---------------------------------------------------------------- self-test

def self_test():
    checks, failed = 0, 0

    def check(label, condition):
        nonlocal checks, failed
        checks += 1
        if not condition:
            failed += 1
            print("  FAIL %s" % label)

    for name in ("config.json", "users.json", "sessions.json",
                 "provisioned.pending", "config.template.json"):
        check("%s is refused" % name, upload_target_refusal(name))
    check("a normal asset is allowed", upload_target_refusal("index.js.gz") is None)
    check("traversal is refused", upload_target_refusal("../users.json"))
    check("an absolute path is refused", upload_target_refusal("/index.html"))
    check("a backslash is refused", upload_target_refusal("a\\b"))
    check("an empty name is refused", upload_target_refusal(""))

    check("config.html.gz is shadowed", is_shadowed("config.html.gz"))
    check("users.js.gz is shadowed", is_shadowed("users.js.gz"))
    check("index.js.gz is not shadowed", not is_shadowed("index.js.gz"))

    esp32 = bytes([IMAGE_MAGIC]) + b"\0" * 11 + (0).to_bytes(2, "little")
    s3 = bytes([IMAGE_MAGIC]) + b"\0" * 11 + (9).to_bytes(2, "little")
    check("an ESP32 image reads as esp32", image_family(esp32) == "esp32")
    check("an S3 image reads as esp32s3", image_family(s3) == "esp32s3")
    try:
        image_family(b"\x00" * 20)
        check("a bad magic is refused", False)
    except Refused:
        check("a bad magic is refused", True)
    try:
        image_family(bytes([IMAGE_MAGIC]) + b"\0" * 11 + (0x77).to_bytes(2, "little"))
        check("an unknown chip id is refused", False)
    except Refused:
        check("an unknown chip id is refused", True)

    check("ADC1 32-39 is esp32", device_family([32, 33, 34, 35, 36, 39]) == "esp32")
    check("ADC1 1-10 is esp32s3", device_family(range(1, 11)) == "esp32s3")
    check("an unknown ADC1 set is None", device_family([1, 2]) is None)
    check("a family match passes", family_refusal("esp32s3", "esp32s3") is None)
    check("a family mismatch is refused", family_refusal("esp32s3", "esp32"))
    check("an unknown device family is refused", family_refusal("esp32", None))

    check("the same version is refused", version_refusal("2.21.0", "2.21.0"))
    check("a newer version passes", version_refusal("2.21.0", "2.20.0") is None)
    check("a downgrade passes", version_refusal("2.19.0", "2.20.0") is None)
    check("no version in the image is refused", version_refusal(None, "2.20.0"))
    check("no running version passes", version_refusal("2.21.0", None) is None)

    local = {"a.gz": "1", "b.gz": "2", "config.js.gz": "3", "config.json": "4"}
    remote = {"a.gz": "1", "b.gz": "changed"}
    upload, identical, unverifiable = plan_uploads(local, remote, {"config.js.gz"})
    check("an identical asset is skipped", identical == ["a.gz"])
    check("a changed asset is sent", "b.gz" in upload)
    check("a shadowed asset is sent", "config.js.gz" in upload)
    check("a shadowed asset is named unverifiable",
          unverifiable == ["config.js.gz"])
    check("config.json is never planned", "config.json" not in upload)

    print("deploy_ota self-test: %d checks, %d failed" % (checks, failed))
    return 1 if failed else 0


# ---------------------------------------------------------------- driver

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--firmware")
    parser.add_argument("--assets", default=".pio/assets")
    parser.add_argument("--credentials", default=str(DEFAULT_CREDENTIALS))
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--yes", action="store_true",
                        help="actually write; without it this is a dry run")
    parser.add_argument("--assets-only", action="store_true")
    parser.add_argument("--firmware-only", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    global ASSET_DIR
    ASSET_DIR = pathlib.Path(args.assets)

    username, password = args.username, args.password
    if not username or not password:
        stored_user, stored_password = credentials(args.credentials)
        username = username or stored_user
        password = password or os.environ.get("ESP_GARDEN_PASSWORD") or \
            stored_password

    write = args.yes
    print("%s %s" % ("WRITING to" if write else "PLAN for", args.host))

    device = connect(args.host, username, password, args.timeout)
    payload = read_status(device)
    status = payload.get("Status", {})
    running = status.get("Firmware")
    print("  firmware %s   history %s   filesystem %s" %
          (running, status.get("History"), status.get("Filesystem")))

    hot = energised_relays(payload)
    if hot:
        raise Refused("relays energised: %s. An update reboots the device and "
                      "a relay across a reset is the one thing not to do."
                      % ", ".join(r["name"] for r in hot))
    print("  every relay is idle")

    backup, document = back_up_config(device, args.host)
    print("  config backed up to %s (id %s, %d chars, no masks)" %
          (backup, document.get("id"), len(backup.read_text(encoding="utf-8"))))

    do_assets = not args.firmware_only
    do_firmware = not args.assets_only

    upload = []
    if do_assets:
        names = [p.name for p in sorted(ASSET_DIR.iterdir()) if p.is_file()]
        local = {}
        for name in names:
            if upload_target_refusal(name):
                continue
            local[name] = hashlib.md5((ASSET_DIR / name).read_bytes()).hexdigest()
        remote, shadowed = remote_md5s(device, list(local))
        upload, identical, unverifiable = plan_uploads(local, remote, shadowed)
        total = sum((ASSET_DIR / n).stat().st_size for n in upload)
        print("  assets: %d to send (%d B), %d identical, %d unverifiable "
              "(shadowed path)" %
              (len(upload), total, len(identical), len(unverifiable)))
        for name in unverifiable:
            print("    unverifiable %s" % name)

    blob = None
    if do_firmware:
        firmware = args.firmware or ".pio/build/espgarden_s3/firmware.bin"
        blob = pathlib.Path(firmware).read_bytes()
        family = image_family(blob)
        caps = read_capabilities(device)
        detected = device_family(caps.get("analogPins", []))
        refusal = family_refusal(family, detected)
        if refusal:
            raise Refused(refusal)
        version = image_version(blob)
        refusal = version_refusal(version, running)
        if refusal:
            raise Refused(refusal)
        print("  firmware: %s, %s, %d B -> device is %s running %s" %
              (firmware, family, len(blob), detected, running))

    if do_assets and upload:
        print("  PHASE 1 assets")
        send_assets(device, upload, write)
        if write:
            mismatched, unreadable = verify_assets(device, upload)
            if mismatched:
                raise Refused("read back and did not match: %s" %
                              ", ".join(mismatched))
            print("    verified %d by reading back, %d unreadable (shadowed)" %
                  (len(upload) - len(unreadable), len(unreadable)))

    if do_firmware:
        print("  PHASE 2 firmware")
        send_firmware(device, blob, write)
        if write:
            wait_for_reboot(device, running)
            expected = image_version(blob)
            ok, seen, fresh = confirm_version(device, username, password,
                                              expected)
            if not ok:
                print("    the version did not reach %s" % expected)
                return 1
            after = read_status(fresh).get("Status", {})
            print("    running %s, history %s, filesystem %s" %
                  (after.get("Firmware"), after.get("History"),
                   after.get("Filesystem")))

    if not write:
        print("\nthis was a plan. Re-run with --yes to write.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refused as refusal:
        print("REFUSED: %s" % refusal)
        raise SystemExit(1)
