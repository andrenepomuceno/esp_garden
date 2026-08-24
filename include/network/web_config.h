#pragma once

#include <ESPAsyncWebServer.h>

// GET /config.json — the stored document with every credential masked, or the
// verbatim document when ?secrets=1 asks for a restorable backup. ADMIN only.
void
handleConfigGet(AsyncWebServerRequest* request);

// POST /config.json — replaces the whole document, restoring any field that
// still carries the mask from the copy on disk. ADMIN only.
void
handleConfigPost(AsyncWebServerRequest* request);
