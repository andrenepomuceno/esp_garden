#pragma once

#include <ESPAsyncWebServer.h>

// GET /users.json — usernames and roles only. ADMIN only.
void
handleUsersJson(AsyncWebServerRequest* request);

// POST /users — action=upsert|delete. ADMIN only.
void
handleUsersPost(AsyncWebServerRequest* request);
