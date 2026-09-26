#!/usr/bin/env python3
"""Full-flash backup, restore and offline verification of an esp-garden board.

    python scripts/flash_clone.py --plan                 # opens no port
    python scripts/flash_clone.py --verify <image>        # opens no port
    python scripts/flash_clone.py --self-test             # no port, no board
    python scripts/flash_clone.py --backup --device espgarden-s3.local
    python scripts/flash_clone.py --restore <image> --port COM5 --yes
    python scripts/flash_clone.py --restore <image> --patch-id cfd0 --yes

WHY THIS EXISTS. The 2026-09-26 backup of the S3 carrier was done BY HAND: an
esptool command line, then a REPL session establishing that the 8 MB was this
firmware and not 8 MB of plausible bytes. The bootloader and the partition table
are the two things that cannot be delivered over OTA, so a full-flash dump is
the only artefact that moves a garden onto new silicon at all - and every hand
step is one somebody gets wrong at 2 a.m. on a board that already stopped.

WHAT IT REFUSES, each naming itself and exiting non-zero:

    --restore without --yes ........... writing flash is not a dry run
    chip family mismatch .............. an S3 image on a WROOM-32 is a brick
    target flash smaller than image ... a 4 MB part cannot hold an 8 MB layout
    manifest sha256 disagreement ...... not the image that was verified
    implausible image length .......... a whole flash is a power-of-two MB
    partition table will not parse .... or does not match the repo's own CSV
    esptool missing, or older than 4.6  `read_flash 0 ALL` needs 4.6
    --patch-id against another chip ... the MAC has to agree with the id asked for
    --backup with no relay evidence ... below, and it is the important one

MOVING A CLONE ONTO A NEW CHIP: `--patch-id`, THE PREFERRED WAY.
A byte-identical clone does NOT come up as a working garden - loadFile() refuses
the whole document when `id` != ESP.getEfuseMac() % 0x10000, so the board raises
the setup AP instead. Rather than re-onboarding, `--patch-id <hex>` rewrites that
four-character id inside the image's own config.json and writes ONLY the
filesystem partition, so the board boots as the garden with the history, the
trained model and the calibration intact. Confirmed on 2026-09-26.
flash_image.patch_config_id() holds the three facts that make it safe and CHECKS
every one, then proves the edit surgical before a byte is written. Read the new
id off the board's own `ID: xxxx` line; the MAC arithmetic only predicts it.

RELAY SAFETY IS THE ONE REFUSAL ABOUT THE GARDEN RATHER THAN THE IMAGE.
Entering download mode stops the app and leaving it is a reset; on these
active-low boards a reset floats every relay GPIO and pulses every pump for the
length of a boot, with no task left to switch it off at the 30 s ceiling. So
`--backup` will not open a port until something says no relay is energised:
either `--device <host>`, which logs in and reads `/data.json`, or an explicit
`--no-relay-check`, which prints what it is trusting.

THE IMAGE IS A CREDENTIAL. Flash encryption is not enabled on these boards, so
`/config.json` is in the dump verbatim - Wi-Fi password, OTA/admin password,
ThingsBoard token - beside `/users.json`'s hashes and `/sessions.json`'s live
bearer tokens. Nothing here prints a value out of a recovered config except the
`id`, which it needs to warn about a clone. `backups/` is gitignored, which
stops `git add .` and nothing else.

Standard library only. `device_http.py` is reused for the login.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

# The byte layout lives next door, on the seam its docstring describes:
# this file knows the serial port, flash_image.py knows the format.
from flash_image import (APP_DESC_MAGIC, APP_DESC_OFFSET, BLOCK, FAMILIES,
                         FILENAME_RE, Fatal, IMAGE_MAGIC, MB, OTA_SEQ_BLANK,
                         PART_TABLE_MAX, PART_TABLE_OFFSET, Partition,
                         _number, build_partition_table, chip_refusal,
                         coredump_report, filesystem_report,
                         flash_size_refusal, id_from_mac, identify_family,
                         image_length_refusal, manifest_refusal,
                         parse_app_desc, parse_otadata, parse_partition_csv,
                         parse_partition_table, parts_dir, patch_config_id,
                         print_clone_warnings, print_report,
                         recover_config_id, running_slot, scan_versions,
                         self_test_checks, sidecar_path, split_partitions,
                         table_mismatch, used_blocks, verify_image)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT_DIR = ROOT / "backups" / "flash"

# PlatformIO is not on PATH on Windows, so both are resolved at runtime and
# REFUSED BY NAME when absent. Assuming a PATH is how a tool comes to report
# "esptool: command not found" about a board that was fine.
ESPTOOL_REL = Path(".platformio") / "packages" / "tool-esptoolpy" / "esptool.py"
PENV_PYTHON_REL = Path(".platformio") / "penv" / "Scripts" / "python.exe"

# `read_flash 0 ALL` - read to the end of the detected part without naming a
# length - landed in esptool 4.6; older ones parse ALL as a length and fail.
ESPTOOL_ALL_SINCE = (4, 6)
BAUD = 921600

# MEASURED on 2026-09-26: 8 388 608 B read in 109.9 s and written in 39.9 s
# (compressed to 2 039 017). One board, one baud, one cable - so --plan quotes
# these as an estimate and says so.
READ_BYTES_PER_SEC = 8388608 / 109.9
WRITE_BYTES_PER_SEC = 8388608 / 39.9
RESET_SECONDS = 6


# -------------------------------------------------------------------------
# Refusals about the TOOL and the garden. The image's own are next door.
# -------------------------------------------------------------------------


def confirmation_refusal(yes):
    """Refusal: --restore without --yes."""
    if not yes:
        return ("--restore rewrites the whole flash, the bootloader and the"
                " partition table included; pass --yes once you mean it")
    return None


def relay_evidence_refusal(device, no_relay_check):
    """Refusal: --backup with nothing saying the pumps are off."""
    if device or no_relay_check:
        return None
    return ("--backup needs evidence that no relay is energised: pass --device"
            " <host> so it can read /data.json, or --no-relay-check to say you"
            " have checked by hand")


def relay_state_refusal(data):
    """Refusal: /data.json says a relay is on, or cannot say at all."""
    relays = data.get("Relays") if isinstance(data, dict) else None
    if not isinstance(relays, list):
        return ("/data.json carried no Relays array, so it cannot show the pumps"
                " are off; a device answering without it is not one this check"
                " understands")
    energised = [relay for relay in relays if _relay_is_on(relay)]
    if energised:
        return ("%d relay(s) energised: %s - wait for them or stop them, because"
                " entering download mode stops the app that would"
                % (len(energised),
                   ", ".join("%s (remaining %s)"
                             % (relay.get("name", relay.get("index")),
                                relay.get("remaining", "?"))
                             for relay in energised)))
    return None


def _relay_is_on(relay):
    """Truthiness of `on`, coerced. bool("0") is True, which would invert this."""
    if not isinstance(relay, dict):
        return False
    value = relay.get("on", 0)
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    text = str(value).strip()
    try:
        return float(text) != 0
    except ValueError:
        return text.lower() not in ("", "false", "off", "no")


def esptool_version_refusal(version, needs_all):
    """Refusal: too old for `read_flash 0 ALL`."""
    if needs_all and version[:2] < ESPTOOL_ALL_SINCE:
        return ("esptool %s is too old: `read_flash 0 ALL` needs %d.%d or newer,"
                " and an older one parses ALL as a length"
                % (".".join(str(part) for part in version),
                   ESPTOOL_ALL_SINCE[0], ESPTOOL_ALL_SINCE[1]))
    return None


# ---------------------------------------------------------------------------
# esptool, and the device side of the relay check
# ---------------------------------------------------------------------------


def resolve_esptool(needs_all=True, override=None):
    """(python, esptool, version), or Fatal naming what is missing.

    `esptool.py version` opens no serial port. The penv python is the
    interpreter esptool's dependencies are installed against.
    """
    home = Path(os.path.expanduser("~"))
    esptool = Path(override) if override else home / ESPTOOL_REL
    python = home / PENV_PYTHON_REL
    if not esptool.is_file():
        raise Fatal("no esptool at %s - install the PlatformIO esp32 platform,"
                    " or pass --esptool" % esptool)
    if not python.is_file():
        python = Path(sys.executable)
    try:
        output = subprocess.run([str(python), str(esptool), "version"],
                                capture_output=True, text=True,
                                timeout=120).stdout
    except (OSError, subprocess.SubprocessError) as error:
        raise Fatal("cannot run %s: %s" % (esptool, error))
    match = re.search(r"v?(\d+)\.(\d+)(?:\.(\d+))?", output)
    if not match:
        raise Fatal("cannot read a version out of `esptool.py version`: %r"
                    % output.strip()[:200])
    version = (int(match.group(1)), int(match.group(2)),
               int(match.group(3) or 0))
    problem = esptool_version_refusal(version, needs_all)
    if problem:
        raise Fatal(problem)
    return str(python), str(esptool), version


def read_command(python, esptool, chip, port, destination):
    return [python, esptool, "--chip", chip, "--port", port, "--baud", str(BAUD),
            "--before", "default_reset", "--after", "hard_reset",
            "read_flash", "0", "ALL", str(destination)]


def write_command(python, esptool, chip, port, image, offset="0x0"):
    # THE THREE `keep` FLAGS ARE LOAD-BEARING. Writing at offset 0 without them
    # lets esptool patch the bootloader's own flash-size/mode/freq bytes to what
    # it detected, and the clone stops being byte-identical to the dump - which
    # is the one property the whole readback check rests on. They are passed at
    # every offset and not only at 0, because `keep` at an offset that carries no
    # image header is simply a no-op and one command shape is one thing to check.
    return [python, esptool, "--chip", chip, "--port", port, "--baud", str(BAUD),
            "--before", "default_reset", "--after", "hard_reset", "write_flash",
            "--flash_size", "keep", "--flash_mode", "keep",
            "--flash_freq", "keep", offset, str(image)]


def run_esptool(command, echo=True):
    """Run it, echoing as it goes, and return the output for banner parsing.

    esptool draws progress with carriage returns, so this reads chunks rather
    than lines: a line-buffered read shows nothing for two minutes and then
    everything at once, which on a 110 s read looks exactly like a hang.
    """
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    chunks = []
    while True:
        chunk = process.stdout.read(512)
        if not chunk:
            break
        text = chunk.decode("utf-8", "replace")
        chunks.append(text)
        if echo:
            sys.stdout.write(text)
            sys.stdout.flush()
    process.stdout.close()
    return process.wait(), "".join(chunks)


def parse_banner(text):
    """chip family, MAC and detected flash size out of esptool's own banner."""
    banner = {"chip_text": None, "family": None, "mac": None,
              "flash_bytes": None}
    match = re.search(r"Chip is (\S+)", text)
    if match:
        banner["chip_text"] = match.group(1)
        banner["family"] = family_of(match.group(1))
    match = re.search(r"MAC:\s*([0-9a-fA-F:]{11,})", text)
    if match:
        banner["mac"] = match.group(1).lower()
    match = re.search(r"[Dd]etected flash size:\s*(\d+)\s*(MB|KB)", text)
    if match:
        banner["flash_bytes"] = int(match.group(1)) * (
            MB if match.group(2) == "MB" else 1024)
    return banner


def family_of(chip_text):
    """"ESP32-S3 (QFN56)" -> esp32s3, "ESP32-D0WD-V3" -> esp32."""
    upper = chip_text.upper()
    for family, prefix, _csv, _nominal in FAMILIES:
        if family != "esp32" and upper.startswith(prefix):
            return family
    return "esp32" if upper.startswith("ESP32") else None


def read_device_state(host, user, password):
    """ONE login, /data.json, one logout. Returns (data, hostname).

    The credential comes from --password or ESP_GARDEN_PASSWORD and DELIBERATELY
    never from data/config.json, which other tools in scripts/ do read: this one
    writes a file next to an image that already contains every secret the device
    has, and giving it a habit of opening the repo's credential store as well
    buys nothing. An operator running --backup is at the keyboard anyway.

    READ-ONLY, and it stays that way: one GET of /data.json, no POST anywhere.
    If a future version ever does reach for /control - to stop a relay it found
    running, say - note that the endpoint takes the WORD and not a number:
    `mqtt=disable` / `mqtt=enable`, where `mqtt=0` is silently ignored. A write
    that is silently ignored while the tool reports success is the shape of
    failure this whole file is built to avoid, so it is written down here rather
    than rediscovered.
    """
    from device_http import Device  # kept local so --self-test imports nothing

    if not password:
        raise Fatal("no password: pass --password or set ESP_GARDEN_PASSWORD"
                    " (this tool never reads data/config.json)")
    device = Device(host)
    try:
        device.login(user, password)
        data = json.loads(device._request("/data.json"))
    finally:
        device.logout()
    status = data.get("Status", {}) if isinstance(data, dict) else {}
    return data, status.get("Hostname")


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------




def cmd_verify(args):
    report = verify_image(args.verify)
    print_report(report)
    if args.patch_id:
        # A DRY RUN of the id patch, and the reason it is worth having on the
        # offline subcommand: every refusal patch_config_id() can raise is a
        # property of the image, so all of them can fire before a board is on
        # the bench at all.
        _patched, evidence = patch_config_id(Path(args.verify).read_bytes(),
                                             args.patch_id)
        print()
        print_patch_evidence(evidence)
        print("  (--verify, so nothing was written)")
    if report.failed():
        print("\n%d check(s) FAILED: %s"
              % (len(report.failed()), ", ".join(report.failed())))
        return 1
    print("\nall %d checks passed" % len(report.checks))
    return 0


def print_patch_evidence(evidence):
    """What the patch checked, so an operator can see it rather than trust it."""
    print("id patch  %s -> %s at flash %#x, %d of 4 bytes change (%s)"
          "\n          the literal occurs exactly once; length unchanged; %d"
          " top-level\n          fields re-parsed and identical; nothing outside"
          " the id moved\n          writes ONLY %s (%#x, %d KB), so the app is"
          " untouched and a\n          mistake costs a mount that reformats,"
          " recoverable by writing\n          the pristine partition back"
          % (evidence["old"], evidence["new"], evidence["offset"],
             evidence["bytes_changed"],
             ", ".join("%#x" % at for at in evidence["offsets"]),
             evidence["fields_compared"], evidence["partition"],
             evidence["partition_offset"], evidence["partition_size"] // 1024))


def cmd_backup(args):
    problem = relay_evidence_refusal(args.device, args.no_relay_check)
    if problem:
        raise Fatal(problem)
    hostname = args.label
    if args.device:
        data, reported = read_device_state(args.device, args.user, args.password)
        problem = relay_state_refusal(data)
        if problem:
            raise Fatal(problem)
        hostname = hostname or reported
        print("relay check: %s reports every relay idle" % args.device)
    else:
        print("\n%s\n--no-relay-check: NOTHING HERE KNOWS WHETHER A PUMP IS"
              " RUNNING.\nDownload mode stops the app and leaving it is a reset."
              " On these\nactive-low boards a reset floats every relay GPIO and"
              " pulses every\npump for the length of a boot, with no task left to"
              " switch it off\nat the 30 s ceiling. This trusts that you have"
              " looked at the board\nor at /data.json yourself.\n%s\n"
              % ("!" * 72, "!" * 72))

    python, esptool, version = resolve_esptool(True, args.esptool)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    destination = out_dir / ("%s-%s-full.bin"
                             % (hostname or args.chip,
                                time.strftime("%Y%m%d-%H%M%S", time.localtime())))
    command = read_command(python, esptool, args.chip, args.port, destination)
    print("$ " + " ".join(command))
    code, output = run_esptool(command)
    if code != 0 or not destination.is_file():
        raise Fatal("esptool exited %d; no image was written" % code)

    # The read's OWN banner carries chip, MAC and flash size, so nothing runs
    # flash_id first: one port open is one reset fewer on a relay board.
    banner = parse_banner(output)
    blob = destination.read_bytes()
    report = verify_image(destination, blob=blob)
    print()
    print_report(report)

    manifest = {
        "image": destination.name, "sha256": report.digest,
        "bytes": report.length, "chip": banner["chip_text"],
        "family": banner["family"], "mac": banner["mac"],
        "predicted_id": id_from_mac(banner["mac"]) if banner["mac"] else None,
        "config_id": report.config_id["id"] if report.config_id else None,
        "detected_flash_bytes": banner["flash_bytes"], "esptool":
            ".".join(str(part) for part in version),
        "taken_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "taken_local": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime()),
        "partition_csv_family": report.family,
        "running_partition": report.running_label,
        "checks_failed": report.failed(), "findings": report.findings}
    sidecar_path(destination).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("\nmanifest  %s" % sidecar_path(destination))
    if args.split:
        target = parts_dir(destination)
        target.mkdir(parents=True, exist_ok=True)
        print("parts     %s (%d files)"
              % (target, split_partitions(blob, report.partitions, target)))
    if report.failed():
        print("\nTHE IMAGE WAS WRITTEN BUT %d CHECK(S) FAILED"
              % len(report.failed()))
        return 1
    return 0


def cmd_restore(args):
    problem = confirmation_refusal(args.yes)
    if problem:
        raise Fatal(problem)
    image = Path(args.restore)
    report = verify_image(image)
    print_report(report)
    if report.failed():
        raise Fatal("the image failed %d structure check(s): %s - refusing to"
                    " write it"
                    % (len(report.failed()), ", ".join(report.failed())))
    python, esptool, _version = resolve_esptool(not args.no_readback,
                                                args.esptool)

    # flash_id rather than the write's own banner: the chip and flash-size
    # refusals have to fire BEFORE anything is erased.
    probe = [python, esptool, "--chip", report.family, "--port", args.port,
             "--before", "default_reset", "--after", "no_reset", "flash_id"]
    print("\n$ " + " ".join(probe))
    code, output = run_esptool(probe)
    if code != 0:
        raise Fatal("esptool could not identify the part on %s (exit %d)"
                    % (args.port, code))
    banner = parse_banner(output)
    for problem in (chip_refusal(banner["family"], report.family),
                    flash_size_refusal(banner["flash_bytes"], report.length)):
        if problem:
            raise Fatal(problem)
    predicted = id_from_mac(banner["mac"]) if banner["mac"] else None
    if args.patch_id:
        if predicted and args.patch_id.strip().lower() != predicted:
            raise Fatal("--patch-id %s but this chip's MAC predicts id %s; read"
                        " the id off the board's own `ID: xxxx` boot line, which"
                        " is by definition the number loadFile() compares"
                        " against, and pass that"
                        % (args.patch_id, predicted))
        return _restore_patched_filesystem(args, report, python, esptool, image)
    print_clone_warnings(report.config_id["id"] if report.config_id else None,
                         predicted)

    command = write_command(python, esptool, report.family, args.port, image)
    print("$ " + " ".join(command))
    code, _output = run_esptool(command)
    if code != 0:
        raise Fatal("esptool exited %d; the flash is in an unknown state and the"
                    " board has no working bootloader until a write succeeds"
                    % code)
    if args.no_readback:
        print("\n--no-readback: the verdict is now esptool's own 'Hash of data"
              "\nverified', which compares what it wrote against what it sent."
              "\nWhat is NOT established is that the part RETAINED it - only a"
              "\nsecond full read proves the flash holds the source image rather"
              "\nthan the writer's arithmetic about it. It costs ~%.0f s."
              % (report.length / READ_BYTES_PER_SEC))
        return 0

    scratch = image.with_name(image.name + ".readback")
    try:
        command = read_command(python, esptool, report.family, args.port, scratch)
        print("\n$ " + " ".join(command))
        code, _output = run_esptool(command)
        if code != 0 or not scratch.is_file():
            raise Fatal("the readback failed (exit %d); the write reported"
                        " success and is unconfirmed" % code)
        digest = hashlib.sha256(scratch.read_bytes()).hexdigest()
    finally:
        if scratch.is_file():
            scratch.unlink()

    # PASS/FAIL IS THE HASH, NEVER THE EXIT CODE: esptool exits 0 on a write it
    # verified against its own buffer, which is a different claim.
    print("\nsource   %s\nreadback %s" % (report.digest, digest))
    if digest != report.digest:
        print("\nREADBACK MISMATCH: the flash does not hold this image.")
        return 1
    print("\nthe flash is byte-identical to the source image")
    return 0


def _restore_patched_filesystem(args, report, python, esptool, image):
    """Write ONLY the filesystem partition, with the config id patched.

    This is the procedure that was actually used, and it is preferred over
    letting the clone boot into the setup portal: the board comes up as the
    garden on its first boot with the history, the trained model, the
    calibration and the names intact.

    The blast radius is the point. The bootloader, the partition table and both
    app slots are not written at all, so the worst case is a filesystem that
    will not mount - and FILESYSTEM.begin(true) then formats it, which costs the
    history and the model and is recovered by writing the pristine partition
    back from the same dump. A whole-image write has no such floor.
    """
    patched, evidence = patch_config_id(image.read_bytes(), args.patch_id)
    print()
    print_patch_evidence(evidence)
    start, size = evidence["partition_offset"], evidence["partition_size"]
    label = evidence["partition"]
    region = patched[start:start + size]
    digest = hashlib.sha256(region).hexdigest()

    scratch = image.with_name(image.name + ".patched-fs")
    readback = image.with_name(image.name + ".readback")
    scratch.write_bytes(region)
    try:
        command = write_command(python, esptool, report.family, args.port,
                                scratch, "%#x" % start)
        print("\n$ " + " ".join(command))
        code, _output = run_esptool(command)
        if code != 0:
            raise Fatal("esptool exited %d writing %s; the app is untouched, so"
                        " the board still boots - write the pristine partition"
                        " back from this dump before trying again" % (code, label))
        if args.no_readback:
            print("\n--no-readback: the verdict is esptool's own 'Hash of data")
            print("verified' against what it sent, not that the part retained it.")
            return 0
        command = [python, esptool, "--chip", report.family, "--port", args.port,
                   "--baud", str(BAUD), "--before", "default_reset",
                   "--after", "hard_reset", "read_flash", "%#x" % start,
                   "%#x" % size, str(readback)]
        print("\n$ " + " ".join(command))
        code, _output = run_esptool(command)
        if code != 0 or not readback.is_file():
            raise Fatal("the readback failed (exit %d); the write reported"
                        " success and is unconfirmed" % code)
        got = hashlib.sha256(readback.read_bytes()).hexdigest()
    finally:
        for leftover in (scratch, readback):
            if leftover.is_file():
                leftover.unlink()

    # PASS/FAIL IS THE HASH, NEVER THE EXIT CODE.
    print("\npatched  %s\nreadback %s" % (digest, got))
    if got != digest:
        print("\nREADBACK MISMATCH: %s does not hold the patched partition."
              % label)
        return 1
    print("\n%s is byte-identical to the patched partition, so the board should"
          "\nboot as the garden under id %s. Confirm on the serial console: an"
          "\n`ID: %s` line and NO 'Device ID does not match config file ID.'"
          % (label, evidence["new"], evidence["new"]))
    return 0


def cmd_plan(args):
    """Every command verbatim and every refusal that would fire, no port opened."""
    say = print
    say("flash_clone --plan: nothing below opens a serial port.\n")
    python = esptool = "<esptool>"
    try:
        python, esptool, version = resolve_esptool(True, args.esptool)
        say("esptool   %s (%s)" % (".".join(str(p) for p in version), esptool))
    except Fatal as error:
        say("esptool   WOULD REFUSE: %s" % error)
    say("port      %s   baud %d   chip %s" % (args.port, BAUD, args.chip))

    report, length = None, 8 * MB
    image = args.restore or args.verify
    if image:
        try:
            report = verify_image(image)
            length = report.length
            say("image     %s: %d MB, family %s, sha256 %s"
                % (image, length // MB, report.family, report.digest[:16]))
            if report.failed():
                say("          WOULD REFUSE: failed %s"
                    % ", ".join(report.failed()))
            if args.patch_id:
                _patched, evidence = patch_config_id(Path(image).read_bytes(),
                                                     args.patch_id)
                say("")
                print_patch_evidence(evidence)
        except Fatal as error:
            say("image     %s\n          WOULD REFUSE: %s" % (image, error))

    say("\nBACKUP would run\n  $ %s\n  ~%.0f s for %d MB plus ~%d s of reset,"
        " then verify, a .manifest.json\n  sidecar and a -parts/ split."
        " Refusals checked first:\n    relay evidence     %s"
        "\n    esptool >= %d.%d      needed for `read_flash 0 ALL`"
        % (" ".join(read_command(python, esptool, args.chip, args.port,
                                 Path(args.out_dir) / "<host>-<stamp>-full.bin")),
           length / READ_BYTES_PER_SEC, length // MB, RESET_SECONDS,
           relay_evidence_refusal(args.device, args.no_relay_check)
           or "ok (%s)" % (args.device or "--no-relay-check"),
           ESPTOOL_ALL_SINCE[0], ESPTOOL_ALL_SINCE[1]))

    say("\nRESTORE would run\n  $ %s\n  ~%.0f s for %d MB, then a full readback"
        " (~%.0f s) and sha256 against\n  the source. With --patch-id it writes"
        " the filesystem partition only.\n  Refusals checked first:"
        "\n    --yes              %s\n    image structure    %s"
        "\n    manifest sha256    checked when a .manifest.json sidecar exists"
        "\n    chip family        from esptool's flash_id banner, at connect"
        "\n    target flash size  from the same banner, at connect"
        "\n    --patch-id vs MAC  the two must agree, checked at connect"
        % (" ".join(write_command(python, esptool, args.chip, args.port,
                                 image or "<image>")),
           length / WRITE_BYTES_PER_SEC, length // MB,
           length / READ_BYTES_PER_SEC,
           confirmation_refusal(args.yes) or "ok",
           "ok" if report and not report.failed()
           else "verified offline before the port is opened"))
    say("\n  Both durations are MEASURED on one board at one baud on 2026-09-26"
        "\n  (8 388 608 B read in 109.9 s, written in 39.9 s); for any other"
        " board\n  they are an estimate, not a specification.")
    return 0


# Self-test: offline, no board, no credential
# ---------------------------------------------------------------------------


def self_test():
    """Every check this tool can make without a board, a port or a credential."""
    checks = []
    FF = b"\xff"

    def ok(name, got, want=True): checks.append((name, got == want, got, want))

    def why(thunk):
        """A Fatal's own sentence, or None - so a check can assert WHY."""
        try:
            thunk()
        except Fatal as error:
            return str(error)
        return None

    fired = lambda text: text is not None           # a refusal returned a reason
    allowed = lambda text: text is None

    # --- the CSV parser, and the table parser round-tripping through it ------
    csv8 = parse_partition_csv(ROOT / "partitions" / "esp_garden_8mb.csv")
    csv4 = parse_partition_csv(ROOT / "partitions" / "esp_garden_4mb.csv")
    ok("the 8 MB CSV declares six partitions", len(csv8), 6)
    ok("spiffs is data/0x82 at 0x590000 sized 0x260000", csv8[4].key(),
       ("spiffs", 1, 0x82, 0x590000, 0x260000, 0))
    ok("app1 is app/ota_1", csv8[3].key()[:3], ("app1", 0, 0x11))
    ok("the 4 MB CSV puts app0 at 0x10000/0x1B0000",
       (csv4[2].offset, csv4[2].size), (0x10000, 0x1B0000))
    ok("a K suffix scales", _number("64K", "size", ""), 65536)
    ok("an M suffix scales", _number("2M", "size", ""), 2 * MB)
    ok("a decimal size parses", _number("4096", "size", ""), 4096)
    ok("a blank size is refused", fired(why(lambda: _number("", "s", "row"))))
    ok("a missing CSV is refused",
       fired(why(lambda: parse_partition_csv(ROOT / "partitions" / "no.csv"))))

    # A table built from the CSV and parsed back: this is what checks the
    # 32-byte layout is right rather than merely self-consistent.
    def table_image(rows, size=None):
        blob = bytearray(FF * (size or PART_TABLE_OFFSET + PART_TABLE_MAX))
        blob[0] = IMAGE_MAGIC
        built = build_partition_table(rows)
        blob[PART_TABLE_OFFSET:PART_TABLE_OFFSET + len(built)] = built
        return bytes(blob)

    keys = lambda rows: [row.key() for row in rows]
    parsed = parse_partition_table(table_image(csv8))
    ok("the table parser round-trips the 8 MB CSV", keys(parsed), keys(csv8))
    ok("the md5 terminator ends the table, not an entry", len(parsed), 6)
    ok("a table equal to its CSV passes", table_mismatch(parsed, csv8, "x"), None)
    moved = [Partition("nvs", 1, 2, 0xA000, 0x5000)] + csv8[1:]
    ok("a moved offset is refused", "nvs" in table_mismatch(parsed, moved, "x"))
    ok("a different partition count is refused",
       "declares" in table_mismatch(parsed, csv8[:5], "x"))
    ok("a table with no magic is refused",
       fired(why(lambda: parse_partition_table(b"\x00" * 0x9000))))
    ok("the 8 MB table is identified as esp32s3",
       identify_family(parsed)[0], "esp32s3")
    ok("the 4 MB table is identified as esp32",
       identify_family(parse_partition_table(table_image(csv4)))[0], "esp32")
    hybrid = csv8[:4] + [Partition("spiffs", 1, 0x82, 0x590000, 0x100000)]
    ok("a table matching neither CSV is not identified",
       identify_family(parse_partition_table(table_image(hybrid)))[0], None)

    # --- the id arithmetic, on all three boards this project has had ---------
    ok("80:b5:4e:.. is b580", id_from_mac("80:b5:4e:e8:ac:88"), "b580")
    ok("24:62:ab:.. is 6224", id_from_mac("24:62:ab:fa:08:18"), "6224")
    ok("d0:cf:13:.. is cfd0", id_from_mac("d0:cf:13:00:00:00"), "cfd0")
    ok("a dash-separated MAC is the same", id_from_mac("24-62-AB-FA-08-18"),
       "6224")
    ok("the low byte is the FIRST printed, not the last",
       id_from_mac("01:02:03:04:05:06"), "0201")
    ok("garbage is refused", fired(why(lambda: id_from_mac("zz"))))

    # --- esp_app_desc_t, and the version scan that actually names a firmware -
    app = bytearray(b"\x00" * (0x1000 + 256))
    app[0x1000] = IMAGE_MAGIC
    base = 0x1000 + APP_DESC_OFFSET
    struct.pack_into("<I", app, base, APP_DESC_MAGIC)
    struct.pack_into("<I", app, base + 4, 7)
    for at, value in ((16, b"1.2.3"), (48, b"garden"), (80, b"12:00:00"),
                      (96, b"Mar  5 2024"), (112, b"v4.4.7")):
        app[base + at:base + at + len(value)] = value
    desc = parse_app_desc(bytes(app), 0x1000)
    ok("the descriptor's fields are read in order",
       (desc["version"], desc["project_name"], desc["time"], desc["date"],
        desc["idf_ver"], desc["secure_version"]),
       ("1.2.3", "garden", "12:00:00", "Mar  5 2024", "v4.4.7", 7))
    ok("no magic means no descriptor",
       parse_app_desc(b"\x00" * 0x2000, 0x1000), None)
    ok("a truncated partition means no descriptor",
       parse_app_desc(b"\xe9" * 64, 0), None)
    found = scan_versions(
        b"\xff\x00Initializing ESP Garden 2.20.0...\x00"
        b'\x00{"firmware":"2.20.0","ap":"x"}\x00'
        b"\x00255.255.255.0<->240)\x00\x002.19.0\x00")
    seen = [version for version, _count, _named in found]
    ok("the anchored version ranks first", found[0][0], "2.20.0")
    ok("both of this firmware's own strings name it", len(found[0][2]), 2)
    ok("an unanchored version is still reported", "2.19.0" in seen)
    ok("a dotted quad is NOT reported as a version",
       any(v.startswith("255.") or v == "55.255.255" for v in seen), False)
    ok("a region with no version reports none", scan_versions(FF * 64), [])

    # --- otadata ------------------------------------------------------------
    blank = OTA_SEQ_BLANK
    ok("seq 3 and 4 elect slot 1", running_slot([3, 4])[0], 1)
    ok("the order of the copies does not matter", running_slot([4, 3])[0], 1)
    ok("seq 1 alone elects slot 0", running_slot([1, blank])[0], 0)
    ok("one blank copy says so",
       "one otadata copy is blank" in running_slot([1, blank])[1])
    ok("seq 2 alone elects slot 1", running_slot([2, blank])[0], 1)
    ok("both blank elects nothing", running_slot([blank, blank])[0], None)
    ok("both blank explains the fallback",
       "first app partition" in running_slot([blank, blank])[1])
    ok("a zero seq is unwritten, not slot -1", running_slot([0, 0])[0], None)
    ota = bytearray(FF * 0x12000)
    for at, seq in ((0xE000, 3), (0xF000, 4)):
        struct.pack_into("<I", ota, at, seq)
        struct.pack_into("<I", ota, at + 24, 2)
    copies, (slot, _why) = parse_otadata(
        bytes(ota), Partition("otadata", 1, 0, 0xE000, 0x2000))
    ok("the second copy is half a partition along", copies[1]["offset"], 0xF000)
    ok("a copy's state is named", copies[0]["state_name"], "valid")
    ok("the real 2026-09-26 otadata elects ota_1", slot, 1)

    # --- blank regions, filenames, the filesystem, the core dump ------------
    ok("an all-0xFF region reports 0 used blocks",
       used_blocks(FF * (4 * BLOCK)), (0, 4))
    ok("one written byte marks one block",
       used_blocks(FF * BLOCK + b"\x00" + FF * (BLOCK - 1)), (1, 2))
    ok("a short trailing block still counts", used_blocks(b"\x00" * 10), (1, 1))
    # THE REGRESSION THIS TOOL WAS WRITTEN AROUND: `js` ahead of `json` in the
    # alternation silently renames the credential store to a web asset.
    names = sorted({m.group().decode() for m in FILENAME_RE.finditer(
        b"\x00users.json\x00config.json\x00devices.js.gz\x00hist7.bin\x01"
        b"\x00index.html.gz\x00thingsboard.pem\x00sessions.json\x00")})
    ok("the filename regex does not truncate json to js",
       [n for n in names if n[0] in "us"], ["sessions.json", "users.json"])
    ok("a .gz name keeps its .gz", "devices.js.gz" in names)
    ok("a name followed by an alphanumeric byte survives", "hist7.bin" in names)
    ok("a .pem is seen", "thingsboard.pem" in names)
    ok("a littlefs region is known by its block-1 magic",
       filesystem_report(FF * BLOCK + b"\x00" * 8 + b"littlefs"
                         + b"\x00" * (BLOCK - 16))["magic_block"], 1)
    ok("a region with no magic reports none",
       filesystem_report(FF * (2 * BLOCK))["magic_block"], None)
    core = coredump_report(b"\x04\x2a\x00\x00" + b"\x00" * 16 + b"\x7fELF"
                           + b"\x01" * 40 + FF * 100)
    ok("a core dump's ELF is found at offset 20", core["elf"])
    ok("a core dump's declared length is read", core["declared"], 0x2A04)
    ok("an erased coredump reports nothing written", coredump_report(FF * 1024),
       {"written": 0, "declared": None, "elf": False})

    # --- config recovery, which must surrender the id and nothing else ------
    recovered = recover_config_id(
        b'\xff\xff{"id":"b580","hostname":"espgarden-s3","wifi":'
        b'{"ssid":"x","password":"secret"},"io":{"relays":[]}}\xff')
    ok("the config id is recovered", recovered["id"], "b580")
    ok("only key NAMES come back, never values", recovered["keys"],
       ["hostname", "id", "io", "wifi"])
    ok("a brace inside a string does not end the document",
       recover_config_id(b'{"id":"aaaa","hostname":"a}b","z":1}')["id"], "aaaa")
    ok("an unrecoverable region gives None", recover_config_id(FF * 4096), None)

    # --- refusals -----------------------------------------------------------
    ok("an 8 MB image on a 4 MB target is refused",
       "does not fit" in flash_size_refusal(4 * MB, 8 * MB))
    ok("8 MB on an 8 MB target is allowed",
       allowed(flash_size_refusal(8 * MB, 8 * MB)))
    ok("8 MB on a 16 MB target is allowed",
       allowed(flash_size_refusal(16 * MB, 8 * MB)))
    ok("an unreported flash size is refused",
       fired(flash_size_refusal(None, 8 * MB)))
    ok("a chip family mismatch is refused",
       "esp32s3" in chip_refusal("esp32", "esp32s3"))
    ok("a matching chip family is allowed",
       allowed(chip_refusal("esp32s3", "esp32s3")))
    ok("an unnamed chip is refused", fired(chip_refusal(None, "esp32s3")))
    ok("--restore without --yes is refused", fired(confirmation_refusal(False)))
    ok("--restore with --yes proceeds", allowed(confirmation_refusal(True)))
    ok("a 3 MB image is refused as not a flash size",
       fired(image_length_refusal(3 * MB)))
    ok("a 2 490 368-byte partition is not a flash",
       fired(image_length_refusal(0x260000)))
    ok("an empty image is refused", fired(image_length_refusal(0)))
    for megabytes in (4, 8, 16):
        ok("%d MB is a plausible flash" % megabytes,
           allowed(image_length_refusal(megabytes * MB)))
    ok("esptool 4.5 is too old for ALL",
       "too old" in esptool_version_refusal((4, 5, 0), True))
    ok("esptool 4.6 is new enough",
       allowed(esptool_version_refusal((4, 6, 0), True)))
    ok("esptool 4.9 is new enough",
       allowed(esptool_version_refusal((4, 9, 0), True)))
    ok("esptool 3.3 is fine when ALL is not needed",
       allowed(esptool_version_refusal((3, 3, 0), False)))
    ok("--backup with no evidence is refused",
       fired(relay_evidence_refusal(None, False)))
    ok("--device is evidence enough",
       allowed(relay_evidence_refusal("h", False)))
    ok("--no-relay-check is evidence enough",
       allowed(relay_evidence_refusal(None, True)))
    ok("an idle garden passes the relay check", allowed(relay_state_refusal(
        {"Relays": [{"index": 0, "name": "Z1", "on": 0}, {"on": 0}]})))
    ok("an energised relay is refused", "energised" in relay_state_refusal(
        {"Relays": [{"index": 0, "name": "Z1", "on": 1, "remaining": 12}]}))
    ok('a string "1" is energised',
       fired(relay_state_refusal({"Relays": [{"on": "1"}]})))
    ok('a string "0" is NOT, though bool("0") is True',
       allowed(relay_state_refusal({"Relays": [{"on": "0"}]})))
    ok("a payload with no Relays array is refused",
       fired(relay_state_refusal({"Status": {}})))

    # --- the manifest refusal, against a real file --------------------------
    scratch = Path(os.environ.get("TEMP") or "/tmp") / "flash_clone_selftest"
    scratch.mkdir(parents=True, exist_ok=True)
    probe = scratch / "probe-full.bin"
    probe.write_bytes(b"\x00" * 16)
    digest = hashlib.sha256(probe.read_bytes()).hexdigest()
    write = lambda body: sidecar_path(probe).write_text(body)
    ok("no sidecar is nothing to disagree with",
       allowed(manifest_refusal(probe, digest)))
    write(json.dumps({"sha256": digest}))
    ok("a matching manifest is allowed", allowed(manifest_refusal(probe, digest)))
    write(json.dumps({"sha256": "00" * 32}))
    ok("a manifest sha256 mismatch is refused",
       "not the image that was verified" in manifest_refusal(probe, digest))
    write("{not json")
    ok("an unparseable manifest is refused", fired(manifest_refusal(probe, digest)))
    write(json.dumps({"bytes": 16}))
    ok("a manifest with no sha256 is not a refusal",
       allowed(manifest_refusal(probe, digest)))
    for leftover in (probe, sidecar_path(probe)): leftover.unlink()

    # --- the banner parser --------------------------------------------------
    banner = parse_banner(
        "esptool.py v4.9.0\nSerial port COM12\nConnecting....\n"
        "Chip is ESP32-S3 (QFN56) (revision v0.2)\n"
        "Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)\n"
        "MAC: 80:b5:4e:e8:ac:88\nDetected flash size: 8MB\n")
    ok("the S3 banner is parsed whole",
       (banner["family"], banner["mac"], banner["flash_bytes"]),
       ("esp32s3", "80:b5:4e:e8:ac:88", 8 * MB))
    wroom = parse_banner("Chip is ESP32-D0WD-V3 (revision v3.1)\n"
                         "MAC: 24:62:ab:fa:08:18\nDetected flash size: 4MB\n")
    ok("a WROOM-32 banner is the bare esp32 family", wroom["family"], "esp32")
    ok("its id follows from its MAC", id_from_mac(wroom["mac"]), "6224")
    ok("an S3 is not an esp32 by prefix", family_of("ESP32-S3"), "esp32s3")
    ok("an unknown chip is not guessed", family_of("RP2040"), None)
    ok("an empty banner yields nothing", parse_banner("")["family"], None)

    # --- the command lines, and the paths a backup writes to ---------------
    command = write_command("py", "esptool", "esp32s3", "COM5", "i.bin")
    for flag in ("--flash_size", "--flash_mode", "--flash_freq"):
        ok("the write command carries %s keep" % flag,
           command[command.index(flag) + 1], "keep")
    ok("the write command writes at 0x0", command[-2], "0x0")
    ok("the read command asks for ALL",
       read_command("py", "e", "esp32s3", "COM5", "o.bin")[-2], "ALL")
    ok("a write at an offset keeps the keep flags",
       write_command("py", "e", "esp32s3", "COM5", "f.bin", "0x590000")[-2],
       "0x590000")
    ok("the sidecar sits beside the image",
       sidecar_path(Path("/a/b-full.bin")).name, "b-full.manifest.json")
    ok("the parts directory follows the manual run's name",
       parts_dir(Path("/a/espgarden-s3-b580-1-full.bin")).name,
       "espgarden-s3-b580-1-parts")

    # verify_image() and patch_config_id() operate on a whole image, so their
    # checks live beside them in flash_image.py; the count below is the total.
    checks += self_test_checks()

    bad = [check for check in checks if not check[1]]
    for name, _passed, got, want in bad:
        print("FAIL  %s\n      got %r\n      want %r" % (name, got, want))
    print("flash_clone --self-test: %d checks, %d failed"
          % (len(checks), len(bad)))
    return 1 if bad else 0


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def build_parser():
    parser = argparse.ArgumentParser(
        description="Full-flash backup, restore and offline verification of an"
                    " esp-garden board.",
        epilog="With none of --backup/--restore/--verify/--self-test it prints"
               " --plan and opens no port.")
    add = parser.add_argument
    add("--backup", action="store_true",
        help="read the whole flash, verify it, write a manifest, split the parts")
    add("--restore", metavar="IMAGE",
        help="write a whole-flash image back (needs --yes)")
    add("--verify", metavar="IMAGE",
        help="offline structure checks, no port opened")
    add("--plan", action="store_true", help="print what would happen, no port")
    add("--self-test", action="store_true",
        help="offline checks, no board and no credential")
    add("--port", default="COM12",
        help="serial port (default COM12, the S3 carrier)")
    add("--chip", default="esp32s3",
        help="esptool --chip for a BACKUP; a restore takes the family from the"
             " image's own partition table")
    add("--esptool", help="override the esptool.py path")
    add("--out-dir", default=str(DEFAULT_OUT_DIR),
        help="where a backup lands (default backups/flash)")
    add("--label", help="name to build the filename from; taken from the"
                        " device's own hostname when --device is given")
    add("--no-split", dest="split", action="store_false",
        help="skip the per-partition -parts/ directory")
    add("--device", metavar="HOST",
        help="read /data.json here to prove no relay is on")
    add("--user", default="admin", help="login user")
    add("--password", default=os.environ.get("ESP_GARDEN_PASSWORD"),
        help="login password, or set ESP_GARDEN_PASSWORD")
    add("--no-relay-check", action="store_true",
        help="back up without checking the relays, and be told what that trusts")
    add("--patch-id", metavar="HEX",
        help="rewrite the config id to these four hex characters. On --restore"
             " this writes ONLY the filesystem partition, so the clone boots as"
             " the garden instead of into the setup portal; on --verify and"
             " --plan it is a dry run")
    add("--yes", action="store_true", help="confirm a --restore")
    add("--no-readback", action="store_true",
        help="skip the post-write read and sha256 compare")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.self_test:
            return self_test()
        if args.backup and args.restore:
            raise Fatal("--backup and --restore are not one operation")
        if args.backup:
            return cmd_backup(args)
        if args.restore and not args.plan:
            return cmd_restore(args)
        if args.verify and not args.plan:
            return cmd_verify(args)
        return cmd_plan(args)
    except Fatal as error:
        print("flash_clone: %s" % error, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nflash_clone: interrupted. A half-written flash has no working"
              " bootloader; re-run --restore before power-cycling the board.",
              file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
