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
