#include "tasks.h"
#include "logger.h"
#include "secret.h"
#include "talkback.h"
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <TaskScheduler.h>
#include <ThingSpeak.h>
#include <WiFi.h>

#define FLOAT_TO_STRING(x) (String(x, 2))
#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

static void
ioTaskHandler();
static void
dhtTaskHandler();
static void
thingSpeakTaskHandler();
static void
clockUpdateTaskHandler();
static void
wateringTaskHandler();
static void
talkBackTaskHandler();

static const unsigned g_buttonPin = 0;
static const unsigned g_wateringPin = 15;
static const unsigned g_dhtPin = 23;

static const unsigned g_thingSpeakTaskPeriod = 2 * 60 * 1000;
static const unsigned g_clockUpdateTaskPeriod = 24 * 60 * 60 * 1000;
static const unsigned g_dhtTaskPeriod = 10 * 1000;
static const unsigned g_ioTaskPeriod = 1000;
static const unsigned g_wateringTaskPeriod = 100;
static const unsigned g_talkBackTaskPeriod = 5 * 60 * 1000;

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

static const time_t g_safeTimestamp = 1609459200; // 01/01/2021

static WiFiClient g_wifiClient;
static TalkBack talkBack;
static DHT_Unified g_dht(g_dhtPin, DHT11);

Accumulator g_soilMoisture;
Accumulator g_luminosity;
Accumulator g_temperature;
Accumulator g_airHumidity;

bool g_buttonState = false;
bool g_wateringState = false;

time_t g_bootTime = 0;

bool g_thingSpeakEnable = true;
unsigned g_packagesSent = 0;
unsigned g_tsErrors = 0;
time_t g_tsLastError = 0;
int g_tsLastCode = 200;
unsigned g_dhtReadErrors = 0;

unsigned g_wateringCycles = 0;
time_t g_lastWateringCycle = 0;

static Scheduler g_taskScheduler;
static Task g_ioTask(g_ioTaskPeriod,
                     TASK_FOREVER,
                     &ioTaskHandler,
                     &g_taskScheduler);
static Task g_dhtTask(g_dhtTaskPeriod,
                      TASK_FOREVER,
                      &dhtTaskHandler,
                      &g_taskScheduler);
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

static void
ioTaskHandler()
{
    g_soilMoisture.add(100.0 - ADC_TO_PERCENT(analogRead(A0)));
    g_luminosity.add(ADC_TO_PERCENT(analogRead(A3)));

    g_buttonState = (digitalRead(g_buttonPin) > 0) ? (false) : (true);
}

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

static void
thingSpeakTaskHandler()
{
    if (g_thingSpeakEnable == false) {
        return;
    }

    ThingSpeak.setField(g_soilMoistureField,
                        FLOAT_TO_STRING(g_soilMoisture.getAverage()));
    ThingSpeak.setField(g_luminosityField,
                        FLOAT_TO_STRING(g_luminosity.getAverage()));

    ThingSpeak.setField(g_temperatureField,
                        FLOAT_TO_STRING(g_temperature.getAverage()));
    ThingSpeak.setField(g_airHumidityField,
                        FLOAT_TO_STRING(g_airHumidity.getAverage()));

    digitalWrite(LED_BUILTIN, 1);
    int status =
      ThingSpeak.writeFields(g_thingSpeakChannelNumber, g_thingSpeakAPIKey);
    digitalWrite(LED_BUILTIN, 0);

    if (status == 200) {
        ++g_packagesSent;
    } else {
        ++g_tsErrors;
        g_tsLastError = time(NULL);
        g_tsLastCode = status;

        logger.println("ThingSpeak.writeFields failed with error " + String(status));
    }

    g_soilMoisture.resetAverage();
    g_luminosity.resetAverage();
    g_temperature.resetAverage();
    g_airHumidity.resetAverage();
}

void
clockUpdateTaskHandler()
{
    logger.println("Starting clock update...");

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
        // digitalWrite(g_wateringPin, 1);
        ledcWrite(g_wateringPWMChannel, 0);
        g_wateringState = true;
    } else if (elapsedTime > g_wateringTime) {
        // digitalWrite(g_wateringPin, 0);
        ledcWrite(g_wateringPWMChannel, 0);
        g_wateringTime = g_wateringDefaultTime;
        g_wateringTask.disable();
        g_wateringState = false;
    } else {
        // start the pump gently
        if (elapsedTime <= g_wateringPWMTime) {
            ledcWrite(g_wateringPWMChannel,
                      (elapsedTime * 1023) / g_wateringPWMTime);
        }
    }
}

static void
talkBackTaskHandler()
{
    if (g_thingSpeakEnable == false) {
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
        int wateringTime = response.substring(index + 1).toInt();
        startWatering(wateringTime);
    }
}

void
tasksSetup()
{
    pinMode(g_buttonPin, INPUT);

    pinMode(g_wateringPin, OUTPUT);
    digitalWrite(g_wateringPin, 0);
    ledcAttachPin(g_wateringPin, 0);
    ledcSetup(g_wateringPWMChannel, 10e3, 10);
    ledcWrite(g_wateringPWMChannel, 0);

    g_dht.begin();

    digitalWrite(LED_BUILTIN, 1);
    do {
        clockUpdateTaskHandler();
        delay(2000);
        g_bootTime = time(NULL);
    } while (g_bootTime < g_safeTimestamp);
    digitalWrite(LED_BUILTIN, 0);

    ThingSpeak.begin(g_wifiClient);
    ThingSpeak.setField(g_bootTimeField, g_bootTime);

    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);

    g_ioTask.enableDelayed(1000);
    g_dhtTask.enableDelayed(1000);
    g_thingSpeakTask.enableDelayed(g_thingSpeakTaskPeriod);
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
}

void
tasksLoop()
{
    g_taskScheduler.execute();
}

void
startWatering(unsigned int wateringTime)
{
    logger.println("Starting watering for " + String(wateringTime) + " ms");

    if ((wateringTime == 0) || (wateringTime > g_wateringMaxTime)) {
        return;
    }

    if (g_wateringTask.isEnabled() == false) {
        g_wateringTime = wateringTime;
        ++g_wateringCycles;
        g_lastWateringCycle = time(NULL);
        g_wateringTask.enable();
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
    
    g_thingSpeakEnable = enable;
}