# The S3 has no ThingsBoard token, by design

`MQTT Link: down (rc=5)` — not authorized. The TLS handshake completes against
the pinned CA, and the broker refuses the credentials because all three
onboarding templates ship `mqtt.username` EMPTY on purpose. An invented token
would connect to nothing and report success, which this repo has paid for once.

**Done means:** a device created in ThingsBoard for this board and its access
token pasted into `mqtt.username` through `/config.html`.

**Decide first whether it is a NEW ThingsBoard device or reuses the garden's.**
A new one keeps both histories honest and the free plan has room (2 of 5 used,
one of them an orphan). Reusing `espgarden1`'s token would put two boards on one
series — the defect this repo refuses for relay indices and `moistureN`.

---
**Done, 2026-09-17 — and the answer was "neither".** The decision this note
framed (new ThingsBoard device or reuse the garden's) was overtaken: both
boards left ThingsBoard Cloud for the self-hosted instance.

Tenant `esp-garden`, devices `espgarden1` and `espgarden-s3`, one access token
each. **Access token, not MQTT Basic** — the README recommends MQTT Basic, but
access-token auth is what this firmware has been doing for weeks, and a cutover
is not the moment to debut an unexercised auth path. Both connected; the server
is accepting payloads, confirmed by reading the series back, not by the link
being up.
