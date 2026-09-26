#!/usr/bin/env python3
"""What an esp-garden full-flash image IS: the format, parsed, and nothing else.

Split out of scripts/flash_clone.py when the pair crossed the 1000-line gate, on
the seam this repo already uses for drying_fit/drying_models and
history_export/history_archive: this half knows the BYTE LAYOUT, the tool half
knows the serial port. Nothing here opens a port, runs a subprocess or sends a
request, so every claim it makes can be checked offline against a dump already
on disk - which is the only way most of it CAN be checked, the alternative being
a live garden with real pumps.

WHAT IS PARSED HERE: the partition table (32-byte entries at 0x8000, magic
0x50AA, ended by the 0xEBEB md5 entry, compared against the repo's OWN
partitions/*.csv rather than a hardcoded copy); esp_app_desc_t, whose `version`
field DOES NOT hold FW_VERSION on an Arduino build - see parse_app_desc(); the
version STRINGS, which do, and which is how the two app slots were told apart as
2.19.0 and 2.20.0; otadata, and the slot it elects; a filesystem region's
superblock magic and filenames, NOT an unpack - filesystem_report() says why;
the core dump, a 20-byte header then an ELF, which is a FINDING; the config id
against mac[0] | mac[1] << 8; and patch_config_id(), the four-character edit
that turns a dump into a clone that boots as the garden.

Every layout was read off the 2026-09-26 dump of board b580 and agrees with it.
self_test_checks() covers the two whole-image operations; the rest is pinned by
`flash_clone.py --self-test`, the one entry point - this module has no CLI.

THE ONE RULE ABOUT SECRETS. A full flash image holds /config.json verbatim -
flash encryption is not enabled on these boards - so the Wi-Fi password, the
ThingsBoard token, users.json's hashes and sessions.json's live bearer tokens
are all plain text in it. recover_config_id() returns the `id` and the key
NAMES, never a value; patch_config_id() compares two whole documents without
letting either leave the function. Nothing here prints one.

Standard library only.
"""

from __future__ import annotations

import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MB = 1024 * 1024
BLOCK = 4096
PART_TABLE_OFFSET = 0x8000
PART_TABLE_MAX = 0xC00  # ESP-IDF's own ceiling: 95 entries plus the md5
ENTRY_MAGIC = 0x50AA
MD5_MAGIC = 0xEBEB
ENTRY_SIZE = 32
IMAGE_MAGIC = 0xE9  # ESP_IMAGE_HEADER_MAGIC: byte 0 of a bootloader or an app
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
FS_SUBTYPES = (0x82, 0x83)  # spiffs, littlefs - this firmware writes lfs to both
COREDUMP_SUBTYPE = 0x03
OTA_SEQ_BLANK = 0xFFFFFFFF

# The families this repo builds, each with the partition CSV that IS its
# expected table. Order matters: every suffixed family also starts with the
# string ESP32, so a bare prefix test calls an S3 a WROOM-32.
FAMILIES = [
    ("esp32s3", "ESP32-S3", "esp_garden_8mb.csv", 8 * MB),
    ("esp32", "ESP32", "esp_garden_4mb.csv", 4 * MB),
]

TYPE_NAMES = {"app": 0, "data": 1}
SUBTYPE_NAMES = {"factory": 0x00, "test": 0x20, "ota": 0x00, "phy": 0x01,
                 "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04, "efuse": 0x05,
                 "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81,
                 "spiffs": 0x82, "littlefs": 0x83}
SUBTYPE_NAMES.update({"ota_%d" % i: 0x10 + i for i in range(16)})
OTA_STATES = {0: "new", 1: "pending verify", 2: "valid", 3: "invalid",
              4: "aborted", OTA_SEQ_BLANK: "undefined"}

# A filename inside a raw filesystem region. THE ALTERNATION IS ORDERED
# LONGEST-FIRST ON PURPOSE. With `js` ahead of `json` the regex matches the
# shorter branch and stops - it is not anchored at the end - so `users.json`
# reports as `users.js`. That is the difference between "this dump holds the
# credential store" and "this dump holds a web asset", and it is how the
# hand-written notes for the 2026-09-26 dump got all four .json names wrong.
# A trailing (?![A-Za-z0-9]) looks like the same fix and is worse: it makes
# every .gz name backtrack to its stem whenever the next byte on flash happens
# to be alphanumeric, which drops hist7.bin and turns devices.js.gz into
# devices.js. The optional \.gz group handles both; a self-test check pins it.
FILENAME_RE = re.compile(rb"[A-Za-z0-9_.\-]{3,40}"
                         rb"\.(?:html|json|js|css|pem|ico|txt|bin)(?:\.gz)?")

# A version, not an octet of a dotted quad. The lookarounds reject the
# 255.255.255.0 in the firmware's own help text, which a bare \b\d+\.\d+\.\d+\b
# happily reports as a candidate version.
VERSION_RE = re.compile(r"(?<![\d.])(\d+\.\d+\.\d+)(?![\d.])")
PRINTABLE_RE = re.compile(rb"[ -~]{4,96}")

# This firmware's own strings, so a candidate can be reported as NAMED rather
# than merely found: src/main.cpp logs the first, the /data.json cache and the
# onboarding body carry the second.
VERSION_ANCHORS = [
    ("the boot log line", re.compile(r"Initializing ESP Garden (\d+\.\d+\.\d+)")),
    ('the "firmware" key', re.compile(r'"firmware"\s*:\s*"(\d+\.\d+\.\d+)"')),
]

CLONE_FIELDS = [
    ("id", "the new chip's efuse id, or ConfigFile::loadFile() refuses the whole"
           " document and the board raises the setup portal"),
    ("hostname", "two boards publishing one name over mDNS is a coin toss for"
                 " every tool that addresses them by name, which is the rule"),
    ("mqtt.username", "THE THINGSBOARD DEVICE ACCESS TOKEN. Two devices on one"
                      " token both write into the original's own telemetry"
                      " series, corrupting the garden's stored history. Make a"
                      " new device, or set mqtt.enabled false until you have"),
    ("ota.password", "the clone otherwise carries the garden's admin credential,"
                     " and sessions.json came across too - so a bearer token"
                     " stolen from the original is live on the clone"),
]


class Fatal(Exception):
    """Something the operator has to fix. Printed as a sentence, not a trace."""


# ---------------------------------------------------------------------------
# The partition table, from flash and from the repo's own CSV
# ---------------------------------------------------------------------------


class Partition:
    __slots__ = ("label", "kind", "subtype", "offset", "size", "flags")

    def __init__(self, label, kind, subtype, offset, size, flags=0):
        self.label, self.kind, self.subtype = label, kind, subtype
        self.offset, self.size, self.flags = offset, size, flags

    def key(self):
        return (self.label, self.kind, self.subtype, self.offset, self.size,
                self.flags)

    def is_app(self):
        return self.kind == 0

    def end(self):
        return self.offset + self.size

    def described(self):
        return "type=%d subtype=%#x off=%#x size=%#x flags=%d" % (
            self.kind, self.subtype, self.offset, self.size, self.flags)


def parse_partition_table(blob, offset=PART_TABLE_OFFSET):
    """Entries at `offset` until the md5 terminator or the first non-entry.

    Raises Fatal, because a table that will not parse is a refusal and not a
    finding: every later check addresses the image through this list.
    """
    entries = []
    for index in range(PART_TABLE_MAX // ENTRY_SIZE):
        raw = blob[offset + index * ENTRY_SIZE:][:ENTRY_SIZE]
        if len(raw) < ENTRY_SIZE:
            raise Fatal("the partition table runs off the end of the image")
        magic, = struct.unpack_from("<H", raw, 0)
        if magic == MD5_MAGIC:
            break
        if magic != ENTRY_MAGIC:
            if not entries:
                raise Fatal("no partition table at %#x: the first entry's magic"
                            " is %#06x, not %#06x"
                            % (offset, magic, ENTRY_MAGIC))
            break
        part_offset, size = struct.unpack_from("<II", raw, 4)
        flags, = struct.unpack_from("<I", raw, 28)
        entries.append(Partition(raw[12:28].split(b"\0")[0].decode("ascii",
                                                                  "replace"),
                                 raw[2], raw[3], part_offset, size, flags))
    if not entries:
        raise Fatal("the partition table at %#x is empty" % offset)
    return entries


def build_partition_table(partitions):
    """The inverse, so --self-test can parse a table it built from the CSV."""
    blob = bytearray()
    for part in partitions:
        entry = bytearray(ENTRY_SIZE)
        struct.pack_into("<H", entry, 0, ENTRY_MAGIC)
        entry[2], entry[3] = part.kind, part.subtype
        struct.pack_into("<II", entry, 4, part.offset, part.size)
        entry[12:12 + len(part.label)] = part.label.encode("ascii")
        struct.pack_into("<I", entry, 28, part.flags)
        blob += entry
    end = bytearray(b"\xff" * ENTRY_SIZE)
    struct.pack_into("<H", end, 0, MD5_MAGIC)
    return bytes(blob + end)


def _number(text, field, row):
    text = text.strip()
    if not text:
        raise Fatal("%s is blank in the partition CSV row %r; this tool needs"
                    " explicit offsets and sizes, and both of this repo's CSVs"
                    " give them" % (field, row))
    scale = 1
    if text[-1] in "kK":
        scale, text = 1024, text[:-1]
    elif text[-1] in "mM":
        scale, text = MB, text[:-1]
    try:
        return int(text, 0) * scale
    except ValueError:
        raise Fatal("cannot read %s %r in the partition CSV row %r"
                    % (field, text, row))


def parse_partition_csv(path):
    """The repo's own gen_esp32part-style CSV. Parsed, deliberately never
    hardcoded: the point of the check is that the image matches the table this
    repo would build TODAY, so the CSV has to be the source."""
    path = Path(path)
    if not path.is_file():
        raise Fatal("no partition CSV at %s" % path)
    named = lambda table, text, what, row: (
        table[text.lower()] if text.lower() in table
        else _number(text, what, row))
    partitions = []
    for line in path.read_text(encoding="utf-8").splitlines():
        row = line.split("#", 1)[0].strip()
        if not row:
            continue
        fields = [field.strip() for field in row.split(",")]
        fields += [""] * (6 - len(fields))
        label, kind, subtype, offset, size, flags = fields[:6]
        partitions.append(Partition(
            label, named(TYPE_NAMES, kind, "type", row),
            named(SUBTYPE_NAMES, subtype, "subtype", row),
            _number(offset, "offset", row), _number(size, "size", row),
            1 if flags.lower() == "encrypted"
            else (_number(flags, "flags", row) if flags else 0)))
    if not partitions:
        raise Fatal("%s declares no partitions" % path)
    return partitions


def table_mismatch(parsed, expected, csv_name):
    """Refusal: the image's table is not the one this repo builds."""
    if len(parsed) != len(expected):
        return ("the image has %d partitions and %s declares %d"
                % (len(parsed), csv_name, len(expected)))
    for got, want in zip(parsed, expected):
        if got.key() != want.key():
            return ("partition %r in the image is %s, where %s declares %r as %s"
                    % (got.label, got.described(), csv_name, want.label,
                       want.described()))
    return None


def identify_family(parsed):
    """(family, csv_name, nominal, expected) or (None, None, None, reasons).

    Identity comes from the table, not the image length: a layout is legal on
    any part big enough to hold it, and an 8 MB layout on a 16 MB part simply
    strands the excess.
    """
    reasons = []
    for family, _banner, csv_name, nominal in FAMILIES:
        expected = parse_partition_csv(ROOT / "partitions" / csv_name)
        problem = table_mismatch(parsed, expected, csv_name)
        if problem is None:
            return family, csv_name, nominal, expected
        reasons.append("%s: %s" % (csv_name, problem))
    return None, None, None, reasons


# ---------------------------------------------------------------------------
# What is inside a partition: app descriptor, versions, otadata, blocks
# ---------------------------------------------------------------------------


def parse_app_desc(blob, partition_offset):
    """esp_app_desc_t at partition_offset + 0x20, or None if the magic is absent.

    DO NOT READ FW_VERSION OUT OF THIS. On an Arduino-framework build `version`
    and `project_name` carry the strings of the precompiled IDF libraries -
    measured on the 2026-09-26 dump, BOTH slots report version
    'esp-idf: v4.4.7 38eeba213a' and project 'arduino-lib-builder' while in fact
    holding 2.19.0 and 2.20.0. The descriptor says which toolchain built the
    libraries; scan_versions() is what answers which firmware this is.
    """
    base = partition_offset + APP_DESC_OFFSET
    if len(blob) < base + 144:
        return None
    magic, = struct.unpack_from("<I", blob, base)
    if magic != APP_DESC_MAGIC:
        return None
    text = lambda start, length: blob[base + start:base + start + length] \
        .split(b"\0")[0].decode("ascii", "replace")
    return {"secure_version": struct.unpack_from("<I", blob, base + 4)[0],
            "version": text(16, 32), "project_name": text(48, 32),
            "time": text(80, 16), "date": text(96, 16), "idf_ver": text(112, 32)}


def scan_versions(region):
    """Version-shaped strings in an app partition, anchored ones named first.

    This is how 2.20.0 (running) and 2.19.0 (superseded) were identified on the
    real dump. It is a scan of printable runs and not a parse of anything, so it
    reports candidates with occurrence counts and leaves the reading to whoever
    is looking.
    """
    texts = [match.group().decode("ascii", "replace")
             for match in PRINTABLE_RE.finditer(region)]
    counts, named = {}, {}
    for text in texts:
        for version in VERSION_RE.findall(text):
            counts[version] = counts.get(version, 0) + 1
    for why, pattern in VERSION_ANCHORS:
        for text in texts:
            for version in pattern.findall(text):
                named.setdefault(version, set()).add(why)
    ranked = sorted(counts.items(),
                    key=lambda pair: (-len(named.get(pair[0], ())), -pair[1],
                                      pair[0]))
    return [(version, count, sorted(named.get(version, ())))
            for version, count in ranked]


def parse_otadata(blob, partition):
    """The two esp_ota_select_entry_t copies, and the slot they elect.

    The copies are the two halves of the partition rather than a hardcoded
    0xE000/0xF000, so this keeps working if the table moves - which is the whole
    reason a full-flash tool parses the table first.
    """
    copies = []
    for base in (partition.offset, partition.offset + partition.size // 2):
        raw = blob[base:base + ENTRY_SIZE]
        if len(raw) < ENTRY_SIZE:
            copies.append({"offset": base, "seq": None, "state_name": "short"})
            continue
        seq, = struct.unpack_from("<I", raw, 0)
        state, = struct.unpack_from("<I", raw, 24)
        copies.append({"offset": base, "seq": seq,
                       "state_name": OTA_STATES.get(state, "unknown")})
    return copies, running_slot([copy["seq"] for copy in copies])


def running_slot(seqs, slots=2):
    """(slot, explanation) from the ota_seq values.

    0xFFFFFFFF is an erased copy. Zero is unusable too: the bootloader computes
    (seq - 1) % count, which underflows at 0, and esp_ota_set_boot_partition
    starts counting at 1 - so a 0 is a copy nothing wrote, not a vote for -1.
    """
    valid = [seq for seq in seqs if seq not in (None, OTA_SEQ_BLANK, 0)]
    if not valid:
        return None, ("both otadata copies are blank, so the bootloader falls"
                      " back to the first app partition - a board that has never"
                      " taken an OTA")
    best = max(valid)
    slot = (best - 1) % slots
    if len(valid) == 1:
        return slot, ("one otadata copy is blank; the other reads ota_seq %d,"
                      " electing ota_%d" % (best, slot))
    return slot, ("the newer otadata copy reads ota_seq %d, electing ota_%d"
                  % (best, slot))


def used_blocks(region):
    """(used, total) 4 KB blocks that are not entirely 0xFF.

    Erased flash is 0xFF, so this is "how much was ever written" and nothing
    finer. It is not a measure of live data: LittleFS reports every block
    written once it has cycled through them.
    """
    blank = b"\xff" * BLOCK
    used = sum(1 for start in range(0, len(region), BLOCK)
               if region[start:start + BLOCK] != blank[:len(region) - start])
    return used, (len(region) + BLOCK - 1) // BLOCK


def filesystem_report(region, probe_blocks=8):
    """The littlefs superblock magic, the filenames a scan can see, and usage.

    DELIBERATELY NOT AN UNPACK. PlatformIO's mklittlefs cannot read an image
    this firmware wrote - `Corrupted dir pair at {0x0, 0x1}`, from both the
    0.2.3 and the @src- build - and the offset is not the problem: on the
    2026-09-26 dump block 1 carries the magic and readable directory entries.
    It is a LittleFS version mismatch between the ESP32 Arduino core's lfs and
    the packing tool's. There is no unpack/repack path with the tools in this
    workstation's PlatformIO install, so nobody should spend an evening
    retrying it; the restore path is written not to need one.
    """
    magic_block = next((index for index in range(probe_blocks)
                        if b"littlefs" in region[index * BLOCK:
                                                 (index + 1) * BLOCK]), None)
    used, total = used_blocks(region)
    return {"magic_block": magic_block, "used": used, "total": total,
            "names": sorted({match.group().decode("ascii", "replace")
                             for match in FILENAME_RE.finditer(region)})}


def coredump_report(region):
    """Bytes written, the declared length, and whether an ELF sits at offset 20.

    The ELF starts at offset 20, so the header ahead of it is 20 bytes - the
    2026-09-26 notes said 24 while checking +20, and +20 is what the dump shows.
    The first u32 is the dump's own declared length, which read 10 756 there
    against 10 665 bytes that are not 0xFF; the difference is interior 0xFF
    bytes inside the payload, so neither number is wrong.
    """
    declared = None
    if len(region) >= 4:
        value, = struct.unpack_from("<I", region, 0)
        declared = None if value == 0xFFFFFFFF else value
    return {"written": sum(1 for byte in region if byte != 0xFF),
            "declared": declared, "elf": region[20:24] == b"\x7fELF"}


# ---------------------------------------------------------------------------
# Identity: the efuse id, and the id the cloned config carries
# ---------------------------------------------------------------------------


def id_from_mac(mac):
    """ESP.getEfuseMac() % 0x10000, from the MAC esptool prints.

    The efuse MAC is stored with mac[0] in the LEAST significant byte, so the id
    is mac[0] | mac[1] << 8 where mac[0] is the FIRST byte printed. Verified on
    three boards: 80:b5:4e:.. -> b580, 24:62:ab:.. -> 6224, d0:cf:13:.. -> cfd0.
    Backwards gives a plausible four hex digits for the wrong board, and the
    consequence is a config ConfigFile::loadFile() refuses.
    """
    parts = [piece for piece in re.split(r"[:\-]", mac.strip()) if piece]
    try:
        return "%04x" % (int(parts[0], 16) | (int(parts[1], 16) << 8))
    except (IndexError, ValueError):
        raise Fatal("cannot read a MAC out of %r" % mac)


def recover_config_id(region, limit=8):
    """The `id` of the config.json inside a raw filesystem region, or None.

    Returns ONLY the id and the top-level key NAMES. The document also holds the
    Wi-Fi password, the admin password and the ThingsBoard token, and this tool
    has no reason to carry any of them into a report or a manifest. A document
    split across non-contiguous LittleFS blocks is simply not found, and the
    caller says "unknown" rather than guessing.
    """
    found = _recover_config_document(region, limit)
    if found is None:
        return None
    document, _start, _end = found
    return {"id": str(document["id"]), "keys": sorted(document.keys())}


def _recover_config_document(region, limit=8):
    """(document, start, end) for the first parseable config.json, or None.

    PRIVATE and it stays private: what it returns holds every credential the
    device has. Exactly two callers need the whole thing - recover_config_id(),
    which keeps the id and the key names, and patch_config_id(), which compares
    two documents field by field without letting either out.
    """
    for seen, match in enumerate(re.finditer(rb'"hostname"', region)):
        if seen >= limit:
            break
        start = match.start()
        for _ in range(4):
            start = region.rfind(b"{", 0, start)
            if start < 0:
                break
            end = _matching_brace(region, start)
            if end is None:
                continue
            try:
                document = json.loads(region[start:end + 1].decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                continue
            if isinstance(document, dict) and "id" in document:
                return document, start, end
    return None


def patch_config_id(blob, new_id):
    """Rewrite the four-character `id` of the config.json inside an image.

    THIS IS WHAT MAKES A CLONE BOOT AS THE GARDEN instead of into the setup AP,
    and it is what was actually done on 2026-09-26. loadFile() refuses the whole
    document when `id` != ESP.getEfuseMac() % 0x10000, so a byte-identical clone
    raises the portal; with the id patched the board comes up on its first boot
    with the history, the trained model and the calibration intact.

    It is safe for three reasons, and EVERY ONE IS CHECKED rather than trusted,
    because each fails silently:

    1. The literal must occur EXACTLY ONCE in the image. A second hit means a
       stale LittleFS block, and patching the wrong copy changes nothing while
       looking exactly like it worked.
    2. The replacement must be THE SAME LENGTH. config.json is ~1806 B against
       an inline cap of min(cache_size, block_size/8) = 512 B here, so it lives
       in an ordinary data block - and LittleFS CRCs cover metadata commits, not
       file DATA. A same-length edit is invisible to the filesystem; any other
       length is a corrupt file.
    3. loadFile() parses it with strtol(id, &endPtr, 16), so case is irrelevant.

    The edit is then proved surgical BEFORE anything is written: length
    unchanged, only bytes inside the id's four characters differ (b580 -> cfd0
    moves THREE, the trailing 0 being shared), and a whole document still parses
    out of the patched region with the new id and every other field as it was.

    Returns (patched_blob, evidence) - offsets and ids only, never a field value
    - or raises Fatal naming the check that refused.
    """
    text = str(new_id).strip().lower()
    if not re.fullmatch(r"[0-9a-f]{4}", text):
        raise Fatal("--patch-id wants four hex characters, as ESP.getEfuseMac()"
                    " %% 0x10000 is always rendered; got %r" % new_id)

    partitions = parse_partition_table(blob)
    filesystem = next((part for part in partitions
                       if part.subtype in FS_SUBTYPES), None)
    if filesystem is None:
        raise Fatal("the image declares no filesystem partition, so there is no"
                    " config.json in it to patch")
    region = blob[filesystem.offset:filesystem.end()]
    found = _recover_config_document(region)
    if found is None:
        raise Fatal("no parseable config.json in the %s region, so the id cannot"
                    " be located; a document split across non-contiguous blocks"
                    " has to be patched by hand" % filesystem.label)
    before, _start, _end = found
    old = str(before["id"]).strip()

    # NO SPACE after the colon: that is how ConfigFile::saveFile() writes it, and
    # the whole single-occurrence argument is about this exact literal.
    needle = ('"id":"%s"' % old).encode("ascii")
    hits = [match.start() for match in re.finditer(re.escape(needle), blob)]
    if len(hits) != 1:
        raise Fatal("the literal %s occurs %d times in the image, not once - a"
                    " second copy means a stale block, and patching the wrong"
                    " one changes nothing while looking like it worked"
                    % (needle.decode(), len(hits)))
    if len(text) != len(old):
        raise Fatal("the stored id %r is %d characters and %r is %d; a"
                    " different-length edit changes the file's length, which"
                    " LittleFS did not record when it wrote it"
                    % (old, len(old), text, len(text)))
    if text == old.lower():
        raise Fatal("the image already carries id %s, so there is nothing to"
                    " patch - this board is the one the dump came from" % old)

    value_at = hits[0] + len(b'"id":"')
    patched = bytearray(blob)
    patched[value_at:value_at + len(text)] = text.encode("ascii")
    patched = bytes(patched)

    # --- and now prove the edit was surgical, before a byte is written -------
    if len(patched) != len(blob):
        raise Fatal("the patch changed the image length, which is a bug here")
    differ = [index for index in range(value_at, value_at + len(text))
              if patched[index] != blob[index]]
    expected = [value_at + offset for offset in range(len(text))
                if text[offset] != old[offset]]
    if differ != expected:
        raise Fatal("the patched bytes are not the ones the id change implies")
    span = range(value_at, value_at + len(text))
    elsewhere = _first_difference(blob, patched, span)
    if elsewhere is not None:
        raise Fatal("the patch changed byte %#x, outside the id" % elsewhere)

    after = _recover_config_document(patched[filesystem.offset:filesystem.end()])
    if after is None:
        raise Fatal("the patched region no longer yields a parseable"
                    " config.json, so the edit broke the document")
    document = after[0]
    if str(document["id"]).strip().lower() != text:
        raise Fatal("the patched document reads id %r rather than %r"
                    % (document["id"], text))
    # Compared here and DELIBERATELY not returned: these are the credentials.
    moved = sorted(key for key in set(document) | set(before)
                   if key != "id" and document.get(key) != before.get(key))
    if moved:
        raise Fatal("the patch also changed %s, which it must not"
                    % ", ".join(moved))

    return patched, {"old": old, "new": text, "offset": value_at,
                     "bytes_changed": len(differ), "offsets": differ,
                     "fields_compared": len(before),
                     "partition": filesystem.label,
                     "partition_offset": filesystem.offset,
                     "partition_size": filesystem.size}


def _first_difference(left, right, skip):
    """The first index where two equal-length blobs differ outside `skip`.

    Compared in 64 KB slices rather than byte by byte: an 8 MB image is eight
    million Python-level comparisons otherwise, which turns a proof into a wait.
    """
    skip = range(skip.start, skip.stop)
    for start in range(0, len(left), 65536):
        stop = min(start + 65536, len(left))
        if left[start:stop] == right[start:stop]:
            continue
        for index in range(start, stop):
            if left[index] != right[index] and index not in skip:
                return index
    return None


def _matching_brace(blob, start, cap=16384):
    """The `}` closing the `{` at start, string-aware. None if it is not there."""
    depth, in_string, escaped = 0, False, False
    for index in range(start, min(len(blob), start + cap)):
        byte = blob[index:index + 1]
        if in_string:
            escaped = (byte == b"\\") and not escaped
            in_string = escaped or byte != b'"'
        elif byte == b'"':
            in_string = True
        elif byte == b"{":
            depth += 1
        elif byte == b"}":
            depth -= 1
            if depth == 0:
                return index
    return None




# -------------------------------------------------------------------------
# Where a backup's files land, and the refusals about the IMAGE. The
# refusals about the tool and the garden are in flash_clone.py.
# -------------------------------------------------------------------------


def sidecar_path(image_path):
    """The manifest recording what an image hashed to when it was read."""
    return Path(image_path).with_suffix(".manifest.json")


def parts_dir(image_path):
    path = Path(image_path)
    return path.with_name(path.name.replace("-full.bin", "") + "-parts")


def split_partitions(blob, partitions, target):
    """One file per partition, plus the bootloader and the table itself.

    Useful for writing one region without the others, and for pointing
    espcoredump at the coredump partition alone.
    """
    pieces = [("bootloader.bin", 0, PART_TABLE_OFFSET),
              ("partitions.bin", PART_TABLE_OFFSET, BLOCK)]
    for part in partitions:
        name = "littlefs" if part.subtype in FS_SUBTYPES else part.label
        pieces.append((name + ".bin", part.offset, part.size))
    for name, offset, size in pieces:
        (target / name).write_bytes(blob[offset:offset + size])
    return len(pieces)

def image_length_refusal(length):
    """Refusal: a whole flash is a power-of-two number of megabytes."""
    if length == 0:
        return "the image is empty"
    if length % MB:
        return ("the image is %d bytes, which is not a whole number of"
                " megabytes - a full-flash dump is" % length)
    megabytes = length // MB
    if megabytes & (megabytes - 1) or megabytes > 64:
        return ("the image is %d MB, and a flash part is a power-of-two size -"
                " this looks like a partition or a truncated read, not a whole"
                " flash" % megabytes)
    return None


def manifest_refusal(image_path, digest):
    """Refusal: a sidecar manifest disagrees about the image's sha256."""
    path = sidecar_path(image_path)
    if not path.is_file():
        return None
    try:
        recorded = json.loads(path.read_text(encoding="utf-8")).get("sha256")
    except (OSError, ValueError) as error:
        return "the manifest %s will not parse: %s" % (path.name, error)
    if recorded and recorded.lower() != digest.lower():
        return ("%s records sha256 %s and the image hashes to %s - this is not"
                " the image that was verified"
                % (path.name, recorded[:16], digest[:16]))
    return None


def flash_size_refusal(detected, image_length):
    """Refusal: the target part cannot hold the image at all."""
    if detected is None:
        return ("esptool's banner reported no flash size, so nothing here can"
                " say the part is big enough for a %d MB image"
                % (image_length // MB))
    if detected < image_length:
        return ("the target reports %d MB of flash and the image is %d MB - it"
                " does not fit, and a partial write leaves no bootloader"
                % (detected // MB, image_length // MB))
    return None


def chip_refusal(detected, image_family):
    """Refusal: the banner's chip is not the family the image's table is for."""
    if detected is None:
        return ("esptool's banner named no chip, so nothing here can say it"
                " matches an %s image" % image_family)
    if detected != image_family:
        return ("the target is %s and the image's partition table is the %s one"
                % (detected, image_family))
    return None


# ---------------------------------------------------------------------------
# Verification: everything establishable without a board
# ---------------------------------------------------------------------------


class Report:
    def __init__(self, path, length, digest):
        self.path, self.length, self.digest = path, length, digest
        self.checks, self.findings, self.partitions = [], [], []
        self.family = self.config_id = self.running_label = None

    def check(self, name, passed, detail=""):
        self.checks.append((name, bool(passed), detail))
        return passed

    def failed(self):
        return [name for name, passed, _ in self.checks if not passed]


def verify_image(path, blob=None):
    """Every structure check, offline. Raises Fatal on a refusal."""
    path = Path(path)
    if blob is None:
        if not path.is_file():
            raise Fatal("no image at %s" % path)
        blob = path.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    for problem in (image_length_refusal(len(blob)),
                    manifest_refusal(path, digest)):
        if problem:
            raise Fatal(problem)

    report = Report(path, len(blob), digest)
    report.check("byte 0 is %#04x (ESP_IMAGE_HEADER_MAGIC)" % IMAGE_MAGIC,
                 blob[0] == IMAGE_MAGIC,
                 "" if blob[0] == IMAGE_MAGIC
                 else "found %#04x - there is no bootloader here" % blob[0])

    report.partitions = parse_partition_table(blob)
    family, csv_name, nominal, expected = identify_family(report.partitions)
    if family is None:
        raise Fatal("the partition table matches neither of this repo's CSVs:\n"
                    "  " + "\n  ".join(expected))
    report.family = family
    report.check("the partition table is %s, entry for entry" % csv_name, True)
    if nominal != len(blob):
        report.findings.append(
            "the %s layout is nominally %d MB and this image is %d MB, which is"
            " legal on a larger part and strands the excess"
            % (csv_name, nominal // MB, len(blob) // MB))
    outside = [part.label for part in report.partitions
               if part.end() > len(blob)]
    report.check("every partition lies inside the image", not outside,
                 ", ".join(outside))

    otadata = next((part for part in report.partitions
                    if part.kind == 1 and part.subtype == 0x00), None)
    if otadata:
        copies, (slot, explanation) = parse_otadata(blob, otadata)
        report.check("otadata parses at %#x and %#x"
                     % (copies[0]["offset"], copies[1]["offset"]),
                     all(copy["seq"] is not None for copy in copies), explanation)
        report.findings += [
            "otadata %#x: ota_seq %s, state %s"
            % (copy["offset"], "blank" if copy["seq"] in (None, OTA_SEQ_BLANK)
               else copy["seq"], copy["state_name"]) for copy in copies]
        running = next((part for part in report.partitions if part.is_app()
                        and slot is not None and part.subtype == 0x10 + slot),
                       None)
        report.running_label = running.label if running else None

    for part in report.partitions:
        region = blob[part.offset:part.end()]
        used, total = used_blocks(region)
        detail = "%d of %d blocks written" % (used, total)
        if part.is_app():
            _verify_app(report, blob, part, region, detail)
        elif part.subtype in FS_SUBTYPES:
            _verify_filesystem(report, part, region)
        elif part.subtype == COREDUMP_SUBTYPE:
            _verify_coredump(report, part, region, detail)
        else:
            report.check("%s read" % part.label, True, detail)
    return report


def _verify_app(report, blob, part, region, detail):
    report.check("%s starts with %#04x" % (part.label, IMAGE_MAGIC),
                 bool(region) and region[0] == IMAGE_MAGIC, detail)
    desc = parse_app_desc(blob, part.offset)
    report.check("%s carries an esp_app_desc_t" % part.label, desc is not None,
                 "" if desc is None else "idf %s, libraries built %s"
                 % (desc["idf_ver"], desc["date"]))
    versions = scan_versions(region)
    if not versions:
        report.findings.append("%s: no version-shaped string found" % part.label)
        return
    named = versions[0][2]
    report.findings.append(
        "%s: version strings %s%s"
        % (part.label,
           ", ".join("%s x%d" % (ver, count) for ver, count, _ in versions[:4]),
           "" if not named else "  (%s named by %s)"
           % (versions[0][0], " and ".join(named))))


def _verify_filesystem(report, part, region):
    found = filesystem_report(region)
    report.check("%s carries the littlefs superblock magic" % part.label,
                 found["magic_block"] is not None,
                 "block %s, %d of %d blocks written, %d filenames seen"
                 % (found["magic_block"], found["used"], found["total"],
                    len(found["names"])))
    report.config_id = recover_config_id(region)
    report.findings += [
        "%s: config.json %s" % (part.label, "id %s, %d top-level keys"
                                % (report.config_id["id"],
                                   len(report.config_id["keys"]))
                                if report.config_id
                                else "not recoverable from raw blocks"),
        "%s: %s" % (part.label, " ".join(found["names"]))]


def _verify_coredump(report, part, region, detail):
    core = coredump_report(region)
    # NOT pass/fail. A core dump is evidence the board panicked or tripped a
    # watchdog, which is a FINDING to print loudly.
    report.check("%s read (%d bytes not 0xFF)" % (part.label, core["written"]),
                 True, detail)
    if core["written"]:
        report.findings.append(
            "FINDING: %s holds %d bytes, declared length %s, and %s - this board"
            " HAS panicked or tripped a watchdog. Decoding it needs espcoredump"
            " and the EXACT ELF that produced the image (backups/elf/); a"
            " rebuild will not match."
            % (part.label, core["written"], core["declared"],
               "an ELF sits at offset 20" if core["elf"]
               else "there is NO ELF at offset 20"))


def print_report(report):
    print("image     %s\nlength    %d bytes (%d MB)\nsha256    %s\nfamily    %s"
          % (report.path, report.length, report.length // MB, report.digest,
             report.family))
    if report.running_label:
        print("running   %s" % report.running_label)
    print()
    for name, passed, detail in report.checks:
        print("  [%s] %s%s" % ("ok" if passed else "FAIL", name,
                               "" if not detail else "\n         %s" % detail))
    if report.findings:
        print("\nfindings\n" + "\n".join("  - %s" % f for f in report.findings))


def print_clone_warnings(image_id, predicted_id):
    """Why a byte-identical clone is not a working garden. Printed, not refused."""
    if image_id and predicted_id and image_id != predicted_id:
        verdict = ("""The image's config says id %s; this chip's MAC predicts %s. So the clone
boots, REFUSES ITS OWN CONFIG, and raises the setup AP 'espgarden-%s' with a
slow-blinking LED - onboarding arm 1 doing its job, not a failure. --patch-id
rewrites that id instead and is the better default. Either way, read the new
id off the board's own `ID: xxxx` line, BY DEFINITION the number loadFile()
compares against.""" % (image_id, predicted_id, predicted_id))
    elif image_id and predicted_id:
        verdict = ("""The image's config id %s MATCHES this chip's predicted id, so it will
accept its own config - which makes it a LIVE TWIN of the original, publishing
on the same ThingsBoard token under the same hostname.""" % image_id)
    else:
        verdict = ("The config id (%s) and/or the MAC could not be read, so\n"
                   "nothing here can predict what this clone does at boot."
                   % (image_id or "not found"))
    rule = "=" * 72
    print("\n%s\nA BYTE-IDENTICAL CLONE DOES NOT COME UP AS A WORKING GARDEN\n%s"
          "\n%s\n\nFour fields to change before the clone touches a network:\n%s"
          "\n\nTHE IMAGE IS A CREDENTIAL. Flash encryption is not enabled on"
          " these\nboards, so the Wi-Fi password, the ThingsBoard token,"
          " users.json's\nhashes and sessions.json's live bearer tokens are all"
          " plain text in\nit. Do not commit, paste or upload it.\n%s\n"
          % (rule, rule, verdict,
             "\n".join("  %-14s %s" % pair for pair in CLONE_FIELDS), rule))



# ---------------------------------------------------------------------------
# The checks for the two things above that OPERATE on a whole image
# ---------------------------------------------------------------------------


def self_test_checks():
    """(name, passed, got, want) for verify_image() and patch_config_id().

    They live here rather than in flash_clone.py because they are checks on THIS
    module's two composite operations, and a test beside the code it covers is
    one a reader finds. `flash_clone.py --self-test` calls this, adds its own
    checks for the esptool and garden halves, and prints the single count - so
    there is still exactly one entry point and one number.
    """
    checks = []
    ok = lambda name, got, want=True: checks.append((name, got == want, got, want))

    def why(thunk):
        """A Fatal's own sentence, or None - so a check can assert WHY."""
        try:
            thunk()
        except Fatal as error:
            return str(error)
        return None

    fired = lambda thunk: why(thunk) is not None
    verify = lambda blob: verify_image(Path("x-full.bin"), blob=blob)

    # An 8 MB image built from the repo's own CSV, with just enough in each
    # partition for every branch of verify_image() to have something to say.
    rows = parse_partition_csv(ROOT / "partitions" / "esp_garden_8mb.csv")
    blob = bytearray(b"\xff" * (8 * MB))
    blob[0] = IMAGE_MAGIC
    table = build_partition_table(rows)
    blob[PART_TABLE_OFFSET:PART_TABLE_OFFSET + len(table)] = table
    for part in rows:
        if part.is_app():
            blob[part.offset] = IMAGE_MAGIC
            struct.pack_into("<I", blob, part.offset + APP_DESC_OFFSET,
                             APP_DESC_MAGIC)
        elif part.subtype in FS_SUBTYPES:
            at = part.offset + BLOCK    # the superblock is in block 1, not 0
            blob[at + 8:at + 16] = b"littlefs"
            config = b'{"id":"b580","hostname":"espgarden-s3","io":{}}'
            blob[at + 64:at + 64 + len(config)] = config
    struct.pack_into("<I", blob, 0xE000, 1)
    image = bytes(blob)

    report = verify(image)
    ok("a synthesized image verifies as esp32s3", report.family, "esp32s3")
    ok("a synthesized image passes every check", report.failed(), [])
    ok("the elected slot is named", report.running_label, "app0")
    ok("the config id is recovered end to end", report.config_id["id"], "b580")
    headless = bytearray(image)
    headless[0] = 0x00
    ok("a missing bootloader magic fails exactly one check",
       verify(bytes(headless)).failed(),
       ["byte 0 is 0xe9 (ESP_IMAGE_HEADER_MAGIC)"])
    ok("an image whose table matches no CSV is refused",
       fired(lambda: verify(b"\xe9" + b"\xff" * (8 * MB - 1))))
    ok("a 3 MB image is refused before anything is parsed",
       fired(lambda: verify(b"\xe9" * (3 * MB))))

    # --- the id patch, the one operation here that MODIFIES an image --------
    patched, evidence = patch_config_id(image, "cfd0")
    ok("the patch reports the id it replaced", evidence["old"], "b580")
    ok("b580 -> cfd0 moves THREE bytes, the trailing 0 shared",
       evidence["bytes_changed"], 3)
    ok("the patch keeps the image length", len(patched), len(image))
    ok("the patched image parses with the new id",
       recover_config_id(patched[0x590000:0x7F0000])["id"], "cfd0")
    ok("the patch names the partition a restore would write",
       (evidence["partition"], evidence["partition_offset"]),
       ("spiffs", 0x590000))
    ok("exactly three bytes of 8 MB differ",
       sum(1 for at in range(len(patched)) if patched[at] != image[at]), 3)
    ok("the patched image still passes every check", verify(patched).failed(), [])
    ok("a five-character id is refused",
       fired(lambda: patch_config_id(image, "cfd00")))
    ok("a non-hex id is refused", fired(lambda: patch_config_id(image, "zzzz")))
    ok("patching to the id already stored is refused",
       fired(lambda: patch_config_id(image, "b580")))
    wiped = bytearray(image)
    wiped[0x590000:0x7F0000] = b"\xff" * 0x260000
    ok("an image with no config.json is refused",
       fired(lambda: patch_config_id(bytes(wiped), "cfd0")))
    # A SECOND COPY OF THE LITERAL IS THE DANGEROUS CASE: patching one of two
    # changes nothing while looking exactly like it worked.
    twin = bytearray(image)
    stale = 0x590000 + 8 * BLOCK
    twin[stale:stale + 12] = b'"id":"b580"\x00'
    ok("a second occurrence of the literal is refused",
       fired(lambda: patch_config_id(bytes(twin), "cfd0")))
    ok("the refusal says why a second copy matters",
       "stale block" in (why(lambda: patch_config_id(bytes(twin), "cfd0")) or ""))
    return checks
