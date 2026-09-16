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
