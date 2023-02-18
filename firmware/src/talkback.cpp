#include "talkback.h"
#include <Arduino.h>

static const char* g_hostname = "api.thingspeak.com";
static const unsigned int g_port = 80;
static const unsigned int g_timeout = 5000;

TalkBack::TalkBack() {}

void
TalkBack::begin(Client& client)
{
    this->client = &client;
}

void
TalkBack::setTalkBackID(const unsigned id)
{
    this->id = id;
}

void
TalkBack::setAPIKey(const String& key)
{
    apiKey = key;
}

bool
TalkBack::executeHelper(String& response)
{
    // TODO use HTTPS

    String content = "api_key=" + apiKey + "&headers=false";
    String header =
      "POST /talkbacks/" + String(id) + "/commands/execute HTTP/1.1\r\n";
    header += "Host: " + String(g_hostname) + "\r\n";
    header += "Content-Type: application/x-www-form-urlencoded\r\n";
    header += "Connection: close\r\n";
    header += "Content-Length: " + String(content.length()) + "\r\n";
    header += "\r\n";

    client->print(header + content);

    unsigned long timeout = millis() + g_timeout;
    while (client->available() == 0) {
        delay(10);
        if (millis() > timeout) {
            break;
        }
    }

    if (client->available() == 0) {
        return false;
    }

    if (client->find("HTTP/1.1 200 OK") == 0) {
        return false;
    }

    if (client->find("\r\n\r\n") == 0) {
        return false;
    }

    response = client->readString();

    return true;
}

bool
TalkBack::execute(String& response)
{
    if (client->connect(g_hostname, g_port) != 1) {
        return false;
    }

    bool ret = executeHelper(response);

    client->stop();

    return ret;
}
