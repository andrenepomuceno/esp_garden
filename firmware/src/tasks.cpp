#include "tasks.h"
#include "logger.h"
#include "talkback.h"
#include "web.h"
#ifdef HAS_DHT_SENSOR
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#endif
#include <ESP32Ping.h>
#include <TaskScheduler.h>
#include <ThingSpeak.h>
#include <WiFi.h>

#define FLOAT_TO_STRING(x) (String(x, 2))
#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

static void
ioTaskHandler();
#ifdef HAS_DHT_SENSOR
static void
dhtTaskHandler();
#endif
static void
thingSpeakTaskHandler();
static void
clockUpdateTaskHandler();
static void
wateringTaskHandler();
static void
talkBackTaskHandler();
static void
checkInternetTaskHandler();
#ifdef HAS_MOISTURE_SENSOR
static void
checkMoistureTaskHandler();
#endif

static const unsigned g_thingSpeakTaskPeriod = 2 * 60 * 1000;
static const unsigned g_clockUpdateTaskPeriod = 24 * 60 * 60 * 1000;
static const unsigned g_dhtTaskPeriod = 10 * 1000;
static const unsigned g_ioTaskPeriod = 1000;
static const unsigned g_wateringTaskPeriod = 100;
static const unsigned g_talkBackTaskPeriod = 5 * 60 * 1000;
static const unsigned g_checkInternetTaskPeriod = 30 * 1000;
static const unsigned g_checkMoistureTaskPeriod = 30 * 60 * 1000;

static const unsigned g_soilMoistureField = 1;
static const unsigned g_wateringField = 2;
static const unsigned g_luminosityField = 5;
static const unsigned g_temperatureField = 6;
static const unsigned g_airHumidityField = 7;
static const unsigned g_bootTimeField = 8;

const unsigned int g_wateringDefaultTime = 5 * 1000;
static const unsigned g_wateringMaxTime = 20 * 1000;
static const unsigned g_wateringPWMChannel = 0;
static const unsigned g_wateringPWMTime = 2 * 1000;
static unsigned g_wateringTime = g_wateringDefaultTime;

static WiFiClient g_wifiClient;
static TalkBack talkBack;

#ifdef HAS_DHT_SENSOR
static DHT_Unified g_dht(g_dhtPin, DHT11);
Accumulator g_temperature;
Accumulator g_airHumidity;
unsigned g_dhtReadErrors = 0;
#endif
#ifdef HAS_MOISTURE_SENSOR
Accumulator g_soilMoisture;
static float g_moistureBeforeWatering = 0.0;
#endif
#ifdef HAS_LUMINOSITY_SENSOR
Accumulator g_luminosity;
#endif

bool g_wateringState = false;
bool g_hasInternet = false;
time_t g_bootTime = 0;
bool g_thingSpeakEnabled = true;
unsigned g_packagesSent = 0;
unsigned g_wateringCycles = 0;

static Scheduler g_taskScheduler;
static Task g_ioTask(g_ioTaskPeriod,
                     TASK_FOREVER,
                     &ioTaskHandler,
                     &g_taskScheduler);
#ifdef HAS_DHT_SENSOR
static Task g_dhtTask(g_dhtTaskPeriod,
                      TASK_FOREVER,
                      &dhtTaskHandler,
                      &g_taskScheduler);
#endif
static Task g_thingSpeakTask(g_thingSpeakTaskPeriod,
                             TASK_FOREVER,
                             &thingSpeakTaskHandler,
                             &g_taskScheduler);
static Task g_clockUpdateTask(g_clockUpdateTaskPeriod,
                              TASK_FOREVER,
                              &clockUpdateTaskHandler,
                              &g_taskScheduler);
static Task g_wateringTask(g_wateringTaskPeriod,
                           TASK_FOREVER,
                           &wateringTaskHandler,
                           &g_taskScheduler);
static Task g_talkBackTask(g_talkBackTaskPeriod,
                           TASK_FOREVER,
                           &talkBackTaskHandler,
                           &g_taskScheduler);
static Task g_checkInternetTask(g_checkInternetTaskPeriod,
                                TASK_FOREVER,
                                &checkInternetTaskHandler,
                                &g_taskScheduler);
#ifdef HAS_MOISTURE_SENSOR
static Task g_checkMoistureTask(g_checkMoistureTaskPeriod,
                                TASK_FOREVER,
                                &checkMoistureTaskHandler,
                                &g_taskScheduler);
#endif

static void
ioTaskHandler()
{
#ifdef HAS_MOISTURE_SENSOR
    g_soilMoisture.add(100.0 - ADC_TO_PERCENT(analogRead(g_soilMoisturePin)));
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    g_luminosity.add(ADC_TO_PERCENT(analogRead(g_luminosityPin)));
#endif
}

#ifdef HAS_DHT_SENSOR
static void
dhtTaskHandler()
{
    sensors_event_t event;
    g_dht.temperature().getEvent(&event);
    if (isnan(event.temperature) == false) {
        g_temperature.add(event.temperature);
    } else {
        ++g_dhtReadErrors;
    }
    g_dht.humidity().getEvent(&event);
    if (isnan(event.relative_humidity) == false) {
        g_airHumidity.add(event.relative_humidity);
    } else {
        ++g_dhtReadErrors;
    }
}
#endif

static void
thingSpeakTaskHandler()
{
    if (!g_thingSpeakEnabled || !g_hasInternet) {
        return;
    }

#ifdef HAS_MOISTURE_SENSOR
    ThingSpeak.setField(g_soilMoistureField,
                        FLOAT_TO_STRING(g_soilMoisture.getAverage()));
    g_soilMoisture.resetAverage();
#endif
#ifdef HAS_LUMINOSITY_SENSOR
    ThingSpeak.setField(g_luminosityField,
                        FLOAT_TO_STRING(g_luminosity.getAverage()));
    g_luminosity.resetAverage();
#endif
#ifdef HAS_DHT_SENSOR
    ThingSpeak.setField(g_temperatureField,
                        FLOAT_TO_STRING(g_temperature.getAverage()));
    ThingSpeak.setField(g_airHumidityField,
                        FLOAT_TO_STRING(g_airHumidity.getAverage()));
    g_temperature.resetAverage();
    g_airHumidity.resetAverage();
#endif

    digitalWrite(LED_BUILTIN, 1);
    int status =
      ThingSpeak.writeFields(g_thingSpeakChannelNumber, g_thingSpeakAPIKey);
    digitalWrite(LED_BUILTIN, 0);

    if (status == 200) {
        ++g_packagesSent;
    } else {
        logger.println("ThingSpeak.writeFields failed with error " +
                       String(status));
    }
}

void
clockUpdateTaskHandler()
{
    logger.println("Syncing clock...");

    if (!g_hasInternet) {
        logger.println("Syncing skipped, no internet connection.");
        return;
    }

    const int tzOffset = -3 * 60 * 60;
    configTime(0,
               tzOffset,
               "0.br.pool.ntp.org",
               "1.br.pool.ntp.org",
               "2.br.pool.ntp.org");
}

static void
wateringTaskHandler()
{
    auto runs = g_wateringTask.getRunCounter();
    unsigned elapsedTime = (runs - 1) * g_wateringTaskPeriod;

    if (runs == 1) {
        ThingSpeak.setField(g_wateringField, static_cast<int>(g_wateringTime));
        #if !USE_WATERING_PWM
            digitalWrite(g_wateringPin, g_wateringPinOn);
        #else
            ledcWrite(g_wateringPWMChannel, !g_wateringPinOn);
        #endif
        g_wateringState = true;

#ifdef HAS_MOISTURE_SENSOR
        g_moistureBeforeWatering = g_soilMoisture.getLastAvg();
#endif
    } else if (elapsedTime > g_wateringTime) {
        #if !USE_WATERING_PWM
            digitalWrite(g_wateringPin, !g_wateringPinOn);
        #else
            ledcWrite(g_wateringPWMChannel, !g_wateringPinOn);
        #endif
        
        g_wateringTime = g_wateringDefaultTime;
        g_wateringTask.disable();
        g_wateringState = false;

#ifdef HAS_MOISTURE_SENSOR
        g_checkMoistureTask.enableDelayed(g_checkMoistureTaskPeriod);
#endif
    } else {
        #if USE_WATERING_PWM
            // start the pump gently
            if (elapsedTime <= g_wateringPWMTime) {
                ledcWrite(g_wateringPWMChannel,
                        (elapsedTime * 1023) / g_wateringPWMTime);
            }
        #endif
    }
}

static void
talkBackTaskHandler()
{
    if (!g_thingSpeakEnabled || !g_hasInternet) {
        return;
    }

    String response;

    digitalWrite(LED_BUILTIN, 1);
    if (talkBack.execute(response) == false) {
        logger.println("TalkBack failure.");
        return;
    }
    digitalWrite(LED_BUILTIN, 0);

    if (response.indexOf("watering:") != -1) {
        int index = response.indexOf(":");
        String timeStr = response.substring(index + 1);
        if (timeStr.length() > 0) {
            logger.println("Executing TalkBack watering task.");
            int wateringTime = timeStr.toInt();
            startWatering(wateringTime);
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
                logger.println("Internet connection detected!");

                if (connectionLostTime != 0) {
                    time_t downTime = time(NULL) - connectionLostTime;
                    logger.println("Down time: " + String(downTime) + " s");
                }
            }

            g_hasInternet = true;
            return;
        }
    }

    if (g_hasInternet) {
        logger.println("Internet connection lost.");
        g_hasInternet = false;
        connectionLostTime = time(NULL);
    }
}

#ifdef HAS_MOISTURE_SENSOR
static void
checkMoistureTaskHandler()
{
    float moistureDelta =
      g_soilMoisture.getLastAvg() - g_moistureBeforeWatering;

    if (moistureDelta < 5.0) {
        logger.println("Maybe we are out of water...");
        logger.println("Delta: " + FLOAT_TO_STRING(moistureDelta));
    }

    g_checkMoistureTask.disable();
}
#endif

void
tasksSetup()
{
    pinMode(g_buttonPin, INPUT);

    pinMode(g_wateringPin, OUTPUT);
    digitalWrite(g_wateringPin, !g_wateringPinOn);
    #if USE_WATERING_PWM
        ledcAttachPin(g_wateringPin, 0);
        ledcSetup(g_wateringPWMChannel, 10e3, 10);
        ledcWrite(g_wateringPWMChannel, !g_wateringPinOn);
    #endif

    ThingSpeak.begin(g_wifiClient);

    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);

    logger.println("Checking internet connection...");
    while (!g_hasInternet) {
        checkInternetTaskHandler();
        delay(1000);
    }
    while (g_bootTime < g_safeTimestamp) {
        clockUpdateTaskHandler();
        delay(2000);
        g_bootTime = time(NULL);
    }

    ThingSpeak.setField(g_bootTimeField, g_bootTime);

    g_ioTask.enableDelayed(g_ioTaskPeriod);
#ifdef HAS_DHT_SENSOR
    g_dht.begin();
    g_dhtTask.enableDelayed(g_dhtTaskPeriod);
#endif
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_thingSpeakTask.enableDelayed(g_thingSpeakTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);

    logger.println("Tasks setup done!");
}

void
tasksLoop()
{
    g_taskScheduler.execute();
}

void
startWatering(unsigned int wateringTime)
{
    if ((wateringTime == 0) || (wateringTime > g_wateringMaxTime)) {
        logger.println("Invalid watering time: " + String(wateringTime));
        return;
    }

    if (g_wateringTask.isEnabled() == false) {
        logger.println("Starting watering for " + String(wateringTime) + " ms");

        g_wateringTime = wateringTime;
        ++g_wateringCycles;
        g_wateringTask.enable();
    } else {
        logger.println("Watering already enabled.");
    }
}

void
thingSpeakEnable(bool enable)
{
    if (enable == true) {
        logger.println("ThinkSpeak enabled.");
    } else {
        logger.println("ThinkSpeak disabled.");
    }

    g_thingSpeakEnabled = enable;
}
