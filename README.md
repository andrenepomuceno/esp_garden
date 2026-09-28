# ESP Garden

Automatic garden irrigation and environmental monitoring system based on the ESP32, built with [PlatformIO](https://platformio.org) and integrated with the [ThingsBoard](https://thingsboard.io) IoT platform over MQTT/TLS.

> **ThingSpeak and TalkBack ship OFF** since firmware 2.12.0 (`USE_THINGSPEAK` / `USE_TALKBACK` in `include/BuildConfig.h`). The code is still here and the flags still build in both positions — see [Telemetry backends](docs/telemetry.md#telemetry-backends).

## Features

- **Automated irrigation** — timed watering triggered locally, on a schedule, or remotely via ThingsBoard RPC
- **Multi-zone relay control** — up to 4 independently timed relays, switched off by a dedicated real-time task
- **Environmental monitoring** — soil moisture (up to 3 probes), luminosity, temperature, humidity, water level, pulse flow meter and a reservoir float switch
- **Local web UI** — dashboard, configuration editor and account management, served
  from the device with no CDN dependency (jQuery is bundled)
- **Per-probe moisture classification** — Dry / Humid / Wet from a Gaussian
  naive Bayes model trained on the probe's own watering record, falling back to
  a two-point calibration and then to no badge at all
- **Cloud cover from the light sensor** — clear / partly cloudy / overcast
  against an empirical clear-sky reference fitted from the device's own
  archive, plus a cloud-transient episode on the event path. Off by default;
  see [Cloud cover](docs/cloud-cover.md)
- **Reference evapotranspiration** — a daily Hargreaves-Samani ET0 from the
  device's own temperature extremes, with no runtime network call. Off by
  default, and validated off-device against a public station before it is worth
  turning on; see [Reference evapotranspiration](docs/evapotranspiration.md)
- **Scheduled watering** — up to 8 timed relay activations, edited in the web UI
- **Cloud logging** — sensor data published to ThingsBoard over MQTT (TLS); the ThingSpeak uplink is compiled out by default
- **On-device history** — append-only segments keep the last N I/O snapshots
  across reboots, served as JSON
- **Internet watchdog** — pings Google/Cloudflare DNS and reports connectivity losses
- **NTP time sync** — clock synchronized daily via Brazilian NTP pool
- **Authenticated web UI** — nonce + SHA-256 login with OPERATOR/ADMIN roles, per-IP lockout and persistent sessions
- **First-boot setup portal** — an unconfigured board raises its own Wi-Fi AP
  and writes its own `/config.json` from a board template, so provisioning needs
  no serial cable. A board that has ever joined a network can never fall back
  into it; see [First boot](#first-boot--the-setup-portal)
- **Over-The-Air firmware update** — from the web UI (ADMIN only) or pushed from ThingsBoard over MQTT
- **Remote commands** — ThingsBoard RPC for relay control and status, with the same guards as the web UI

## Getting Started

### Prerequisites

- [VS Code](https://code.visualstudio.com) + [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode)
- Git

### Build & Flash

```bash
# Clone
git clone https://github.com/andrenepomuceno/esp-garden.git
cd esp-garden

# Build a specific environment
pio run -e espgarden1

# Upload firmware
pio run -e espgarden1 --target upload

# Upload filesystem (LittleFS — config + web assets)
pio run -e espgarden1 --target uploadfs

# Open serial monitor
pio device monitor
```

### First boot — the setup portal

A board with no usable `/config.json` raises **its own access point** instead of
sitting unreachable:

- SSID **`espgarden-<id>`**, password **`espgarden`**, page at
  `http://192.168.4.1/` (or `http://espgarden-<id>.local/`). A captive-portal
  DNS answers every name, so most phones open the page on their own.
- Pick a board template, type the Wi-Fi network and password and an admin
  account, and submit. The device writes its own `/config.json` — **including
  the `id`, which it reads from its own efuse MAC**, so the one field that
  bricks a board cannot be typed wrong — and restarts onto the network.
- Everything else is configured from `/devices.html`, `/config.html` and
  `/schedules.html` once it is on the LAN. The templates offered are only the
  ones for the chip the image was built for.

**Read this before using it on a network you care about.** The AP password is
published here and the setup endpoint takes no token, so while the portal is up
anyone in radio range can claim the device. What bounds that is *when* the
portal can exist at all: the first boot that successfully joins a network
deletes a marker file, and from then on the board can never raise the AP again —
not after a router reboot, not after an outage. A board that has been on your
network once is outside this path for good.

The portal also comes back **once** if the credentials you typed never
associate: the marker is still there, so a mistyped Wi-Fi password is fixable
instead of terminal.

If you would rather provision from a workstation, `scripts/provision_config.py`
writes `data/config.json` from a template with the same refusals.

## Documentation

| Document | What is in it |
|---|---|
| [docs/hardware.md](docs/hardware.md) | Boards, sensors and pinouts, including the `espgarden_s3` carrier |
| [docs/configuration.md](docs/configuration.md) | Every `config.json` key, the shapes each one accepts, and what the build still decides |
| [docs/authentication.md](docs/authentication.md) | The nonce + SHA-256 login, roles, sessions, the partition table and the reservoir interlock |
| [docs/web-interface.md](docs/web-interface.md) | The pages, the endpoints and the `/data.json` contract |
| [docs/soil-moisture.md](docs/soil-moisture.md) | The Dry/Humid/Wet classifier, its calibration and why it refuses |
| [docs/soil-moisture-sampling.md](docs/soil-moisture-sampling.md) | `io.soilMoisturePeriodSec` and the task that reads the probe bank |
| [docs/evapotranspiration.md](docs/evapotranspiration.md) | Hargreaves-Samani ET0, and why it ships disabled |
| [docs/cloud-cover.md](docs/cloud-cover.md) | The clear-sky reference and its clearness index |
| [docs/telemetry.md](docs/telemetry.md) | ThingSpeak and ThingsBoard, RPC and cloud FOTA, TalkBack |
| [docs/task-schedule.md](docs/task-schedule.md) | Every task, its period and its bucket |
| [docs/tooling.md](docs/tooling.md) | The PC-side scripts under `scripts/` |

`CLAUDE.md` is the operating guide for working in this repository.

## Dependencies

| Library | Purpose |
|---|---|
| [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) | Async HTTP server |
| [CriticalTaskScheduler](https://github.com/andrenepomuceno/CriticalTaskScheduler) | Cooperative task scheduler |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MQTT client |
| [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) | DHT11/22 driver |
| [ESP32Ping](https://github.com/marian-craciunescu/ESP32Ping) | ICMP ping |
| [Arduino_JSON](https://github.com/arduino-libraries/Arduino_JSON) | JSON parsing |

## Live Data

[ThingSpeak Live Data](https://thingspeak.com/channels/1348790)

## Useful Links

- [Arduino core for the ESP32](https://github.com/espressif/arduino-esp32)
- [Espressif IoT Development Framework](https://github.com/espressif/esp-idf)
- [PlatformIO Documentation](https://docs.platformio.org)
- [ThingSpeak Documentation](https://www.mathworks.com/help/thingspeak/)
