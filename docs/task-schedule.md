# Task schedule

Tasks marked **critical** run on a dedicated FreeRTOS task; the rest share the
cooperative pump driven from `loop()`, where one blocking handler (ping,
TalkBack, an MQTT drain) stalls every other background task for its duration.

| Task | Period | Description |
|---|---|---|
| `relays` | 50 ms — **critical** | Switch each relay off when its timer expires |
| `ledBlink` | 1 s — **critical** | Blink built-in LED (enabled on config error) |
| `io` | 1 s | Read the luminosity, water-level, flow and float inputs and rebuild the `/data.json` payload |
| `moisture` | `io.soilMoisturePeriodSec` | Read the soil-moisture probe bank. Separate from `io` because that task also paces the relay, cloud and ET0 events and the `/data.json` cache, none of which may slow down with the ADC |
| `ambient` | 1 s | Read air temperature and humidity from whichever part is fitted — a DHT11 on one wire or an SHT40 on I²C. One task for both: a task per sensor kind is how a firmware reaches the 16-slot cap `addTask()` overruns silently |
| `checkInternet` | 15 s | Ping DNS servers, update connectivity state |
| `history` | `history.periodSec` | Append one I/O snapshot to the newest segment |
| `schedules` | 20 s | Fire any schedule that is due |
| `mqtt` | 1 min | Publish averaged sensor data to the configured backend |
| `talkBack` | 1 min | Poll ThingSpeak TalkBack for remote commands. **Not registered by default** — the whole task is behind `USE_TALKBACK`, which ships at 0 |
| `clockUpdate` | 24 h | Re-sync NTP clock |
| `moistureModel` | 24 h | Decay the stored moisture evidence and fold in the day's watering cycles |
| `logBackup` | 1 h | Flush serial log to LittleFS |
| `checkMoisture` | 4 h | Check soil moisture delta after watering |
