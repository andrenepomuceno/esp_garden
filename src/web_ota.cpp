#include "core/config.h"
#include "core/logger.h"
#include "core/relays.h"
#include "network/thingsboard.h"
#include "core/tasks.h"
#include "network/web_ota.h"
#include <ESPAsyncWebServer.h>
#include <Update.h>

static bool g_otaEnabled = false;

void
handleUpdateEnable(AsyncWebServerRequest* request)
{
    // Both OTA paths drive the same single Update object. Arming a browser
    // upload while the cloud is streaming an image would let whichever wrote
    // second finish over a half-written partition, and Update reports success
    // for it — the corruption only shows at the next boot.
    if (tbFotaInProgress()) {
        logger.warning("[OTA] refused: a firmware download from the broker is "
                       "already running");
        request->send(
          409, "text/plain", "A firmware update from the broker is in progress");
        return;
    }

    // The same rule the cloud FOTA path already follows, and it belongs here
    // for the same reason: this ends in delay(500) + ESP.restart(), and on an
    // ESP32 reset every GPIO floats until relayPinsSafeInit() runs — which on
    // these active-low boards switches the pump back ON for the length of the
    // boot. Documenting the hazard for one of the two OTA paths and not the
    // other left the browser upload as the way to hit it.
    for (unsigned i = 0; i < config.relayCount; ++i) {
        if (relayIsOn(i)) {
            logger.warning("[OTA] refused: " + config.relayName[i] +
                           " is running");
            request->send(409,
                          "text/plain",
                          config.relayName[i] +
                            " is running. Updating reboots the device, and a "
                            "relay is energised across a reset.");
            return;
        }
    }

    g_otaEnabled = true;
    logger.info("[OTA] Enabled OTA");
    request->send(200);
}

void
handleUpdateRequest(AsyncWebServerRequest* request)
{
    if (!g_otaEnabled) {
        request->send(400);
        return;
    }

    // FINISHED, not merely "no error recorded".
    //
    // Update.hasError() is false when Update.begin() was never reached at all,
    // so an upload that died partway used to land here clean and reboot the
    // board on a half-written partition. Every failed attempt therefore
    // restarted the garden, and the restart then killed the connection of the
    // next attempt — which is how one bad upload became a run of five
    // `software (ESP.restart)` boots in the log with the firmware unchanged.
    const bool finished = Update.isFinished() && !Update.hasError();

    // A partial write leaves Update RUNNING, and the next Update.begin() then
    // refuses because one is already in progress — so a single interrupted
    // upload poisons every retry until the board is power-cycled. Clear it.
    if (!finished && Update.isRunning()) {
        Update.abort();
        logger.warning("[OTA] upload did not complete; partial update aborted");
    }

    AsyncWebServerResponse* response = request->beginResponse(
      finished ? 200 : 500, "text/plain", finished ? "OK" : "FAIL");
    response->addHeader("Connection", "close");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);

    // Disarm either way: a flag left set accepts an unsolicited image for as
    // long as the device stays up.
    g_otaEnabled = false;

    if (finished) {
        // NOT delay(500) + ESP.restart(). request->send() only QUEUES the
        // response, and the async_tcp task that would flush it is the one this
        // handler runs on — so blocking it for half a second and then resetting
        // guarantees the browser sees a connection reset instead of "OK".
        // CLAUDE.md records the same trap for /control, where moving the
        // restart after the send was tried and was not enough. requestRestart()
        // reboots from loop() once the response is actually out.
        requestRestart();
    }
}

void
handleUpdateUpload(AsyncWebServerRequest* request,
                   String filename,
                   size_t index,
                   uint8_t* data,
                   size_t len,
                   bool final)
{
    if (!g_otaEnabled) {
        request->send(400);
        return;
    }

    if (!index) {
        logger.info("[OTA] Starting update: " + filename);
        int cmd = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
        if (request->hasParam("MD5", true)) {
            Update.setMD5(request->getParam("MD5", true)->value().c_str());
        }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, cmd)) {
            logger.error(String("[OTA] ") + Update.errorString());
            return request->send(400, "text/plain", "OTA could not begin");
        }
    }

    if (len) {
        if (Update.write(data, len) != len) {
            logger.error(String("[OTA] ") + Update.errorString());
            Update.abort();
            return request->send(400, "text/plain", "OTA could not write");
        }
    }

    if (final) {
        if (!Update.end(true)) {
            logger.error(String("[OTA] ") + Update.errorString());
            Update.abort();
            return request->send(400, "text/plain", "OTA could not end");
        }
        logger.info("[OTA] Complete! " + String(index + len) + " bytes");
    }
}
