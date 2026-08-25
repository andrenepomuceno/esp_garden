#include "network/mqtt.h"
#include "core/config.h"
#include "core/logger.h"
#include "network/thingsboard.h"
#include <PubSubClient.h>
#include "core/filesystem.h"
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
mqttIsConnected()
{
    return mqttClient.connected();
}

int
mqttState()
{
    return mqttClient.state();
}

bool
mqttPublishTopic(const String& topic, const String& message)
{
    return mqttClient.publish(topic.c_str(), message.c_str());
}

// Runs inside mqttClient.loop(). Nothing called from here may publish:
// PubSubClient uses a single buffer for both directions and `payload` points
// into it, so an outbound message would overwrite the packet being handled.
// tbHandleMessage() records intent and tbLoop() does the publishing.
void
mqttSubscriptionCallback(char* topic, byte* payload, unsigned int length)
{
    tbHandleMessage(topic, (const uint8_t*)payload, length);
}

bool
mqttSubscribeTopic(const String& topic)
{
    const bool ok = mqttClient.subscribe(topic.c_str());
    if (!ok) {
        logger.error("MQTT subscribe failed: " + topic);
    } else {
        logger.debug("MQTT subscribed: " + topic);
    }
    return ok;
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

    // Subscriptions and the firmware-attribute request belong to the backend,
    // and both have to be redone on every reconnection — a broker forgets a
    // session the moment it drops.
    tbOnConnect();

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

    // A firmware chunk arrives as one MQTT packet, so the buffer has to hold a
    // whole one. Undersize it and PubSubClient discards every chunk without a
    // word and the download stalls at 0 %.
    const unsigned bufferSize = max((unsigned)2048, tbRequiredBufferSize());
    if (!mqttClient.setBufferSize(bufferSize)) {
        logger.error("MQTT buffer allocation failed (" + String(bufferSize) +
                     " B); firmware updates over MQTT will not work");
    }

    if (!config.mqttUseTLS) {
        // A self-hosted ThingsBoard on 1883 has no certificate to pin, and
        // loading one would only fail confusingly.
        return true;
    }

    File cacert = FILESYSTEM.open(g_mqttCACert, FILE_READ);
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
        // Exponential backoff. Without it a broker that refuses the connection
        // — a wrong token, an expired CA pin — is retried on every single
        // loop() iteration, which buries the reason in hundreds of identical
        // log lines a minute and starves the rest of loop().
        static unsigned long nextAttempt = 0;
        static unsigned long backoff = g_mqttRetryMinMs;

        // Signed difference, not `now < nextAttempt`. millis() wraps at ~49.7
        // days, and if the device is in this branch when it does — broker
        // down, wrong token, stale CA pin, i.e. exactly what the backoff is
        // for — the naive compare stays true for another 49 days and
        // mqttConnect() is never called again. Nothing would surface it:
        // /data.json keeps reporting MQTT enabled and only Packages Sent
        // stops moving, the same silent signature as the three-year stale-CA
        // incident. tasksLoop() and relaysTick() already use this form.
        const unsigned long now = millis();
        if ((long)(now - nextAttempt) < 0) {
            return;
        }

        if (mqttConnect()) {
            backoff = g_mqttRetryMinMs;
        } else {
            nextAttempt = now + backoff;
            backoff = min(backoff * 2, g_mqttRetryMaxMs);
        }
        return;
    }

    mqttClient.loop();
    tbLoop();
}
