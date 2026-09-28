# Web interface

| Page | Role | What it does |
|---|---|---|
| `/` (`index.html`) | any signed-in user | Sensors, relay buttons, logs |
| `/config.html` | ADMIN | Edit the configuration in tabs; secrets stay masked |
| `/users.html` | ADMIN | Add, edit and remove accounts and roles |
| `/update.html` | ADMIN | Firmware and filesystem OTA |
| `/history.html` | any signed-in user | Charts over 1 h / 6 h / 12 h / 1 d / 7 d / 30 d |
| `/devices.html` | ADMIN | Add, remove, name and re-pin the relays, the moisture probes and their calibration, and the single-instance sensors |
| `/schedules.html` | ADMIN | Timed relay activations |
| `/moisture.html` | any signed-in user | The classifier's inference and its fitted parameters |

![Local web UI](docs/ui.png)
| `/moisture.html` | any signed-in user | The moisture model per probe: class means, weights, separation, live classification, and which gate blocks a probe that reports nothing |

Two endpoint behaviours worth knowing, because neither has a button:

- **`GET /config.json?secrets=1`** (ADMIN) exports the configuration
  **unmasked**. Take this before any filesystem upload: `uploadfs` overwrites
  `/config.json`, and a backup taken through the normal masked `GET` cannot be
  restored — every credential comes back as eight asterisks and the loss only
  surfaces at the next boot, as a device that cannot join the network. The
  export is logged with the caller's IP.
- **`POST /spiffs/upload`** (ADMIN) replaces **one file** instead of rewriting
  the partition. Use it whenever only files under `data/` changed: a filesystem
  deploy wipes `/config.json`, the history ring and the moisture model along
  with the web assets, so a one-line CSS change costs the device its
  credentials, its 24 h of history and its watering-event count. Send the file
  as a multipart upload whose **filename is the destination path**, with an
  `MD5` form field; the bytes land in a temp file and are moved into place only
  once the checksum matches, so a dropped connection cannot leave half a file
  in production. `/users*`, `/sessions*`, `/config*` and `/provisioned.pending`
  are refused — the last one because putting it back would re-arm the
  [first-boot setup portal](../README.md#first-boot--the-setup-portal) on a working device.
- **`POST /spiffs/delete`** (ADMIN) removes one file, refusing exactly what the
  upload refuses — nothing there may delete the file that lets you undo a
  mistake made there.
- **`GET /history.json?window=<seconds>`** selects by time and decimates to fit
  `limit`, returning the `stride` it used. Without it a 24 h window is 1440
  records against a 200-record cap, so the page would silently show only the
  newest three hours. The relay mask is OR-ed across each decimation bucket
  rather than sampled — it is sticky precisely because a watering is seconds
  long, and sampling would drop seven activations in eight.
