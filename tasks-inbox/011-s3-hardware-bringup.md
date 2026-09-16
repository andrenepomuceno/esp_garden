# Bring up the S3 carrier when it arrives

**Blocked on hardware.** The operator has the ESP32-S3-DevKitC-1 module; the
carrier is in transit.

Nothing in the `espgarden_s3` env has executed on an S3. Specifically unproven
and worth checking in this order, because each one can hide the next:

1. `templates/config.espgarden_s3.json` has never been through `loadFile()`.
2. No ADC has been read on GPIO 1; no relay switched on GPIO 10;
   `validatePins()` has never run on this family.
3. The relay safe state is **inverted** on this board and that is read off `J200`
   and a vendor datasheet, not measured. Put a meter on a coil through a boot
   before trusting it.
4. `partitions/esp_garden_8mb.csv` has never been written to a flash.
5. No SHT40 has ever answered on a bus — 0x44, the 0xFD command and the 8.2 ms
   conversion are all datasheet, not measurement.

**Do this with the relay board DISCONNECTED.** Every reset floats the GPIOs.

---
**Partly done, 2026-09-16** — bare module on USB, no carrier. Items 1, 2 (config
half) and 4 are now measurements; see CLAUDE.md. The board runs at
**192.168.1.77**, hostname `espgarden-s3`, onboarded through its own AP.

**Still blocked on the carrier**, and these are the ones that need it:
3. the relay safe state, which is read off `J200` and a datasheet and has never
   met a scope through a boot;
5. the SHT40 — no I²C transaction has happened on any pin;
   plus every ADC reading and every relay switch.

Its admin account was seeded with the SAME `ota.username`/`ota.password` pair as
the garden board, because that is what `data/config.json` had to hand. Change it
in `/config.html` if two boards should not share one credential.
