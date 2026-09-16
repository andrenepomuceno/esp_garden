# The error indicator may not light on the S3, exactly when it is needed

CLAUDE.md: *"A slow-blinking LED means config did not load."* On the S3 that is
doubtful. `LED_BUILTIN` on the `esp32s3` variant is the addressable-RGB encoding,
so `digitalWrite()` drives an RMT LED rather than a plain GPIO — and the
DevKitC-1 **v1.1** carries that LED on GPIO 38, not the 48 the variant names, so
it may simply not light.

Separately, the carrier has its own status LED (`D1` / `R3` at 100 R) that is
**not in the cross-repo contract table**, so the firmware does not drive it.

Onboarding lowers the urgency rather than raising it: an AP you can see from your
phone is a better "this board is unconfigured" signal than a blink nobody is
standing next to. Do not do this before 002.

**Done means:** a decision recorded in BOTH repos' contract sections about
whether `LED_STATUS` is firmware-driven, and if so a status-LED seam that
resolves per family instead of trusting `LED_BUILTIN`.
