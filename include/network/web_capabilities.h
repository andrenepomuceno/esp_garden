#pragma once

#include <ESPAsyncWebServer.h>

// GET /capabilities.json — what this firmware can be configured to drive:
// the per-kind maxima, the kinds it has drivers for, and which GPIOs are
// usable for analog, output and pull-up inputs.
//
// Exists so /devices.html can offer only pins the firmware would accept,
// without restating the rules in JavaScript where they would drift.
void
handleCapabilitiesJson(AsyncWebServerRequest* request);
