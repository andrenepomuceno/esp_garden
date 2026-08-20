#include "core/tasks.h"
#include "BuildConfig.h"
#include "core/logger.h"
#include "network/mqtt.h"
#include "network/talkback.h"
#include "network/web.h"
#include <CriticalTaskScheduler.h>
#include <ESP32Ping.h>
#include <WiFi.h>
#include <list>
#include <new>
#ifdef HAS_DHT_SENSOR
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#endif

#define FLOAT_TO_STRING(x) (String(x, 2))
#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

#define DECLARE_TASK(name, period)                                             \
    static void name##TaskHandler();                                           \
    static const unsigned g_##name##TaskPeriod = period;                       \
    static TSTask g_##name##Task(#name, g_##name##TaskPeriod,                  \
                                 &name##TaskHandler)

// Critical tasks run on a dedicated FreeRTOS task instead of the cooperative
// loop() pump, so a blocking background handler cannot delay them.
#define DECLARE_CRITICAL_TASK(name, period)                                    \
    static void name##TaskHandler();                                           \
    static const unsigned g_##name##TaskPeriod = period;                       \
    static TSTask g_##name##Task(#name, g_##name##TaskPeriod,                  \
                                 &name##TaskHandler, true)

static TSScheduler g_taskScheduler;
static TSFreeRTOSCriticalRunner g_criticalRunner(g_taskScheduler);

DECLARE_TASK(io, 1000);                         // 1 s
DECLARE_TASK(clockUpdate, 24 * 60 * 60 * 1000); // 24 h
DECLARE_TASK(checkInternet, 15 * 1000);         // 15 s
DECLARE_TASK(logBackup, 60 * 60 * 1000);        // 1 h
DECLARE_TASK(mqtt, 1 * 60 * 1000);              // 1 min
DECLARE_TASK(talkBack, 1 * 60 * 1000);          // 1 min
#ifdef HAS_MOISTURE_SENSOR
DECLARE_TASK(checkMoisture, 4 * 60 * 60 * 1000); // 4 h
#endif
#ifdef HAS_DHT_SENSOR
// At the DHT11's sampling floor: the Adafruit driver returns its cached
// reading rather than an error when polled faster than once per second.
DECLARE_TASK(dht, 1 * 1000); // 1 s
#endif

// Switching a pump off on time is the one deadline in this firmware that has a
// physical cost when missed, so it does not share the cooperative pump with
// Ping, TalkBack and the MQTT drain — any of which blocks for seconds.
DECLARE_CRITICAL_TASK(relays, CRITICAL_TASKS_PERIOD_MS);

// Critical for a different reason: it is the only indication that the config
// failed to load, and that failure makes tasksSetup() block forever waiting for
// an internet connection the device cannot get. On the cooperative pump the
// error blink would never run in precisely the case it exists to report.
DECLARE_CRITICAL_TASK(ledBlink, 1000);

// ThingSpeak channels have exactly 8 fields and the numbering is a permanent
// contract with the data already stored. Relays 1..N have no field — they are
// local-only.
static const unsigned g_soilMoistureField = 1;
static const unsigned g_wateringField = 2;
static const unsigned g_pingField = 3;
#ifdef HAS_WATER_LEVEL_SENSOR
static const unsigned g_waterLevelField = 4;
#endif
static const unsigned g_luminosityField = 5;
static const unsigned g_temperatureField = 6;
static const unsigned g_airHumidityField = 7;
static const unsigned g_bootTimeField = 8;

// Second soil probe. Field 4 is free only on a board without a water level
// sensor; where both exist the slot has to be chosen deliberately, because
// reusing a number rewrites the meaning of everything already stored under it.
// Set -D MOISTURE2_FIELD=<n> in platformio.ini to pick one; leaving it unset
// keeps the probe on the dashboard and out of the cloud.
#if (MOISTURE_SENSOR_COUNT > 1) && !defined(MOISTURE2_FIELD) &&                \
  !defined(HAS_WATER_LEVEL_SENSOR)
#define MOISTURE2_FIELD 4
#endif

#if (MOISTURE_SENSOR_COUNT > 1) && !defined(MOISTURE2_FIELD)
#warning "Second soil probe is dashboard-only: no ThingSpeak field assigned (set -D MOISTURE2_FIELD=<n>)."
#endif

const unsigned int g_wateringDefaultTime = 5 * 1000;
static const unsigned g_relayMaxTime = 30 * 1000;

AccumulatorV2 g_pingTime(g_mqttTaskPeriod / g_checkInternetTaskPeriod);
static String g_mqttMessage = "";

#if USE_WATERING_PWM
static const unsigned g_wateringPWMChannel = 0;
static const unsigned g_wateringPWMTime = 2 * 1000;
#endif

#ifdef HAS_DHT_SENSOR
// Constructed in tasksSetup(), not at static-init time: DHT_Unified copies the
// pin in its constructor, and at static-init config.json has not been read yet,
// so a file-scope instance permanently runs on the compiled default pin.
alignas(DHT_Unified) static uint8_t g_dhtStorage[sizeof(DHT_Unified)];
static DHT_Unified* g_dht = nullptr;
AccumulatorV2 g_temperature(g_mqttTaskPeriod / g_dhtTaskPeriod);
AccumulatorV2 g_airHumidity(g_mqttTaskPeriod / g_dhtTaskPeriod);
unsigned g_dhtReadErrors = 0;
unsigned g_dhtTotalReads = 0;
#endif

#ifdef HAS_MOISTURE_SENSOR
AccumulatorV2 g_soilMoisture[MOISTURE_SENSOR_COUNT];
static float g_moistureBeforeWatering[MOISTURE_SENSOR_COUNT] = { 0.0 };
#endif

#ifdef HAS_LUMINOSITY_SENSOR
AccumulatorV2 g_luminosity(g_mqttTaskPeriod / g_ioTaskPeriod);
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
#define ADC_TO_WATER_LEVEL(v) (9.0 - 12.0 * sin(4.04 - 1.61 * (3.3 * v / 4095.0)))
AccumulatorV2 g_waterLevel(g_mqttTaskPeriod / g_ioTaskPeriod);
#endif

void
mqttAddField(int field, String val);
void
mqttAddStatus(String status);

struct RelayState
{
    bool on;
    unsigned long startTime;
    unsigned long duration;
};

static RelayState g_relay[RELAY_COUNT] = {};

// The relay task runs on its own FreeRTOS task while startRelay() is called
// from loop() (TalkBack) and from async_tcp (the /control handler), so every
// read-modify-write of g_relay goes through this spinlock.
static portMUX_TYPE g_relayMux = portMUX_INITIALIZER_UNLOCKED;

static WiFiClient g_wifiClient;
static TalkBack talkBack;

bool g_hasInternet = false;
time_t g_bootTime = 0;
bool g_mqttEnabled = true;
unsigned g_packagesSent = 0;
unsigned g_wateringCycles = 0;
bool g_ledBlinkEnabled = false;
unsigned g_connectionLossCount = 0;

static void
relayWrite(unsigned index, bool on)
{
    const uint8_t level = on ? config.relayPinOn[index] : !config.relayPinOn[index];

#if USE_WATERING_PWM
    if (index == 0) {
        ledcWrite(g_wateringPWMChannel, level ? 1023 : 0);
        return;
    }
#endif

    digitalWrite(config.relayPin[index], level);
}

static void
relaysTaskHandler()
{
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        bool expired = false;

        portENTER_CRITICAL(&g_relayMux);
        if (g_relay[i].on &&
            (millis() - g_relay[i].startTime >= g_relay[i].duration)) {
            g_relay[i].on = false;
            expired = true;
        }
        portEXIT_CRITICAL(&g_relayMux);

        if (expired) {
            relayWrite(i, false);
        }
    }
}

bool
relayIsOn(unsigned index)
{
    if (index >= RELAY_COUNT) {
        return false;
    }

    portENTER_CRITICAL(&g_relayMux);
    const bool on = g_relay[index].on;
    portEXIT_CRITICAL(&g_relayMux);

    return on;
}

unsigned long
relayRemaining(unsigned index)
{
    if (index >= RELAY_COUNT) {
        return 0;
    }

    portENTER_CRITICAL(&g_relayMux);
    const RelayState state = g_relay[index];
    portEXIT_CRITICAL(&g_relayMux);

    if (!state.on) {
        return 0;
    }

    const unsigned long elapsed = millis() - state.startTime;
    return (elapsed >= state.duration) ? 0 : (state.duration - elapsed);
}

void
startRelay(unsigned index, unsigned int duration)
{
    if (index >= RELAY_COUNT) {
        logger.error("Invalid relay index: " + String(index));
        return;
    }

    if ((duration == 0) || (duration > g_relayMaxTime)) {
        logger.error("Invalid relay time: " + String(duration));
        return;
    }

    bool started = false;

    portENTER_CRITICAL(&g_relayMux);
    if (!g_relay[index].on) {
        g_relay[index].on = true;
        g_relay[index].startTime = millis();
        g_relay[index].duration = duration;
        started = true;
    }
    portEXIT_CRITICAL(&g_relayMux);

    if (!started) {
        logger.warning(config.relayName[index] + " already active.");
        return;
    }

    logger.info("Starting " + config.relayName[index] + " for " +
                String(duration) + " ms");
    relayWrite(index, true);

    if (index == 0) {
        ++g_wateringCycles;
        mqttAddField(g_wateringField, String(duration));

#ifdef HAS_MOISTURE_SENSOR
        for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
            g_moistureBeforeWatering[i] = g_soilMoisture[i].getAverage();
        }
        g_checkMoistureTask.enableDelayed(g_checkMoistureTaskPeriod);
#endif
    }
}

void
startWatering(unsigned int wateringTime)
{
    startRelay(0, wateringTime);
}

#ifdef HAS_MOISTURE_SENSOR
String
moistureState(unsigned index)
{
    if (index >= MOISTURE_SENSOR_COUNT) {
        return String();
    }

    const float dry = config.moistureDry[index];
    const float wet = config.moistureWet[index];
    const float span = wet - dry;

    // Uncalibrated. Reporting a band from an unknown scale would be a guess
    // dressed as a measurement, so the probe reports no state at all.
    if (fabsf(span) < 1e-3) {
        return String();
    }

    // Ordering is not assumed: with the 100-ADC% conversion the air reading is
    // the smaller number, but a different probe or conversion can invert that.
    float fraction = (g_soilMoisture[index].getAverage() - dry) / span;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }

    if (fraction < 1.0 / 3.0) {
        return String("Dry");
    }
    if (fraction < 2.0 / 3.0) {
        return String("Humid");
    }
    return String("Wet");
}
#endif

static void
ioTaskHandler()
{
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        g_soilMoisture[i].add(
          100.0 - ADC_TO_PERCENT(analogRead(config.soilMoisturePin[i])));
    }
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    g_luminosity.add(ADC_TO_PERCENT(analogRead(config.luminosityPin)));
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    g_waterLevel.add(ADC_TO_WATER_LEVEL(analogRead(config.waterLevelPin)));
#endif

    // Same task, same thread as the accumulator writes above — the request
    // handler must never walk these lists itself.
    webUpdateDataCache();
}

#ifdef HAS_DHT_SENSOR
static void
dhtTaskHandler()
{
    if (g_dht == nullptr) {
        return;
    }

    sensors_event_t event;
    bool error = false;

    g_dht->temperature().getEvent(&event);
    if (isnan(event.temperature) == false) {
        g_temperature.add(event.temperature);
    } else {
        error = true;
    }

    g_dht->humidity().getEvent(&event);
    if (isnan(event.relative_humidity) == false) {
        g_airHumidity.add(event.relative_humidity);
    } else {
        error = true;
    }

    ++g_dhtTotalReads;
    if (error) {
        ++g_dhtReadErrors;
    }
}
#endif

void
mqttAddField(int field, String val)
{
    g_mqttMessage += "field" + String(field) + "=" + val + "&";
}

void
mqttAddStatus(String status)
{
    g_mqttMessage += "status='" + status + "'&";
}

void
mqttTaskHandler()
{
    static std::list<String> msgQueue;

    if (!g_mqttEnabled || !g_hasInternet) {
        logger.info("MQTT skipped.");
        logger.info("g_mqttEnabled = " + String(g_mqttEnabled) +
                    " g_hasInternet = " + String(g_hasInternet));
        return;
    }

#ifdef HAS_MOISTURE_SENSOR
    mqttAddField(g_soilMoistureField,
                 FLOAT_TO_STRING(g_soilMoisture[0].getAverage()));
#if (MOISTURE_SENSOR_COUNT > 1) && defined(MOISTURE2_FIELD)
    mqttAddField(MOISTURE2_FIELD,
                 FLOAT_TO_STRING(g_soilMoisture[1].getAverage()));
#endif
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    mqttAddField(g_luminosityField, FLOAT_TO_STRING(g_luminosity.getAverage()));
#endif

#ifdef HAS_DHT_SENSOR
    mqttAddField(g_temperatureField,
                 FLOAT_TO_STRING(g_temperature.getAverage()));
    mqttAddField(g_airHumidityField,
                 FLOAT_TO_STRING(g_airHumidity.getAverage()));
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    mqttAddField(g_waterLevelField, FLOAT_TO_STRING(g_waterLevel.getAverage()));
#endif

    mqttAddField(g_pingField, String(g_pingTime.getAverage()));

    char timestamp[64];
    time_t now = time(nullptr);
    strftime(timestamp, sizeof timestamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    g_mqttMessage += "created_at='" + String(timestamp) + "'";

    msgQueue.push_back(g_mqttMessage);
    g_mqttMessage = "";

    digitalWrite(LED_BUILTIN, 1);
    int errors = 0;
    const unsigned maxMsgQueueSize = 60 * 60 * 1000 / g_mqttTaskPeriod;
    while (msgQueue.size() > maxMsgQueueSize) {
        logger.warning("msgQueue is full, discarding messages..");
        msgQueue.pop_front();
    }
    while (msgQueue.size() > 0) {
        bool success = mqttPublish(g_thingSpeakChannelNumber, msgQueue.front());

        if (success) {
            ++g_packagesSent;
            msgQueue.pop_front();
            errors = 0;
        } else {
            logger.error("mqttPublish failed.");
            ++errors;
            if (errors > 3) {
                logger.warning("Giving up for now...");
                break;
            }
        }
    }
    digitalWrite(LED_BUILTIN, 0);
}

void
clockUpdateTaskHandler()
{
    logger.info("Syncing clock...");

    if (!g_hasInternet) {
        logger.warning("Syncing skipped, no internet connection.");
        return;
    }

    configTime(
      0, 0, "0.br.pool.ntp.org", "1.br.pool.ntp.org", "2.br.pool.ntp.org");

    setenv("TZ", g_timezone.c_str(), 1); // America/Sao Paulo
    tzset();
}

static void
talkBackTaskHandler()
{
    if (!g_mqttEnabled || !g_hasInternet) {
        return;
    }

    String response;

    digitalWrite(LED_BUILTIN, 1);
    if (talkBack.execute(response) == false) {
        logger.error("TalkBack failure.");
        return;
    }
    digitalWrite(LED_BUILTIN, 0);

    // watering:<ms>        -> relay 0, kept for existing TalkBack queues
    // relay:<index>:<ms>   -> any relay
    if (response.indexOf("relay:") != -1) {
        int first = response.indexOf("relay:") + 6;
        int second = response.indexOf(":", first);
        if (second != -1) {
            unsigned index = response.substring(first, second).toInt();
            unsigned time = response.substring(second + 1).toInt();
            logger.info("Executing TalkBack relay task.");
            startRelay(index, time);
        }
    } else if (response.indexOf("watering:") != -1) {
        int index = response.indexOf(":");
        String timeStr = response.substring(index + 1);
        if (timeStr.length() > 0) {
            logger.info("Executing TalkBack watering task.");
            startWatering(timeStr.toInt());
        }
    }
}

static void
checkInternetTaskHandler()
{
    if (!g_wifiConnected || !g_hasNetwork) {
        g_hasInternet = false;
        return;
    }

    static const size_t addresListLen = 3;
    static const IPAddress addressList[addresListLen] = {
        IPAddress(8, 8, 8, 8), IPAddress(8, 8, 4, 4), IPAddress(1, 1, 1, 1)
    };
    static time_t connectionLostTime = 0;

    for (int i = 0; i < addresListLen; ++i) {
        bool success = Ping.ping(addressList[i], 2); // retry at least one time
        if (success) {
            if (!g_hasInternet) {
                logger.info("Internet connection detected!");

                if (connectionLostTime != 0) {
                    time_t downTime = time(NULL) - connectionLostTime;
                    logger.info("Down time: " + String(downTime) + " s");

                    mqttAddStatus("Im back online! Downtime: " +
                                  String(downTime));
                }
            }
            float avgTime = Ping.averageTime();
            if (!isnan(avgTime) && avgTime > 0.0 && avgTime < 1e6) {
                g_pingTime.add(avgTime);
            } else {
                logger.warning("Invalid avgTime " + String(avgTime));
            }
            g_hasInternet = true;
            return;
        }
    }

    if (g_hasInternet) {
        logger.warning("Internet connection lost.");
        g_hasInternet = false;
        connectionLostTime = time(NULL);
        ++g_connectionLossCount;

        mqttAddStatus("Internet connection lost.");
    }
}

#ifdef HAS_MOISTURE_SENSOR
static void
checkMoistureTaskHandler()
{
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        float moistureDelta =
          g_soilMoisture[i].getAverage() - g_moistureBeforeWatering[i];

        if (moistureDelta < 0.5) {
            logger.warning("Probe " + String(i) +
                           ": no moisture gain after watering. Delta: " +
                           FLOAT_TO_STRING(moistureDelta));
        }
    }

    g_checkMoistureTask.disable();
}
#endif

static void
ledBlinkTaskHandler()
{
    static bool on = false;
    if (g_ledBlinkEnabled) {
        on = !on;
        digitalWrite(LED_BUILTIN, on);
    } else {
        digitalWrite(LED_BUILTIN, 0);
    }
}

static void
logBackupTaskHandler()
{
    logger.backup();
}

void
tasksSetup()
{
    logger.info("Tasks setup...");

    g_taskScheduler.addTask(&g_ioTask);
    g_taskScheduler.addTask(&g_relaysTask);
    g_taskScheduler.addTask(&g_ledBlinkTask);
    g_taskScheduler.addTask(&g_clockUpdateTask);
    g_taskScheduler.addTask(&g_checkInternetTask);
    g_taskScheduler.addTask(&g_logBackupTask);
    g_taskScheduler.addTask(&g_mqttTask);
    g_taskScheduler.addTask(&g_talkBackTask);
#ifdef HAS_MOISTURE_SENSOR
    g_taskScheduler.addTask(&g_checkMoistureTask);
#endif
#ifdef HAS_DHT_SENSOR
    g_taskScheduler.addTask(&g_dhtTask);
#endif

    // Every scalar accumulator sizes its window from the MQTT period at
    // construction, so each average covers exactly one publish interval. An
    // array cannot pass a constructor argument, so the probes are sized here
    // instead — otherwise they silently keep the 120-sample default and their
    // averages span a different interval from every other channel.
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        g_soilMoisture[i].setMaxLen(g_mqttTaskPeriod / g_ioTaskPeriod);
    }
#endif

    pinMode(config.buttonPin, INPUT);

    // Relay pins were already parked by relayPinsSafeInit() before and after
    // the config load; this only covers the PWM variant's channel setup.
#if USE_WATERING_PWM
    ledcAttachPin(config.relayPin[0], g_wateringPWMChannel);
    ledcSetup(g_wateringPWMChannel, 10e3, 10);
    ledcWrite(g_wateringPWMChannel, 0);
#endif

    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);

    // Relay timing must be live before the blocking waits below: they can hold
    // setup() for minutes, and a relay commanded in that window still has to
    // switch off on schedule.
    g_relaysTask.enable();
    g_ledBlinkTask.enable();
    if (!g_criticalRunner.start()) {
        logger.fatal("Failed to start the critical task runner.");
    }

    logger.info("Waiting for internet connection...");
    while (!g_hasInternet) {
        checkInternetTaskHandler();
        delay(1000);
    }
    while (g_bootTime < g_safeTimestamp) {
        clockUpdateTaskHandler();
        delay(2000);
        g_bootTime = time(NULL);
    }

    mqttSetup();
    mqttAddField(g_bootTimeField, String(g_bootTime));

    g_ioTask.enableDelayed(g_ioTaskPeriod);
#ifdef HAS_DHT_SENSOR
    g_dht = new (g_dhtStorage) DHT_Unified(config.dhtPin, DHT11);
    g_dht->begin();
    g_dhtTask.enableDelayed(g_dhtTaskPeriod);
#endif
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_mqttTask.enableDelayed(g_mqttTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
    g_logBackupTask.enableDelayed(g_logBackupTaskPeriod);

    logger.info("Tasks setup done!");
    logger.backup();
}

static volatile bool g_restartRequested = false;
static volatile unsigned long g_restartDeadline = 0;

void
requestRestart()
{
    g_restartDeadline = millis() + 500;
    g_restartRequested = true;
}

void
tasksLoop()
{
    g_taskScheduler.execute();
    mqttLoop();

    // Gives the queued HTTP response time to leave before the reboot.
    if (g_restartRequested && ((long)(millis() - g_restartDeadline) >= 0)) {
        logger.warning("Restarting on request.");
        ESP.restart();
    }
}

void
mqttEnable(bool enable)
{
    if (enable == true) {
        logger.info("MQTT enabled.");
    } else {
        logger.info("MQTT disabled.");
    }

    g_mqttEnabled = enable;
}
