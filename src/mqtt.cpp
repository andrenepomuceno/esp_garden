#include "network/mqtt.h"
#include "core/config.h"
#include "core/logger.h"
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// Both transports exist statically; only the selected one is ever connected,
// and WiFiClientSecure allocates its handshake buffers on connect rather than
// at construction, so the unused one costs almost nothing.
static WiFiClientSecure secureClient;
static WiFiClient plainClient;
PubSubClient mqttClient(secureClient);

bool
mqttIsThingsBoard()
{
    return config.mqttBackend == "thingsboard";
}

bool
mqttPublishTopic(const String& topic, const String& message)
{
    return mqttClient.publish(topic.c_str(), message.c_str());
}

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
    logger.info("MQTT backend: " + config.mqttBackend + " on " + g_mqttServer +
                ":" + String(g_mqttPort) +
                (config.mqttUseTLS ? " (TLS)" : " (plain)"));

    mqttClient.setClient(config.mqttUseTLS ? (Client&)secureClient
                                           : (Client&)plainClient);
    mqttClient.setServer(g_mqttServer.c_str(), g_mqttPort);
    mqttClient.setCallback(mqttSubscriptionCallback);
    mqttClient.setBufferSize(2048);

    if (!config.mqttUseTLS) {
        // A self-hosted ThingsBoard on 1883 has no certificate to pin, and
        // loading one would only fail confusingly.
        return true;
    }

    File cacert = SPIFFS.open(g_mqttCACert, FILE_READ);
    if (cacert == false) {
        logger.error("Failed to open MQTT CA certificate " + g_mqttCACert);
        return false;
    }
    static String data = cacert.readString();
    cacert.close();
    secureClient.setCACert(data.c_str());

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
