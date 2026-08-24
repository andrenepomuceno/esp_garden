#pragma once

#include <ESPAsyncWebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Guards the rendered /data.json string. Created by webSetup(); the writer is
// webUpdateDataCache() on loop(), the reader is the request handler on the
// async_tcp task. webUpdateDataCache() itself is declared in network/web.h.
extern SemaphoreHandle_t g_dataMutex;

void
handleDataJson(AsyncWebServerRequest* request);
