#include "talkback.h"

TalkBack::TalkBack() {}

void TalkBack::begin(Client& client) {
    this->client = &client;
}

void TalkBack::setTalkBackID(const unsigned id) {
    this->id = id;
}

void TalkBack::setAPIKey(const String& key) {
    apiKey = key;
}

bool TalkBack::execute(String& response) {
    return false;
}