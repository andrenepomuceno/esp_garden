#pragma once

#include <ESPAsyncWebServer.h>

// Replace ONE file on SPIFFS, instead of rewriting the whole partition.
//
// This exists because `uploadfs` and the filesystem OTA both overwrite
// /config.json with whatever the build directory happened to hold, which is how
// a device loses its Wi-Fi and broker credentials over a one-line change to a
// stylesheet. A per-file upload makes that class of accident impossible for
// everything except the configuration itself, which keeps its own validating
// endpoint and is refused here.
//
// The target path is the multipart FILENAME — the same trick
// handleUpdateUpload uses to choose between firmware and filesystem. An `MD5`
// form field, when present, is checked against what actually landed before the
// file is moved into place.
void
handleFileUploadRequest(AsyncWebServerRequest* request);

void
handleFileUpload(AsyncWebServerRequest* request,
                 String filename,
                 size_t index,
                 uint8_t* data,
                 size_t len,
                 bool final);
