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
