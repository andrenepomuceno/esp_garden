#pragma once

#include "BuildConfig.h"

#if USE_TALKBACK

#include <Client.h>

class TalkBack {
public:
    TalkBack();

    void begin(Client& client);
    void setTalkBackID(const unsigned id);
    void setAPIKey(const String& key);
    bool execute(String& response);

private:
    bool executeHelper(String& response);

    Client* client = nullptr;
    unsigned id;
    String apiKey;
};

#endif // USE_TALKBACK
