# SHT40 over I²C

**Done** — `061eb3f`, FW_VERSION 2.14.0. Pushed.

The `esp-garden-hardware` carrier dropped the DHT22 header for an SHT40, so the
S3 board read no temperature and no humidity at all until this landed.

Hand-rolled driver (`sht4x_protocol.h` + `sht4x.cpp`), two CRC8s per frame as
the primary gate, `ROLE_I2C` in `validatePins()` where a shared bus is legal and
`SDA == SCL` is not, `ambient_sensor` client attribute recording which part is
fitted. Existing `temperature` / `airHumidity` keys kept — same quantity measured
better, not a different quantity under one name.

Verified independently: 189/189 host cases, `check_lines` 954, six envs build.
Cost +25 636 B flash on WROOM, 18 KB of which is the framework I²C stack a
library would have paid too.

**Nothing has run on silicon.** Everything is in CLAUDE.md's Unverified list.
