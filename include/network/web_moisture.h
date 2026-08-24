#pragma once

#include <ESPAsyncWebServer.h>

// GET /moisture.json — the inference AND the parameters behind it: per class
// the mean, standard deviation, accumulated weight and prior; per probe the
// Fisher separation, the watering events used, and when a probe reports
// nothing, WHICH gate refused it.
//
// The reasons are part of the contract. A classifier that silently declines is
// indistinguishable from one that is broken, and this repository already has a
// documented case of a confident clustering result that was nonsense.
void
handleMoistureJson(AsyncWebServerRequest* request);
