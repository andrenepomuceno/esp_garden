#pragma once

#include <ESPAsyncWebServer.h>

// Browser OTA. One /updateEnable buys exactly one upload: handleUpdateRequest
// clears the arm flag again.
void
handleUpdateEnable(AsyncWebServerRequest* request);

void
handleUpdateRequest(AsyncWebServerRequest* request);

void
handleUpdateUpload(AsyncWebServerRequest* request,
                   String filename,
                   size_t index,
                   uint8_t* data,
                   size_t len,
                   bool final);
