# `cloud.enabled` changed false → true on the S3, and nothing owns the change

Found during the carrier bring-up. The stored document held `false` at 17:02:48
and `true` at 17:10:03. **Every document the tooling sent is on disk and all
carry `false`**; only `POST /config.json` and the onboarding handler call
`saveFile()`, and the portal's routes are not registered in normal mode. **It
did not reproduce in 32 attempts** (20 byte-identical round trips plus 12
replays of the exact candidate documents).

The `Saved /config.json (N bytes)` line that would name the writer is gone: the
8 KB buffer rolled, and `/log0..3.txt` hold only boot lines because
`Logger::backup()` is hourly and the board never stayed up an hour.

**The main session did NOT post config to that board that day** — its only S3
config write was the previous day's admin credential change.

**So one question decides whether this is a bug at all:** was a browser sitting
on `/config.html` for that board around 17:03-17:05? Every session in the log
comes from the workstation's IP, so a human save is indistinguishable from a
scripted one. If yes, there is nothing here. If no, this is silent corruption
on the documented restore path — and `/config.json` is the file that bricks a
board when it is wrong.

**It mattered:** with the flag true, `/data.json` served a cloud-cover badge
from a table fitted to a different board's LDR at a different mounting in a
different season. Set back to false.

**What would make it findable next time:** the log rolls faster than anyone can
notice a config change. A durable record of config writes — who, when, how many
bytes — is the thing this incident could not be diagnosed without.
