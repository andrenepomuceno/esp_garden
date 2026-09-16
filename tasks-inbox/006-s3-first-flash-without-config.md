# A virgin S3 has to reach onboarding, and `-t buildfs` wants a config

Once 002 lands, the right first flash for a new board carries **no**
`/config.json` at all — the board comes up in AP mode and writes its own.

But `-t buildfs` requires `data/config.json` today, and `config_guard()`
correctly refuses to pack the live garden's WROOM document into an S3 image. So
the current path is circular: provisioning needs the id, the id needs the board
flashed.

**Done means:** a documented, executed command sequence that takes an unflashed
ESP32-S3-DevKitC-1 to a working AP, without `data/config.json` and without
weakening the guard.

May be resolved in passing by 002 — check before doing it twice.
