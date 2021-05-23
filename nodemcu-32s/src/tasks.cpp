#include "tasks.h"
#include "secret.h"
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <TaskScheduler.h>
#include <ThingSpeak.h>
#include <WiFi.h>

static WiFiClient g_wifiClient;

static const unsigned g_buttonGPIO = 0;
static const unsigned g_wateringGPIO = 22;
static const unsigned g_dhtPin = 23;

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

static DHT_Unified g_dht(g_dhtPin, DHT11);

static void
ioTaskHandler();
static void
dhtTaskHandler();
static void
tsTaskHandler();
static void
clockUpdateTaskHandler();
static void
wateringTaskHandler();

static const unsigned g_tsTaskPeriod = 60 * 1000;
static const unsigned g_clockUpdateTaskPeriod = 24 * 60 * 60 * 1000;
static const unsigned g_dhtTaskPeriod = 5 * 1000;
static const unsigned g_ioTaskPeriod = 1000;
static const unsigned g_wateringTaskPeriod = 1000;
static const unsigned g_wateringIterations = 10;

static Scheduler g_taskScheduler;
static Task g_ioTask(g_ioTaskPeriod,
                     TASK_FOREVER,
                     &ioTaskHandler,
                     &g_taskScheduler);
static Task g_dhtTask(g_dhtTaskPeriod,
                      TASK_FOREVER,
                      &dhtTaskHandler,
                      &g_taskScheduler);
static Task g_tsTask(g_tsTaskPeriod,
                     TASK_FOREVER,
                     &tsTaskHandler,
                     &g_taskScheduler);
static Task g_clockUpdateTask(g_clockUpdateTaskPeriod,
                              TASK_FOREVER,
                              &clockUpdateTaskHandler,
                              &g_taskScheduler);
static Task g_wateringTask(g_wateringTaskPeriod,
                           TASK_FOREVER,
                           &wateringTaskHandler,
                           &g_taskScheduler);

static void
ioTaskHandler()
{
  g_soilMoisture.add(analogRead(A0));
  g_luminosity.add(analogRead(A3));

  g_buttonState = (digitalRead(g_buttonGPIO) > 0) ? (false) : (true);
  g_wateringState = (digitalRead(g_wateringGPIO) > 0) ? (true) : (false);
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
tsTaskHandler()
{
  ThingSpeak.setField(1, g_soilMoisture.getAverage());
  ThingSpeak.setField(2, g_luminosity.getAverage());

  ThingSpeak.setField(6, g_temperature.getAverage());
  ThingSpeak.setField(7, g_airHumidity.getAverage());

  digitalWrite(LED_BUILTIN, 1);
  int status = ThingSpeak.writeFields(g_channelNumber, g_apiKey);
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
  int tzOffset = -3 * 60 * 60;
  configTime(
    0, tzOffset, "0.br.pool.ntp.org", "1.br.pool.ntp.org", "2.br.pool.ntp.org");
}

static void
wateringTaskHandler()
{
  auto runs = g_wateringTask.getRunCounter();

  if (runs == 1) {
    digitalWrite(g_wateringGPIO, 1);
  } else if (runs > g_wateringIterations) {
    digitalWrite(g_wateringGPIO, 0);
    g_wateringTask.disable();
  }
}

void
tasksSetup()
{
  pinMode(g_buttonGPIO, INPUT);

  pinMode(g_wateringGPIO, OUTPUT);
  digitalWrite(g_wateringGPIO, 0);

  g_dht.begin();

  Serial.println("Updating clock...");
  clockUpdateTaskHandler();
  delay(2000);
  g_bootTime = time(NULL);

  ThingSpeak.begin(g_wifiClient);
  ThingSpeak.setField(8, g_bootTime);

  g_ioTask.enable();
  g_dhtTask.enable();
  g_tsTask.enableDelayed(g_tsTaskPeriod);
  g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
}

void
tasksLoop()
{
  g_taskScheduler.execute();
}

void
startWatering()
{
  g_wateringTask.enable();
}