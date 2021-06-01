#include "tasks.h"
#include "secret.h"
#include "talkback.h"
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <TaskScheduler.h>
#include <ThingSpeak.h>
#include <WiFi.h>

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
static const unsigned g_wateringPin = 22;
static const unsigned g_dhtPin = 23;

static const unsigned g_thingSpeakTaskPeriod = 60 * 1000;
static const unsigned g_clockUpdateTaskPeriod = 24 * 60 * 60 * 1000;
static const unsigned g_dhtTaskPeriod = 10 * 1000;
static const unsigned g_ioTaskPeriod = 1000;
static const unsigned g_wateringTaskPeriod = 1000;
static const unsigned g_talkBackTaskPeriod = 5 * 60 * 1000;

static const unsigned g_soilMoistureField = 1;
static const unsigned g_wateringField = 2;
static const unsigned g_luminosityField = 5;
static const unsigned g_temperatureField = 6;
static const unsigned g_airHumidityField = 7;
static const unsigned g_bootTimeField = 8;

static const unsigned g_wateringTime = 10;

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
unsigned g_packagesSent = 0;
unsigned g_tsErrors = 0;
time_t g_tsLastError = 0;
int g_tsLastCode = 200;
unsigned g_dhtReadErrors = 0;

unsigned g_wateringCycles = 0;

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
    float soilMoisture = 100.0 - (analogRead(A0) * 100.0) / 4095.0;
    g_soilMoisture.add(soilMoisture);
    float luminosity = (analogRead(A3) * 100.0) / 4095.0;
    g_luminosity.add(luminosity);

    g_buttonState = (digitalRead(g_buttonPin) > 0) ? (false) : (true);
    g_wateringState = (digitalRead(g_wateringPin) > 0) ? (true) : (false);
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
    ThingSpeak.setField(g_soilMoistureField, g_soilMoisture.getAverage());
    ThingSpeak.setField(g_luminosityField, g_luminosity.getAverage());

    ThingSpeak.setField(g_temperatureField, g_temperature.getAverage());
    ThingSpeak.setField(g_airHumidityField, g_airHumidity.getAverage());

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
    }

    g_soilMoisture.resetAverage();
    g_luminosity.resetAverage();
    g_temperature.resetAverage();
    g_airHumidity.resetAverage();
}

void
clockUpdateTaskHandler()
{
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

    if (runs == 1) {
        ThingSpeak.setField(g_wateringField, static_cast<int>(g_wateringTime));
        digitalWrite(g_wateringPin, 1);
    } else if (runs > g_wateringTime) {
        digitalWrite(g_wateringPin, 0);
        g_wateringTask.disable();
    }
}

static void
talkBackTaskHandler()
{
    String response;

    digitalWrite(LED_BUILTIN, 1);
    if (talkBack.execute(response) == false) {
        return;
    }
    digitalWrite(LED_BUILTIN, 0);

    if (response.indexOf("watering") != -1) {
        startWatering();
    }
}

void
tasksSetup()
{
    pinMode(g_buttonPin, INPUT);

    pinMode(g_wateringPin, OUTPUT);
    digitalWrite(g_wateringPin, 0);

    g_dht.begin();

    digitalWrite(LED_BUILTIN, 1);
    Serial.print("Updating clock...");
    do {
        clockUpdateTaskHandler();
        delay(2000);
        g_bootTime = time(NULL);
        Serial.print(".");
    } while (g_bootTime < 60 * 60);
    Serial.println("");
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
startWatering()
{
    ++g_wateringCycles;
    g_wateringTask.enable();
}