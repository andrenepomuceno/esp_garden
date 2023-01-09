#include "mqtt.h"
#include "config.h"
#include "logger.h"
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WiFiClientSecure.h>

WiFiClientSecure client;
PubSubClient mqttClient(client);

int status = WL_IDLE_STATUS;
long lastPublishMillis = 0;
int connectionDelay = 1;
int updateInterval = 15;

void
mqttSubscriptionCallback(char* topic, byte* payload, unsigned int length)
{
    //logger.println("MQTT [" + String(topic) + "] " + String(payload, length));
}

bool
mqttSubscribe(long subChannelID)
{
    String myTopic = "channels/" + String(subChannelID) + "/subscribe";
    return mqttClient.subscribe(myTopic.c_str());
}

bool
mqttPublish(long pubChannelID, String message)
{
    String topicString = "channels/" + String(pubChannelID) + "/publish";
    return mqttClient.publish(topicString.c_str(), message.c_str());
}

bool
mqttConnect()
{
    if (mqttClient.connected()) {
        return true;
    }

    if (!mqttClient.connect(
          g_mqttClientID.c_str(), g_mqttUser.c_str(), g_mqttPassword.c_str())) {
        logger.println("MQTT connect failed: " + String(mqttClient.state()));
        return false;
    }

    logger.println("MQTT connected!");

    return true;
}

bool
mqttSetup()
{
    mqttClient.setServer(g_mqttServer.c_str(), g_mqttPort);
    mqttClient.setCallback(mqttSubscriptionCallback);
    mqttClient.setBufferSize(2048);

    File cacert = SPIFFS.open(g_mqttCACert, FILE_READ);
    if (cacert == false) {
        logger.println("Failed to open MQTT CA Certificate.");
        return false;
    }
    static String data = cacert.readString();
    cacert.close();
    client.setCACert(data.c_str());

    mqttLoop();

    return true;
}

void
mqttLoop()
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqttClient.connected()) {
        mqttConnect();
        //mqttSubscribe(g_thingSpeakChannelNumber);
    }

    mqttClient.loop();
}
