# Deploying over HTTP without erasing the flash

`scripts/deploy_ota.py` sends a firmware image and the changed web assets to a
running board. It exists because the two obvious ways to do this both destroy
state the garden cannot get back:

- `pio run -t uploadfs` and the filesystem OTA rewrite the **whole** partition,
  so `/config.json`, `hist0..7.bin` and `/moisture_model.bin` go with the web
  assets. That has cost this project its history three times in one day.
- Doing it by hand works and has been done repeatedly — the verification log
  records nine pages deployed one file at a time — but every occasion
  re-invented the script, and none of them was kept.

What this tool does instead: assets go one at a time through
`POST /spiffs/upload`, which writes `/upload.tmp` and renames it over the
target atomically, and the firmware goes through `/updateEnable` + `/update`,
which writes the other app slot. Neither touches the filesystem partition.

## Usage

```bash
python scripts/deploy_ota.py --host espgarden-s3.local            # plan only
python scripts/deploy_ota.py --host espgarden-s3.local --yes      # write
python scripts/deploy_ota.py --assets-only --yes
python scripts/deploy_ota.py --firmware-only --yes
python scripts/deploy_ota.py --self-test                          # offline
```

`--plan` is the default: without `--yes` nothing is written. The assets come
from `.pio/assets/`, which `scripts/build_assets.py` produces — never from
`data/`, whose sources are unbundled and ungzipped. The firmware defaults to
`.pio/build/espgarden_s3/firmware.bin`; pass `--firmware` for another env.

Credentials come from `data/config.json`, `--username` / `--password`, or
`ESP_GARDEN_PASSWORD`.

## What it refuses

- **A relay is energised.** An update reboots the device, and a relay across a
  reset is the one thing not to do on this hardware. Checked before anything
  is sent, and the firmware's own `/updateEnable` guard checks it again.
- **A config backup that comes back masked.** Every run first takes
  `GET /config.json?secrets=1` into `backups/`. A masked backup cannot be
  restored from, so one is treated as a failed backup rather than a backup.
- **A protected target.** `config.json`, `users.json`, `sessions.json`,
  `provisioned.pending`, `moisture_model.bin` and `config.template.json` are
  never written, nor is any name carrying `..`, a leading `/` or a backslash.
  The device refuses most of these too; this is the near-side half.
- **An image for the wrong family.** The chip id in the image header
  (`0x0000` ESP32, `0x0009` ESP32-S3) is matched against the family the device
  reports through `/capabilities.json`'s `analogPins` (32-39 on a WROOM-32,
  1-10 on an S3). Flashing across that boundary is a serial recovery.
- **A stale asset bundle.** `scripts/pio_assets.py` builds `.pio/assets/` only
  for `buildfs`, `uploadfs` and `uploadfsota`, so a plain `pio run` leaves it
  untouched and a deploy would ship the previous UI. Any file under `data/`
  newer than the newest built asset is refused, naming the command to run.
  This fired for real on 2026-09-28 and caught a `devices_model.js` edit that
  would otherwise have gone out as the old bundle.
- **An image whose version equals the running one.** `fwVersionDiffers()`
  would answer false and the broker would report a successful update that
  never flashed. Bumping `FW_VERSION` in the same commit as the change is the
  convention this enforces.

## How it verifies

By reading the file back and comparing the hash, never by the HTTP status. The
exception is `/spiffs/config*`, `/spiffs/users*` and `/spiffs/sessions*`, which
are shadowed 403 by design so the credential store is not served to an admin
session. Those four assets are listed as **unverifiable** in the plan and are
sent without a read-back; the device still checks their MD5 before the rename.

The firmware is confirmed by polling `/data.json` until the version changes,
not by the upload's return value. A finished flash ends in a reboot that kills
the connection, so a successful update arrives at the client as an error — this
project has misdiagnosed that as a failed flash three times. The wait first
watches the board go **down** and only then waits for it to come back, because
a liveness loop that starts while the old firmware is still serving exits
immediately and reports a failure on a flash that landed.

## What has run

**2026-09-28, against `espgarden-s3` (device `cfd0`), firmware 2.21.0.**

- `--self-test`: 33 checks, 0 failed, offline and with no credential.
- The plan, against the live garden: config backed up (1734 chars, `id cfd0`,
  no masked fields), every relay idle, 22 of 26 assets identical, 4
  unverifiable.
- **The version refusal fired for real.** The board was already on 2.21.0 and
  the tool refused to send the image rather than perform an update that would
  be reported as successful.
- `--assets-only --yes` sent the four shadowed assets (10 678 B, 4 requests).
  All six pages then served gzipped at exactly the byte counts of the local
  build, and `Status.History` read 9213/10000 across the whole operation —
  nothing in the filesystem was disturbed.

**Later the same day, 2.21.0 -> 2.22.0 on the same board, the whole path:**

- The stale-bundle refusal fired and was right; after
  `python scripts/build_assets.py` the plan grew from 4 assets to 5.
- Five assets sent, then `/updateEnable` 200, `/update` 200 in 8.7 s. The board
  went down at 7 s and was serving again at 9 s, and the version confirmation
  read **2.22.0** back.
- `Status.History` went 9232 -> 9233 across the reboot and the filesystem stayed
  at 712 / 2432 KB, which is the whole point: the app slot was rewritten and the
  filesystem partition was not.

**Still not exercised:** the family refusal and the relay refusal. Both are
checked on every run and both have always passed.
