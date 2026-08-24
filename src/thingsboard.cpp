#include "network/thingsboard.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/fw_version.h"
#include "core/logger.h"
#include "core/tasks.h"
#include "network/mqtt.h"
#include <Arduino_JSON.h>
#include <Update.h>
#include <vector>

// ---------------------------------------------------------------------------
// Protocol constants
// ---------------------------------------------------------------------------

static const char* const TB_TELEMETRY = "v1/devices/me/telemetry";
static const char* const TB_ATTRIBUTES = "v1/devices/me/attributes";
static const char* const TB_ATTR_REQUEST = "v1/devices/me/attributes/request/";
static const char* const TB_ATTR_RESPONSE = "v1/devices/me/attributes/response/";
static const char* const TB_RPC_REQUEST = "v1/devices/me/rpc/request/";
static const char* const TB_RPC_RESPONSE = "v1/devices/me/rpc/response/";
static const char* const TB_FW_REQUEST = "v2/fw/request/";
static const char* const TB_FW_RESPONSE = "v2/fw/response/";

static const char* const TB_FW_SHARED_KEYS =
  "fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm";

// One chunk is one MQTT packet. 4 KB keeps a ~1.1 MB image at ~280 round trips
// while costing 4 KB of permanently allocated PubSubClient buffer — the same
// order as the TLS handshake buffers this board already carries.
static const unsigned TB_CHUNK_SIZE = 4096;

// A stalled download must not sit on the Update object forever: the web OTA
// endpoint shares it, so an abandoned cloud FOTA would lock out the recovery
// path that exists precisely for a bad cloud FOTA.
static const unsigned long TB_CHUNK_TIMEOUT_MS = 30000;
static const unsigned TB_CHUNK_MAX_RETRIES = 5;

// The outbox exists because publishing is forbidden inside the callback (see
// the header). It only ever holds an RPC reply, an fw_state line or a chunk
// request, so the cap is generous.
static const unsigned TB_OUTBOX_MAX = 8;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static std::vector<std::pair<String, String>> g_outbox;

static unsigned g_attrRequestId = 0;
static unsigned g_fwRequestId = 0;

// Announced by the broker but not started yet — a download is deferred while a
// relay is energised (see tbRelaysIdle).
static bool g_updateAnnounced = false;
static String g_offeredVersion;
static String g_offeredChecksum;
static size_t g_offeredSize = 0;

static bool g_fotaActive = false;
static unsigned g_currentChunk = 0;
static unsigned g_totalChunks = 0;
static size_t g_receivedBytes = 0;
static unsigned long g_lastChunkMs = 0;
static unsigned g_chunkRetries = 0;
static bool g_chunkRequestPending = false;
static String g_fotaError;

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

static void
tbQueue(const String& topic, const String& payload)
{
    if (g_outbox.size() >= TB_OUTBOX_MAX) {
        // Loudly: a dropped RPC reply looks to the operator like a device that
        // ignored the command, and a dropped chunk request stalls the download
        // until the retry timer fires.
        logger.error("ThingsBoard outbox full, dropping " + topic);
        return;
    }
    g_outbox.push_back(std::make_pair(topic, payload));
}

static void
tbRequestFirmwareAttributes()
{
    ++g_attrRequestId;
    JSONVar keys;
    keys["sharedKeys"] = TB_FW_SHARED_KEYS;
    tbQueue(String(TB_ATTR_REQUEST) + String(g_attrRequestId),
            JSON.stringify(keys));
}

static void
tbPublishState(const String& state)
{
    JSONVar telemetry;
    telemetry["fw_state"] = state;
    if (g_fotaError.length() > 0) {
        telemetry["fw_error"] = g_fotaError;
    }
    tbQueue(TB_TELEMETRY, JSON.stringify(telemetry));
}

// ---------------------------------------------------------------------------
// Firmware over MQTT
// ---------------------------------------------------------------------------

unsigned
tbRequiredBufferSize()
{
    if (!mqttIsThingsBoard() || !config.mqttFwUpdate) {
        return 0;
    }

    // MQTT fixed header plus the v2/fw/response/<id>/chunk/<n> topic. 128 B is
    // several times what either needs.
    return TB_CHUNK_SIZE + 128;
}

bool
tbFotaInProgress()
{
    return g_fotaActive;
}

String
tbFotaStatus()
{
    if (g_fotaActive) {
        const unsigned percent =
          (g_offeredSize > 0)
            ? (unsigned)((uint64_t)g_receivedBytes * 100 / g_offeredSize)
            : 0;
        return "downloading " + g_offeredVersion + " " + String(percent) +
               "% (" + String(g_currentChunk) + "/" + String(g_totalChunks) +
               ")";
    }

    if (g_updateAnnounced) {
        return "waiting for relays to idle before flashing " + g_offeredVersion;
    }

    if (g_fotaError.length() > 0) {
        return "last attempt failed: " + g_fotaError;
    }

    return "";
}

static void
tbFotaFail(const String& reason)
{
    logger.error("[TB-FOTA] " + reason);
    g_fotaError = reason;

    if (Update.isRunning()) {
        Update.abort();
    }

    g_fotaActive = false;
    g_updateAnnounced = false;
    g_chunkRequestPending = false;
    tbPublishState("FAILED");
}

static void
tbRequestChunk(unsigned index)
{
    // The payload of a chunk request is the chunk size the device wants, as a
    // decimal string. ThingsBoard answers with at most that many bytes.
    tbQueue(String(TB_FW_REQUEST) + String(g_fwRequestId) + "/chunk/" +
              String(index),
            String(TB_CHUNK_SIZE));
    g_lastChunkMs = millis();
}

// Decides whether the announced image is one this device should flash.
static void
tbCheckFirmwareAttributes(JSONVar& attributes)
{
    if (!config.mqttFwUpdate) {
        return;
    }

    if (!attributes.hasOwnProperty("fw_version") ||
        !attributes.hasOwnProperty("fw_title") ||
        !attributes.hasOwnProperty("fw_size") ||
        !attributes.hasOwnProperty("fw_checksum")) {
        return; // a shared-attribute push that is not about firmware
    }

    // Named locals: Arduino_JSON's operator[] returns by value, so casting a
    // subscript straight to const char* reads a buffer the temporary freed.
    JSONVar versionVar = attributes["fw_version"];
    JSONVar titleVar = attributes["fw_title"];
    JSONVar checksumVar = attributes["fw_checksum"];
    JSONVar sizeVar = attributes["fw_size"];

    const String version = (const char*)versionVar;
    const String title = (const char*)titleVar;
    const String checksum = (const char*)checksumVar;
    const long size = (long)sizeVar;

    String algorithm = "MD5";
    if (attributes.hasOwnProperty("fw_checksum_algorithm")) {
        JSONVar algorithmVar = attributes["fw_checksum_algorithm"];
        algorithm = (const char*)algorithmVar;
    }

    // The title is the safety catch. One ThingsBoard tenant holds every device
    // an operator owns, and assigning the wrong package to a device profile is
    // one wrong click — a firmware image built for another board is a brick
    // that needs USB to recover.
    if (title != config.mqttFwTitle) {
        logger.warning("[TB-FOTA] ignoring firmware titled '" + title +
                       "', this device accepts '" + config.mqttFwTitle + "'");
        return;
    }

    if (algorithm != "MD5") {
        logger.warning("[TB-FOTA] unsupported checksum algorithm " + algorithm +
                       "; Update only verifies MD5");
        return;
    }

    if (size <= 0) {
        logger.warning("[TB-FOTA] refusing firmware with size " + String(size));
        return;
    }

    if (!fwVersionDiffers(version.c_str(), FW_VERSION)) {
        return; // already running it — the usual case on every reconnect
    }

    if (g_fotaActive) {
        if (version != g_offeredVersion) {
            logger.warning("[TB-FOTA] " + version +
                           " announced while downloading " + g_offeredVersion +
                           "; finishing the one in flight first");
        }
        return;
    }

    g_offeredVersion = version;
    g_offeredChecksum = checksum;
    g_offeredSize = (size_t)size;
    g_updateAnnounced = true;
    g_fotaError = "";

    logger.info("[TB-FOTA] update available: " + version + " (" + String(size) +
                " bytes, running " + String(FW_VERSION) + ")");
}

// A cloud FOTA ends in a reboot, and on an ESP32 reset every GPIO floats for
// the few hundred ms before relayPinsSafeInit() runs. These relay boards are
// active-low, so a floating pin reads as ENERGISE — rebooting while a pump is
// running turns that pump back on for the length of the boot. Waiting for an
// idle relay costs at most the 30 s a relay is allowed to be on for.
static bool
tbRelaysIdle()
{
    for (unsigned i = 0; i < config.relayCount; ++i) {
        if (relayIsOn(i)) {
            return false;
        }
    }
    return true;
}

static void
tbStartFirmwareUpdate()
{
    logger.info("[TB-FOTA] starting download of " + g_offeredVersion + " (" +
                String(g_offeredSize) + " bytes)");

    if (!Update.begin(g_offeredSize, U_FLASH)) {
        // Almost always "Not Enough Space": the image is bigger than the OTA
        // slot in partitions/esp_garden_4mb.csv.
        tbFotaFail(String(Update.errorString()));
        return;
    }

    if (!Update.setMD5(g_offeredChecksum.c_str())) {
        tbFotaFail("bad fw_checksum '" + g_offeredChecksum + "'");
        return;
    }

    g_fotaActive = true;
    g_updateAnnounced = false;
    g_receivedBytes = 0;
    g_currentChunk = 0;
    g_chunkRetries = 0;
    g_totalChunks = (g_offeredSize + TB_CHUNK_SIZE - 1) / TB_CHUNK_SIZE;
    ++g_fwRequestId;

    tbPublishState("DOWNLOADING");
    tbRequestChunk(0);
}

static void
tbFinishFirmwareUpdate()
{
    tbPublishState("DOWNLOADED");

    // end(true) is where the MD5 set above is verified, so a corrupted download
    // fails here rather than at the next boot.
    if (!Update.end(true)) {
        tbFotaFail(String(Update.errorString()));
        return;
    }

    logger.info("[TB-FOTA] " + g_offeredVersion +
                " written and verified; restarting");

    g_fotaActive = false;
    g_fotaError = "";
    tbPublishState("VERIFIED");
    tbPublishState("UPDATING");

    // Draining the outbox needs a few more mqttLoop() calls; requestRestart()
    // reboots from loop() half a second later, which is many iterations. The
    // UPDATED state is reported by the client attributes published on the next
    // connection, which is also how ThingsBoard learns the new version.
    requestRestart();
}

static void
tbHandleChunk(const String& topic, const uint8_t* payload, unsigned length)
{
    if (!g_fotaActive) {
        return; // a late chunk from an aborted attempt
    }

    // v2/fw/response/<requestId>/chunk/<index>
    const String expected = String(TB_FW_RESPONSE) + String(g_fwRequestId) +
                            "/chunk/" + String(g_currentChunk);
    if (topic != expected) {
        logger.warning("[TB-FOTA] out-of-order chunk on " + topic +
                       ", expecting " + expected);
        return;
    }

    if (length == 0) {
        // ThingsBoard answers an unavailable image with an empty body. Retrying
        // would loop forever against a device that has no firmware assigned.
        tbFotaFail("broker returned an empty chunk");
        return;
    }

    if (g_receivedBytes + length > g_offeredSize) {
        tbFotaFail("received more than fw_size (" +
                   String(g_receivedBytes + length) + " > " +
                   String(g_offeredSize) + ")");
        return;
    }

    if (Update.write((uint8_t*)payload, length) != length) {
        tbFotaFail(String(Update.errorString()));
        return;
    }

    g_receivedBytes += length;
    ++g_currentChunk;
    g_chunkRetries = 0;
    g_lastChunkMs = millis();

    if (g_receivedBytes < g_offeredSize) {
        g_chunkRequestPending = true; // sent from tbLoop, never from here
        return;
    }

    tbFinishFirmwareUpdate();
}

// ---------------------------------------------------------------------------
// RPC
// ---------------------------------------------------------------------------

static void
tbRpcReply(const String& requestId, JSONVar& response)
{
    tbQueue(String(TB_RPC_RESPONSE) + requestId, JSON.stringify(response));
}

static void
tbRpcError(const String& requestId, const String& message)
{
    logger.warning("[TB-RPC] " + message);
    JSONVar response;
    response["ok"] = false;
    response["error"] = message;
    tbRpcReply(requestId, response);
}

// Relay index from params. "relay" and "index" are both accepted because the
// telemetry keys are relay1..relay4 while the addressing contract everywhere
// else in this firmware is the 0-based index.
static String
tbRelayRangeHint(const char* method)
{
    if (config.relayCount == 0) {
        return String(method) + ": this device has no relays configured";
    }
    return String(method) + " needs \"relay\": 0.." +
           String(config.relayCount - 1);
}

static bool
tbRpcRelayIndex(JSONVar& params, unsigned& out)
{
    const char* keys[] = { "relay", "index" };

    for (unsigned k = 0; k < 2; ++k) {
        if (!params.hasOwnProperty(keys[k])) {
            continue;
        }
        JSONVar var = params[keys[k]];
        const int index = (int)var;
        if (index < 0 || index >= (int)config.relayCount) {
            return false;
        }
        out = (unsigned)index;
        return true;
    }

    return false;
}

// Duration in ms, from either "durationMs" or the friendlier "seconds".
static unsigned
tbRpcDuration(JSONVar& params, unsigned fallback)
{
    if (params.hasOwnProperty("durationMs")) {
        JSONVar var = params["durationMs"];
        const long value = (long)var;
        return (value > 0) ? (unsigned)value : 0;
    }

    if (params.hasOwnProperty("seconds")) {
        JSONVar var = params["seconds"];
        const double value = (double)var;
        return (value > 0) ? (unsigned)(value * 1000) : 0;
    }

    return fallback;
}

// Writes the relay array through the parent rather than taking a reference to
// it: Arduino_JSON's operator[] returns BY VALUE, so a nested object built on
// a returned JSONVar is filled in and then thrown away.
static void
tbAppendRelays(JSONVar& parent, const char* key)
{
    for (unsigned i = 0; i < config.relayCount; ++i) {
        parent[key][i]["index"] = (int)i;
        parent[key][i]["name"] = config.relayName[i];
        parent[key][i]["on"] = relayIsOn(i);
        parent[key][i]["remaining"] = (int)relayRemaining(i);
    }
}

static void
tbHandleRpc(const String& requestId, const String& body)
{
    if (!config.mqttRpc) {
        tbRpcError(requestId, "remote commands are disabled on this device");
        return;
    }

    JSONVar request = JSON.parse(body);
    if (JSON.typeof(request) != "object" || !request.hasOwnProperty("method")) {
        tbRpcError(requestId, "malformed RPC body");
        return;
    }

    JSONVar methodVar = request["method"];
    const String method = (const char*)methodVar;
    JSONVar params = request["params"];

    logger.info("[TB-RPC] " + method);

    JSONVar response;

    if (method == "getStatus") {
        response["ok"] = true;
        response["firmware"] = FW_VERSION;
        response["hostname"] = config.hostname;
        response["internet"] = g_hasInternet;
        response["mqtt"] = g_mqttEnabled;
        response["wateringCycles"] = (int)g_wateringCycles;
        response["packagesSent"] = (int)g_packagesSent;
        response["uptimeSec"] = (int)(millis() / 1000);
        tbAppendRelays(response, "relays");
    } else if (method == "getRelays") {
        response["ok"] = true;
        tbAppendRelays(response, "relays");
    } else if (method == "startRelay" || method == "startWatering") {
        // startWatering is relay 0 on every board, which is the contract
        // TalkBack and the legacy /control parameter already rely on.
        unsigned index = 0;
        if (method == "startRelay" && !tbRpcRelayIndex(params, index)) {
            tbRpcError(requestId, tbRelayRangeHint("startRelay"));
            return;
        }

        const unsigned duration = tbRpcDuration(params, g_wateringDefaultTime);
        if (duration == 0) {
            tbRpcError(requestId, "duration must be positive");
            return;
        }

        // startRelay applies the same ceiling and the same already-running
        // guard as the web UI: the cloud gets no privileged path to the pumps.
        if (!startRelay(index, duration)) {
            tbRpcError(requestId,
                       config.relayName[index] +
                         " not started (already running, or duration outside "
                         "1.." +
                         String(g_relayMaxTime) + " ms)");
            return;
        }

        response["ok"] = true;
        response["relay"] = (int)index;
        response["durationMs"] = (int)duration;
    } else if (method == "stopRelay") {
        unsigned index = 0;
        if (!tbRpcRelayIndex(params, index)) {
            tbRpcError(requestId, tbRelayRangeHint("stopRelay"));
            return;
        }
        response["ok"] = true;
        response["relay"] = (int)index;
        response["wasRunning"] = stopRelay(index);
    } else if (method == "getFirmware") {
        response["ok"] = true;
        response["version"] = FW_VERSION;
        response["title"] = config.mqttFwTitle;
        response["updatesEnabled"] = config.mqttFwUpdate;
        const String state = tbFotaStatus();
        response["state"] = (state.length() > 0) ? state : String("idle");
    } else if (method == "checkFirmware") {
        // Re-asks for the shared attributes. Useful right after assigning a
        // package in ThingsBoard, instead of waiting for the push.
        tbRequestFirmwareAttributes();
        response["ok"] = true;
    } else if (method == "restart") {
        response["ok"] = true;
        requestRestart();
    } else {
        tbRpcError(requestId, "unknown method '" + method + "'");
        return;
    }

    tbRpcReply(requestId, response);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void
tbOnConnect()
{
    if (!mqttIsThingsBoard()) {
        return;
    }

    // Client attributes. current_fw_title / current_fw_version are what
    // ThingsBoard reads to decide a device finished an update, so publishing
    // them on every connect is also how it learns the version after a reboot.
    JSONVar attributes;
    attributes["deviceId"] = String(config.deviceId, 16);
    attributes["current_fw_title"] = config.mqttFwTitle;
    attributes["current_fw_version"] = FW_VERSION;
    attributes["hostname"] = config.hostname;
    tbQueue(TB_ATTRIBUTES, JSON.stringify(attributes));

    if (config.mqttRpc) {
        mqttSubscribeTopic(String(TB_RPC_REQUEST) + "+");
    }

    if (!config.mqttFwUpdate) {
        logger.info("[TB-FOTA] disabled by config (mqtt.fwUpdate)");
        return;
    }

    mqttSubscribeTopic(TB_ATTRIBUTES);
    mqttSubscribeTopic(String(TB_ATTR_RESPONSE) + "+");
    mqttSubscribeTopic(String(TB_FW_RESPONSE) + "+");

    // A download interrupted by the disconnection resumes from the chunk it
    // was waiting on rather than starting over.
    if (g_fotaActive) {
        logger.info("[TB-FOTA] reconnected mid-download, resuming at chunk " +
                    String(g_currentChunk));
        g_chunkRequestPending = true;
        return;
    }

    tbRequestFirmwareAttributes();
}

void
tbHandleMessage(const char* topic, const uint8_t* payload, unsigned length)
{
    if (!mqttIsThingsBoard()) {
        return;
    }

    const String topicStr(topic);

    // Firmware chunks first: they are the hot path and they are binary, so they
    // must never reach a JSON parser.
    if (topicStr.startsWith(TB_FW_RESPONSE)) {
        tbHandleChunk(topicStr, payload, length);
        return;
    }

    if (topicStr.startsWith(TB_RPC_REQUEST)) {
        const String requestId = topicStr.substring(strlen(TB_RPC_REQUEST));
        tbHandleRpc(requestId, String((const char*)payload, length));
        return;
    }

    if (topicStr.startsWith(TB_ATTR_RESPONSE)) {
        JSONVar response = JSON.parse(String((const char*)payload, length));
        if (!response.hasOwnProperty("shared")) {
            return;
        }
        JSONVar shared = response["shared"];
        tbCheckFirmwareAttributes(shared);
        return;
    }

    if (topicStr == TB_ATTRIBUTES) {
        // A live shared-attribute push: same keys, no "shared" wrapper.
        JSONVar attributes = JSON.parse(String((const char*)payload, length));
        tbCheckFirmwareAttributes(attributes);
        return;
    }

    logger.debug("ThingsBoard: unhandled topic " + topicStr);
}

void
tbLoop()
{
    if (!mqttIsThingsBoard()) {
        return;
    }

    // Drain the outbox. A failed publish keeps its slot and retries on the next
    // loop rather than being dropped — this is where an RPC reply and the next
    // firmware chunk request live.
    while (!g_outbox.empty()) {
        if (!mqttPublishTopic(g_outbox.front().first,
                              g_outbox.front().second)) {
            break;
        }
        g_outbox.erase(g_outbox.begin());
    }

    if (g_updateAnnounced && !g_fotaActive) {
        if (Update.isRunning()) {
            return; // a browser OTA is mid-upload; it owns Update
        }
        if (!tbRelaysIdle()) {
            return; // see tbRelaysIdle for why the reboot has to wait
        }
        tbStartFirmwareUpdate();
        return;
    }

    if (!g_fotaActive) {
        return;
    }

    if (g_chunkRequestPending) {
        g_chunkRequestPending = false;
        tbRequestChunk(g_currentChunk);
        return;
    }

    if (millis() - g_lastChunkMs > TB_CHUNK_TIMEOUT_MS) {
        if (++g_chunkRetries > TB_CHUNK_MAX_RETRIES) {
            tbFotaFail("no answer for chunk " + String(g_currentChunk) +
                       " after " + String(TB_CHUNK_MAX_RETRIES) + " retries");
            return;
        }
        logger.warning("[TB-FOTA] chunk " + String(g_currentChunk) +
                       " timed out, retry " + String(g_chunkRetries));
        tbRequestChunk(g_currentChunk);
    }
}
