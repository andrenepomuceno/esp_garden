#include "tasks.h"
#include "ina3221.h"
#include "logger.h"
#include "mqtt.h"
#include "talkback.h"
#include "web.h"
#include <ESP32Ping.h>
#include <TaskScheduler.h>
#include <WiFi.h>
#include <list>
#include <INA3221.h>

#define FLOAT_TO_STRING(x) (String(x, 2))
#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

#define DECLARE_TASK(name, period)                                             \
    static void name##TaskHandler();                                           \
    static const unsigned g_##name##TaskPeriod = period;                       \
    static Task g_##name##Task(g_##name##TaskPeriod,                           \
                               TASK_FOREVER,                                   \
                               &name##TaskHandler,                             \
                               &g_taskScheduler)

static Scheduler g_taskScheduler;

DECLARE_TASK(io, 250);
DECLARE_TASK(ledBlink, 1000);                   // 1 s
DECLARE_TASK(clockUpdate, 24 * 60 * 60 * 1000); // 24 h
DECLARE_TASK(checkInternet, 60 * 1000);
DECLARE_TASK(logBackup, 60 * 60 * 1000); // 1 h
DECLARE_TASK(mqtt, 30 * 1000);
DECLARE_TASK(talkBack, 5 * 60 * 1000); // 5 min

static const unsigned g_voltageField = 1;
static const unsigned g_currentField = 2;
static const unsigned g_pingField = 7;
static const unsigned g_bootTimeField = 8;

AccumulatorV2 g_pingTime(g_mqttTaskPeriod / g_checkInternetTaskPeriod);
static String g_mqttMessage = "";

static WiFiClient g_wifiClient;
static TalkBack talkBack;

bool g_hasInternet = false;
time_t g_bootTime = 0;
bool g_mqttEnabled = true;
unsigned g_packagesSent = 0;
bool g_ledBlinkEnabled = false;
unsigned g_connectionLossCount = 0;

INA3221 g_ina3221(INA3221_ADDR40_GND);
AccumulatorV2 g_voltage(g_mqttTaskPeriod / g_ioTaskPeriod);
AccumulatorV2 g_current(g_mqttTaskPeriod / g_ioTaskPeriod);

static void
ioTaskHandler()
{
    g_voltage.add(g_ina3221.getVoltage(INA3221_CH1));
    g_current.add(g_ina3221.getCurrent(INA3221_CH1));
}

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
        logger.println("MQTT skipped.");
        logger.println("g_mqttEnabled = " + String(g_mqttEnabled) +
                       " g_hasInternet = " + String(g_hasInternet));
        return;
    }

    mqttAddField(g_voltageField, String(g_voltage.getAverage(), 3));
    mqttAddField(g_currentField, String(g_current.getAverage(), 3));
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
        logger.println("msgQueue is full, discarding messages..");
        msgQueue.pop_front();
    }
    while (msgQueue.size() > 0) {
        // logger.println("Publish [" + String(msgQueue.size()) + "]: " +
        // msgQueue.front());
        bool success = mqttPublish(g_thingSpeakChannelNumber, msgQueue.front());

        if (success) {
            ++g_packagesSent;
            msgQueue.pop_front();
            errors = 0;
        } else {
            logger.println("mqttPublish failed.");
            ++errors;
            if (errors > 3) {
                logger.println("Giving up for now...");
                break;
            }
        }
    }
    digitalWrite(LED_BUILTIN, 0);
}

void
clockUpdateTaskHandler()
{
    logger.println("Syncing clock...");

    if (!g_hasInternet) {
        logger.println("Syncing skipped, no internet connection.");
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
        logger.println("TalkBack failure.");
        return;
    }
    digitalWrite(LED_BUILTIN, 0);

    // ...
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

                    mqttAddStatus("Im back online! Downtime: " +
                                  String(downTime));
                }
            }
            g_pingTime.add(Ping.averageTime());
            g_hasInternet = true;
            return;
        }
    }

    if (g_hasInternet) {
        logger.println("Internet connection lost.");
        g_hasInternet = false;
        connectionLostTime = time(NULL);
        ++g_connectionLossCount;

        mqttAddStatus("Internet connection lost.");
    }
}

static void
ledBlinkTaskHandler()
{
    static bool on = false;
    if (g_ledBlinkEnabled) {
        on = !on;
        digitalWrite(BUILTIN_LED, on);
    } else {
        digitalWrite(BUILTIN_LED, 0);
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
    logger.println("Tasks setup...");

    g_ina3221.begin();
    g_ina3221.reset();
    g_ina3221.setShuntRes(100, 100, 100);

    talkBack.setTalkBackID(g_talkBackID);
    talkBack.setAPIKey(g_talkBackAPIKey);
    talkBack.begin(g_wifiClient);

    logger.println("Waiting for internet connection...");
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
    g_clockUpdateTask.enableDelayed(g_clockUpdateTaskPeriod);
    g_checkInternetTask.enableDelayed(g_checkInternetTaskPeriod);
    g_mqttTask.enableDelayed(g_mqttTaskPeriod);
    g_talkBackTask.enableDelayed(g_talkBackTaskPeriod);
    g_ledBlinkTask.enableDelayed(g_ledBlinkTaskPeriod);
    g_logBackupTask.enableDelayed(g_logBackupTaskPeriod);

    logger.println("Tasks setup done!");
    logger.backup();
}

void
tasksLoop()
{
    g_taskScheduler.execute();
    mqttLoop();
}

void
mqttEnable(bool enable)
{
    if (enable == true) {
        logger.println("MQTT enabled.");
    } else {
        logger.println("MQTT disabled.");
    }

    g_mqttEnabled = enable;
}
