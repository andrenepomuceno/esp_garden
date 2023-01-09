#include "mqtt.h"
#include "logger.h"
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include "config.h"
#include <SPIFFS.h>

// https://www.mathworks.com/help/thingspeak/use-arduino-client-to-publish-to-a-channel.html

#define MQTT_PORT 8883
const char* server = "mqtt3.thingspeak.com";

WiFiClientSecure client;

int status = WL_IDLE_STATUS;
long lastPublishMillis = 0;
int connectionDelay = 1;
int updateInterval = 15;
PubSubClient mqttClient(client);

// Function to handle messages from MQTT subscription.
void
mqttSubscriptionCallback(char* topic, byte* payload, unsigned int length)
{
    // Print the details of the message that was received to the serial monitor.
    logger.println("Message arrived [" + String(topic) + "] " + String(payload, length));
}

// Subscribe to ThingSpeak channel for updates.
void
mqttSubscribe(long subChannelID)
{
    String myTopic = "channels/" + String(subChannelID) + "/subscribe";
    mqttClient.subscribe(myTopic.c_str());
}

// Publish messages to a ThingSpeak channel.
void
mqttPublish(long pubChannelID, String message)
{
    String topicString = "channels/" + String(pubChannelID) + "/publish";
    mqttClient.publish(topicString.c_str(), message.c_str());
}

// Connect to MQTT server.
void
mqttConnect()
{
    // Loop until connected.
    while (!mqttClient.connected()) {
        // Connect to the MQTT broker.
        if (mqttClient.connect(g_mqttClientID.c_str(), g_mqttUser.c_str(), g_mqttPassword.c_str())) {
            logger.println("MQTT to " + String(server) + ":" + String(MQTT_PORT) + " successful.");
        } else {
            logger.println("MQTT connection failed, rc = " + String(mqttClient.state()));
            // See https://pubsubclient.knolleary.net/api.html#state for the
            // failure code explanation.
            logger.println("Will try again in a few seconds.");
            delay(connectionDelay * 1000);
        }
    }
}

void
mqttSetup()
{
    // Configure the MQTT client
    mqttClient.setServer(server, MQTT_PORT);
    // Set the MQTT message handler function.
    mqttClient.setCallback(mqttSubscriptionCallback);
    // Set the buffer to handle the returned JSON. NOTE: A buffer overflow of
    // the message buffer will result in your callback not being invoked.
    mqttClient.setBufferSize(2048);
    // Use secure MQTT connections if defined.
    File cacert = SPIFFS.open("/thingspeak.crt", FILE_READ);
    if (cacert == false) {
        logger.println("Failed to open thingspeak cacert.");
        return;
    }
    String data = cacert.readString();
    cacert.close();
    client.setCACert(data.c_str());
}

void
mqttLoop()
{
    // Reconnect to WiFi if it gets disconnected.
    if (WiFi.status() != WL_CONNECTED) {
        //connectWifi();
        return;
    }

    // Connect if MQTT client is not connected and resubscribe to channel
    // updates.
    if (!mqttClient.connected()) {
        mqttConnect();
        mqttSubscribe(g_thingSpeakChannelNumber);
    }

    // Call the loop to maintain connection to the server.
    mqttClient.loop();

    // Update ThingSpeak channel periodically. The update results in the message
    // to the subscriber.
    /*if (abs(millis() - lastPublishMillis) > updateInterval * 1000) {
        mqttPublish(channelID, (String("field1=") + String(WiFi.RSSI())));
        lastPublishMillis = millis();
    }*/
}