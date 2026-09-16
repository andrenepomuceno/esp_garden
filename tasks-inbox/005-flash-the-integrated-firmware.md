# Flash 6224 once the three streams are integrated

**Mine.** Blocked on 004.

Deliberately NOT flashed per stream. 2.14.0 costs the live WROOM +25.6 KB for an
I²C bus it has no sensor on and gains that garden nothing on its own, and three
separate flashes are three reboots — each of which, on this active-low board,
pulses every pump for the length of a boot.

**Done means:** one OTA, version confirmed by polling until it changes (never by
the upload's return value — a finished flash reaches the client as an error), ELF
archived under `backups/elf/`, and `/data.json` checked for the renamed
`Ambient Error Rate` row and an intact `Status.History`.

---
**BLOCKED, 2026-09-16, and not by caution.** Checked for a serial port before
flashing: **there are none.** CLAUDE.md's *"The ESP32 currently attached is on
COM7"* is stale, so there is no USB recovery path right now.

2.15.0 changes the **boot path** — `webSetup()` takes a new argument and calls
`onboarding::decide()`, `tasksSetup()` gained an early return, `main.cpp` changed
what it hands `UserStore::load()` — and none of it has run on hardware. On paper
6224 takes the unchanged arm of every one of those branches (config loads, no
marker, no SHT40 fitted), which is exactly why it is *probably* fine and exactly
why nothing would be learned if it is.

**Attach USB first.** A boot regression on an OTA-only board is the scenario this
repo built the RTC strike counter for, and 2.15.0 gains 6224 nothing on its own:
no SHT40 to read, no portal to raise, and the archiver is PC-side.

**Do it in this order:** plug USB, flash, watch the serial boot for `ID: 6224`,
`Sensors:` and the absence of any onboarding line, then archive the ELF.
