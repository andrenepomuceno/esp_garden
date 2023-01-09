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
    logger.println("Message arrived [" + String(topic) + "] " +
                   String(payload, length));
}

void
mqttSubscribe(long subChannelID)
{
    String myTopic = "channels/" + String(subChannelID) + "/subscribe";
    mqttClient.subscribe(myTopic.c_str());
}

void
mqttPublish(long pubChannelID, String message)
{
    String topicString = "channels/" + String(pubChannelID) + "/publish";
    mqttClient.publish(topicString.c_str(), message.c_str());
}

void
mqttConnect()
{
    if (!mqttClient.connected()) {
        if (mqttClient.connect(g_mqttClientID.c_str(),
                               g_mqttUser.c_str(),
                               g_mqttPassword.c_str())) {
            logger.println("MQTT to " + g_mqttServer + ":" +
                           String(g_mqttPort) + " successful.");
        } else {
            logger.println("MQTT connection failed, rc = " +
                           String(mqttClient.state()));
            logger.println("Will try again in a few seconds.");
        }
    }
}

void
mqttSetup()
{
    mqttClient.setServer(g_mqttServer.c_str(), g_mqttPort);
    mqttClient.setCallback(mqttSubscriptionCallback);
    mqttClient.setBufferSize(2048);

    File cacert = SPIFFS.open(g_mqttCACert, FILE_READ);
    if (cacert == false) {
        logger.println("Failed to open thingspeak cacert.");
        return;
    }
    static String data = cacert.readString();
    cacert.close();
    client.setCACert(data.c_str());
}

void
mqttLoop()
{
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqttClient.connected()) {
        mqttConnect();
        mqttSubscribe(g_thingSpeakChannelNumber);
    }

    mqttClient.loop();
}
