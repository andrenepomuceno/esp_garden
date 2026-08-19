#include "SPIFFS.h"
#include "BuildConfig.h"
#include "core/accumulator_v2.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/role.h"
#include "core/tasks.h"
#include "core/user_store.h"
#include "network/custom_login.h"
#include <Arduino_JSON.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
// 3.7.x no longer pulls WiFi.h in transitively the way the old fork did.
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>

static AsyncWebServer g_webServer(80);

bool g_wifiConnected = false;
bool g_hasNetwork = false;

// Rendered by webUpdateDataCache() from loop() and served verbatim by the
// request handler, which runs on the async_tcp task. The mutex covers the
// String itself; the sensor accumulators are only ever touched by the writer.
static String g_dataJson = "{}";
static SemaphoreHandle_t g_dataMutex = nullptr;

static unsigned
getSignalStrength()
{
    static AccumulatorV2 rssiAcc(60);

    auto rssi = WiFi.RSSI();
    unsigned val = 0;
    if (rssi <= -100) {
        val = 0;
    } else if (rssi >= -50) {
        val = 100;
    } else {
        val = 2 * (rssi + 100);
    }
    rssiAcc.add(val);

    return rssiAcc.getAverage();
}

static void
addAccumulator(JSONVar& inputs, const char* name, AccumulatorV2& acc)
{
    JSONVar entry;
    entry["val"] = String(acc.getLast());
    entry["avg"] = String(acc.getAverage());
    entry["var"] = String(acc.variance);
    inputs[name] = entry;
}

void
webUpdateDataCache()
{
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    time_t now = mktime(&timeinfo);
    int uptime = now - g_bootTime;
    int minutes = uptime / 60;
    int hours = minutes / 60;
    int days = hours / 24;
    char buffer[32];

    JSONVar statusJson;
    statusJson["Hostname"] = g_hostname;
    statusJson["Firmware"] = FW_VERSION;
    strftime(buffer, sizeof(buffer), "%F %T", &timeinfo);
    statusJson["Date/Time"] = String(buffer);
    if (g_bootTime > g_safeTimestamp) {
        snprintf(buffer,
                 sizeof(buffer),
                 "%dd %dh %dm %ds",
                 days,
                 hours % 24,
                 minutes % 60,
                 uptime % 60);
        statusJson["Uptime"] = String(buffer);
    }
#ifdef HAS_DHT_SENSOR
    if (g_dhtTotalReads > 0) {
        statusJson["DHT Error Rate"] =
          String((float)g_dhtReadErrors / (float)g_dhtTotalReads * 100, 2);
    }
#endif
    statusJson["Internet"] = String((g_hasInternet) ? "online" : "offline");
    statusJson["Signal Strength"] = String(getSignalStrength()) + "%";
    statusJson["Ping"] = String(g_pingTime.getAverage()) + "ms";
    statusJson["Connection Loss Count"] = String(g_connectionLossCount);
    statusJson["MQTT"] = String((g_mqttEnabled) ? "enabled" : "disabled");
    statusJson["Packages Sent"] = String(g_packagesSent);
    statusJson["Watering Cycles"] = String(g_wateringCycles);

    JSONVar inputsJson;
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        // A single probe keeps the historical label so existing dashboards and
        // the simulator do not have to special-case one device.
        String name = (MOISTURE_SENSOR_COUNT == 1)
                        ? String("Soil Moisture")
                        : ("Soil Moisture " + String(i + 1));
        addAccumulator(inputsJson, name.c_str(), g_soilMoisture[i]);

        // Empty unless the probe has been calibrated against air and water.
        const String state = moistureState(i);
        if (state.length() > 0) {
            inputsJson[name.c_str()]["state"] = state;
        }
    }
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    addAccumulator(inputsJson, "Luminosity", g_luminosity);
#endif

#ifdef HAS_DHT_SENSOR
    addAccumulator(inputsJson, "Temperature", g_temperature);
    addAccumulator(inputsJson, "Air Humidity", g_airHumidity);
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    addAccumulator(inputsJson, "Water Level", g_waterLevel);
#endif

    JSONVar outputsJson;
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        outputsJson[config.relayName[i].c_str()] =
          String(relayIsOn(i) ? 1 : 0);
    }

    JSONVar relaysJson;
    for (unsigned i = 0; i < RELAY_COUNT; ++i) {
        JSONVar relay;
        relay["index"] = (int)i;
        relay["name"] = config.relayName[i];
        relay["on"] = relayIsOn(i) ? 1 : 0;
        relay["remaining"] = (double)relayRemaining(i);
        relaysJson[i] = relay;
    }

    JSONVar responseJson;
    responseJson["Status"] = statusJson;
    responseJson["Inputs"] = inputsJson;
    responseJson["Outputs"] = outputsJson;
    responseJson["Relays"] = relaysJson;
    responseJson["Channel"] = String(g_thingSpeakChannelNumber);

    String rendered = JSON.stringify(responseJson);

    if (g_dataMutex == nullptr) {
        g_dataJson = rendered;
        return;
    }

    if (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE) {
        g_dataJson = rendered;
        xSemaphoreGive(g_dataMutex);
    }
}

void
handleDataJson(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    String payload;
    if ((g_dataMutex != nullptr) &&
        (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE)) {
        payload = g_dataJson;
        xSemaphoreGive(g_dataMutex);
    } else {
        payload = g_dataJson;
    }

    request->send(200, "application/json", payload);

    digitalWrite(LED_BUILTIN, 0);
}

void
handleControl(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    bool restartRequested = false;

    // relay + relayTime address any relay; watering / wateringTime are the
    // legacy spelling for relay 0 and stay supported.
    if (request->hasParam("relay", true)) {
        const unsigned index =
          request->getParam("relay", true)->value().toInt();
        unsigned duration = g_wateringDefaultTime;
        if (request->hasParam("relayTime", true)) {
            duration = request->getParam("relayTime", true)->value().toInt();
        }
        startRelay(index, duration);
    }

    for (int i = 0; i < request->params(); ++i) {
        const AsyncWebParameter* param = request->getParam(i);
        if ((param->name() == "watering") && (param->value() == "enable")) {
            startWatering();
        } else if (param->name() == "wateringTime") {
            startWatering(param->value().toInt());
        } else if (param->name() == "mqtt") {
            if (param->value() == "enable") {
                mqttEnable(true);
            } else if (param->value() == "disable") {
                mqttEnable(false);
            }
        } else if ((param->name() == "reset") && (param->value() == "1")) {
            restartRequested = true;
        }
    }

    request->send(200);

    digitalWrite(LED_BUILTIN, 0);

    // Handed to loop() rather than done here: request->send() only queues the
    // response, and rebooting (or blocking) on the async_tcp task kills the
    // connection before it flushes, so the caller could never tell a reboot
    // from a failure.
    if (restartRequested) {
        logger.warning("Restart requested over /control.");
        requestRestart();
    }
}

void
handleLogs(AsyncWebServerRequest* request)
{
    digitalWrite(LED_BUILTIN, 1);

    String output = logger.read();
    request->send(200, "text/plain", output);

    digitalWrite(LED_BUILTIN, 0);
}

static bool g_otaEnabled = false;

static void
handleUpdateEnable(AsyncWebServerRequest* request)
{
    g_otaEnabled = true;
    logger.info("[OTA] Enabled OTA");
    request->send(200);
}

static void
handleUpdateRequest(AsyncWebServerRequest* request)
{
    if (!g_otaEnabled) {
        request->send(400);
        return;
    }

    bool error = Update.hasError();
    int code = error ? 500 : 200;
    const char* content = error ? "FAIL" : "OK";
    AsyncWebServerResponse* response =
      request->beginResponse(code, "text/plain", content);
    response->addHeader("Connection", "close");
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);

    // Disarm either way: a flag left set accepts an unsolicited image for as
    // long as the device stays up.
    g_otaEnabled = false;

    if (!error) {
        delay(500);
        ESP.restart();
    }
}

static void
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
            return request->send(400, "text/plain", "OTA could not write");
        }
    }

    if (final) {
        if (!Update.end(true)) {
            logger.error(String("[OTA] ") + Update.errorString());
            return request->send(400, "text/plain", "OTA could not end");
        }
        logger.info("[OTA] Complete!");
    }
}

// Credentials are masked on the way out and restored on the way back in, so a
// plaintext secret never leaves the device — unlike fullbot, which serves the
// raw /config.json to any authenticated session.
static const char* const g_secretPaths[][2] = {
    { "wifi", "password" },     { "ota", "password" },
    { "thingSpeak", "apiKey" }, { "talkBack", "apiKey" },
    { "mqtt", "password" },
};
static const size_t g_secretCount =
  sizeof(g_secretPaths) / sizeof(g_secretPaths[0]);

static void
handleConfigGet(AsyncWebServerRequest* request)
{
    String raw = config.readFile();
    if (raw.isEmpty()) {
        request->send(500, "text/plain", "config unreadable");
        return;
    }

    JSONVar doc = JSON.parse(raw);
    if (JSON.typeof(doc) == "undefined") {
        request->send(500, "text/plain", "config unparseable");
        return;
    }

    for (size_t i = 0; i < g_secretCount; ++i) {
        const char* section = g_secretPaths[i][0];
        const char* key = g_secretPaths[i][1];
        if (JSON.typeof(doc[section]) != "object") {
            continue;
        }
        if (doc[section].hasOwnProperty(key)) {
            doc[section][key] = g_configSecretMask;
        }
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(doc));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static void
handleConfigPost(AsyncWebServerRequest* request)
{
    if (!request->hasParam("config", true)) {
        request->send(400, "text/plain", "missing 'config' parameter");
        return;
    }

    JSONVar incoming = JSON.parse(request->getParam("config", true)->value());
    if (JSON.typeof(incoming) != "object") {
        request->send(400, "text/plain", "config is not a JSON object");
        return;
    }

    // Refuse a document addressed at a different device. Without this a config
    // pasted from another garden would be written, and loadFile() would then
    // reject it on the next boot — leaving the device on compiled defaults it
    // cannot connect with, and unreachable to fix.
    String id = (const char*)incoming["id"];
    char* endPtr;
    if (strtol(id.c_str(), &endPtr, 16) != (long)config.deviceId) {
        request->send(400, "text/plain", "config id does not match this device");
        return;
    }

    JSONVar stored = JSON.parse(config.readFile());
    const bool haveStored = (JSON.typeof(stored) == "object");

    String newOtaPassword;
    for (size_t i = 0; i < g_secretCount; ++i) {
        const char* section = g_secretPaths[i][0];
        const char* key = g_secretPaths[i][1];

        // Every JSONVar is bound to a named local before being read.
        // Arduino_JSON's operator[] returns BY VALUE, so casting
        // `incoming[section][key]` straight to const char* reads a buffer that
        // the temporary already freed and yields an empty String. That made
        // every mask comparison fail silently and wrote the masks over the real
        // credentials — verified against a live device.
        JSONVar incomingSection = incoming[section];
        if (JSON.typeof(incomingSection) != "object" ||
            !incomingSection.hasOwnProperty(key)) {
            continue;
        }

        JSONVar incomingValue = incomingSection[key];
        const String value = (const char*)incomingValue;

        if (value != g_configSecretMask) {
            if (strcmp(section, "ota") == 0 && strcmp(key, "password") == 0) {
                newOtaPassword = value;
            }
            continue;
        }

        if (!haveStored) {
            continue;
        }
        JSONVar storedSection = stored[section];
        if (JSON.typeof(storedSection) != "object" ||
            !storedSection.hasOwnProperty(key)) {
            continue;
        }

        JSONVar storedValue = storedSection[key];
        const String restored = (const char*)storedValue;
        if (restored.isEmpty()) {
            continue;
        }

        // Assign the C string, never the JSONVar: move-assign from an rvalue
        // JSONVar is broken in this library and produces a null child.
        incoming[section][key] = restored.c_str();
    }

    // Refuse to persist a document that still carries a mask. Writing one
    // replaces a real credential with eight asterisks, and the damage only
    // surfaces at the next boot — as an unreachable device.
    for (size_t i = 0; i < g_secretCount; ++i) {
        JSONVar section = incoming[g_secretPaths[i][0]];
        if (JSON.typeof(section) != "object" ||
            !section.hasOwnProperty(g_secretPaths[i][1])) {
            continue;
        }
        JSONVar value = section[g_secretPaths[i][1]];
        if (String((const char*)value) == g_configSecretMask) {
            logger.error("Refusing to save config: could not restore " +
                         String(g_secretPaths[i][0]) + "." +
                         String(g_secretPaths[i][1]));
            request->send(500, "text/plain", "secret restore failed");
            return;
        }
    }

    // Refuse anything loadFile() would reject at boot. Persisting such a
    // document is how the editor bricks the device: the write succeeds, the
    // page reports success, and the next reboot falls back to compiled defaults
    // that cannot join the network — leaving no way in except USB.
    String problem;
    if (!configDocumentIsUsable(incoming, problem)) {
        logger.error("Refusing to save config: '" + problem + "' is shorter than " +
                     String(g_configMinStringLength) + " characters.");
        request->send(400,
                      "text/plain",
                      "'" + problem + "' must have at least " +
                        String(g_configMinStringLength) + " characters");
        return;
    }

    if (!config.saveFile(JSON.stringify(incoming))) {
        request->send(500, "text/plain", "failed to write config");
        return;
    }

    // The login password lives in /users.json, not /config.json, so changing
    // ota.password has to be pushed into the user store or the new value would
    // only take effect after a filesystem deploy wiped /users.json.
    if (!newOtaPassword.isEmpty()) {
        const String username = (const char*)incoming["ota"]["username"];
        if (!username.isEmpty()) {
            userStore.upsert(username, newOtaPassword, Role::ADMIN);
            userStore.save();
            logger.warning("Credentials updated for '" + username +
                           "'. Existing sessions stay valid until logout.");
        }
    }

    // Nothing re-reads config.json at runtime.
    AsyncWebServerResponse* response = request->beginResponse(
      200, "application/json", "{\"saved\":true,\"restartRequired\":true}");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static size_t
countAdmins()
{
    size_t admins = 0;
    for (size_t i = 0; i < userStore.size(); ++i) {
        if (userStore.at(i).role == Role::ADMIN) {
            ++admins;
        }
    }
    return admins;
}

// Usernames and roles only. The salt and the password hash never leave the
// device — /spiffs/users* is shadowed with a 403 for the same reason.
static void
handleUsersJson(AsyncWebServerRequest* request)
{
    JSONVar users;
    for (size_t i = 0; i < userStore.size(); ++i) {
        JSONVar entry;
        entry["username"] = userStore.at(i).username;
        entry["role"] = (int)userStore.at(i).role;
        users[i] = entry;
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(users));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static void
handleUsersPost(AsyncWebServerRequest* request)
{
    String action, username, password;
    int role = (int)Role::OPERATOR;

    for (int i = 0; i < request->params(); ++i) {
        const AsyncWebParameter* param = request->getParam(i);
        if (param->name() == "action") {
            action = param->value();
        } else if (param->name() == "username") {
            username = param->value();
        } else if (param->name() == "password") {
            password = param->value();
        } else if (param->name() == "role") {
            role = param->value().toInt();
        }
    }

    if (username.isEmpty()) {
        request->send(400, "text/plain", "Missing username");
        return;
    }
    if ((role != (int)Role::OPERATOR) && (role != (int)Role::ADMIN)) {
        request->send(400, "text/plain", "Invalid role");
        return;
    }

    const int index = userStore.find(username);

    if (action == "delete") {
        if (index < 0) {
            request->send(404, "text/plain", "User not found");
            return;
        }
        if ((userStore.at((size_t)index).role == Role::ADMIN) &&
            (countAdmins() <= 1)) {
            request->send(400, "text/plain", "Cannot delete the last admin");
            return;
        }

        userStore.remove(username);
        userStore.save();
        logger.warning("User '" + username + "' deleted.");

        // remove() shifts every later entry, and a Session holds an index.
        customLogin.invalidateAllSessions();
        request->send(200, "application/json", "{\"reauth\":true}");
        return;
    }

    if (action == "upsert") {
        // Demoting the only admin locks every administrative route — including
        // this one — behind an account that no longer exists. fullbot guards
        // deletion but not demotion; both close the same door.
        if ((index >= 0) && (userStore.at((size_t)index).role == Role::ADMIN) &&
            (role != (int)Role::ADMIN) && (countAdmins() <= 1)) {
            request->send(400, "text/plain", "Cannot demote the last admin");
            return;
        }

        if (password.isEmpty()) {
            if (index < 0) {
                request->send(400, "text/plain", "Password required for a new user");
                return;
            }
            userStore.setRole(username, (Role)role);
        } else {
            if (password.length() < g_configMinStringLength) {
                request->send(400,
                              "text/plain",
                              "Password must have at least " +
                                String(g_configMinStringLength) + " characters");
                return;
            }
            userStore.upsert(username, password, (Role)role);
        }

        userStore.save();
        logger.info("User '" + username + "' saved with role " + String(role) + ".");
        request->send(200, "application/json", "{\"reauth\":false}");
        return;
    }

    request->send(400, "text/plain", "Invalid action");
}

// Refuses a path outright. Used to shadow files that a serveStatic below would
// otherwise expose.
static void
handleForbidden(AsyncWebServerRequest* request)
{
    request->send(403, "text/plain", "Forbidden");
}

static void
servePublicFile(const char* route, const char* path, const char* contentType)
{
    g_webServer.on(
      route, HTTP_GET, [path, contentType](AsyncWebServerRequest* request) {
          digitalWrite(LED_BUILTIN, 1);
          request->send(SPIFFS, path, contentType);
          digitalWrite(LED_BUILTIN, 0);
      });
}

static void
wifiConnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("Wifi connected.");
    g_wifiConnected = true;
}

static void
wifiGotIP(WiFiEvent_t event, WiFiEventInfo_t info)
{
    logger.info("IP address: " + WiFi.localIP().toString());
    g_hasNetwork = true;
}

static void
wifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (g_wifiConnected) {
        logger.warning("Wifi disconnected. Reconnecting...");
    }

    g_wifiConnected = false;
    g_hasNetwork = false;

    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());
}

void
webSetup()
{
    logger.info("Web setup...");

    g_dataMutex = xSemaphoreCreateMutex();
    webUpdateDataCache();

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_hostname.c_str());
    WiFi.onEvent(wifiConnected, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(wifiGotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.begin(g_ssid.c_str(), g_wifiPassword.c_str());

    if (MDNS.begin(g_hostname.c_str()) == false) {
        logger.warning("Error starting mDNS!");
    }

    // Public assets: the pages themselves carry no data, and the login page has
    // to be reachable before a token exists. Everything that reads or changes
    // state is guarded below.
    servePublicFile("/", "/index.html", "text/html");
    servePublicFile("/index.html", "/index.html", "text/html");
    servePublicFile("/index.js", "/index.js", "application/javascript");
    servePublicFile("/login.html", "/login.html", "text/html");
    servePublicFile("/login.js", "/login.js", "application/javascript");
    // Vendored so the pages work with the WAN down. The config page in
    // particular exists to fix a device that cannot reach the internet, and
    // with jQuery on a CDN it rendered blank in exactly that situation.
    servePublicFile("/jquery.js", "/jquery.js", "application/javascript");
    servePublicFile("/spark-md5.js", "/spark-md5.js", "application/javascript");
    servePublicFile("/sha256.js", "/sha256.js", "application/javascript");
    servePublicFile("/auth.js", "/auth.js", "application/javascript");
    servePublicFile("/config.html", "/config.html", "text/html");
    servePublicFile("/config.js", "/config.js", "application/javascript");
    servePublicFile("/users.html", "/users.html", "text/html");
    servePublicFile("/users.js", "/users.js", "application/javascript");
    servePublicFile("/update.html", "/update.html", "text/html");
    servePublicFile("/update.js", "/update.js", "application/javascript");
    servePublicFile("/favicon.ico", "/favicon.ico", "image/x-icon");

#if USE_CUSTOM_LOGIN
    customLogin.begin();

    g_webServer.on("/nonce", HTTP_GET, [](AsyncWebServerRequest* request) {
        customLogin.handleNonce(request);
    });
    g_webServer.on("/login", HTTP_POST, [](AsyncWebServerRequest* request) {
        customLogin.handleLogin(request);
    });
    g_webServer.on("/logout", HTTP_POST, [](AsyncWebServerRequest* request) {
        customLogin.handleLogout(request);
    });

    auto& authenticated = customLogin.authenticationMiddleware;
    auto* adminOnly = customLogin.requireRole(Role::ADMIN);
    auto* operatorOnly = customLogin.requireRole(Role::OPERATOR);

    g_webServer.on("/data.json", HTTP_GET, handleDataJson)
      .addMiddleware(&authenticated);
    g_webServer.on("/control", HTTP_POST, handleControl)
      .addMiddleware(operatorOnly);
    g_webServer.on("/logs", HTTP_GET, handleLogs).addMiddleware(adminOnly);
    g_webServer.on("/config.json", HTTP_GET, handleConfigGet)
      .addMiddleware(adminOnly);
    g_webServer.on("/config.json", HTTP_POST, handleConfigPost)
      .addMiddleware(adminOnly);
    g_webServer.on("/users.json", HTTP_GET, handleUsersJson)
      .addMiddleware(adminOnly);
    g_webServer.on("/users", HTTP_POST, handleUsersPost).addMiddleware(adminOnly);
    g_webServer.on("/updateEnable", HTTP_POST, handleUpdateEnable)
      .addMiddleware(adminOnly);
    g_webServer
      .on("/update", HTTP_POST, handleUpdateRequest, handleUpdateUpload)
      .addMiddleware(adminOnly);

    // MUST be registered BEFORE the serveStatic they shadow: handlers are
    // matched in registration order, and the matcher is a PREFIX, so the
    // rotated users.bak.json variants are covered too. /config.json carries the
    // WiFi and MQTT passwords in plaintext, /users.json the password hashes and
    // salts, /sessions.json live bearer tokens — none of them may ever be
    // served, even to an admin.
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/users"), HTTP_GET, handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/sessions"),
          HTTP_GET,
          handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer
      .on(AsyncURIMatcher::prefix("/spiffs/config"), HTTP_GET, handleForbidden)
      .addMiddleware(adminOnly);
    g_webServer.serveStatic("/spiffs", SPIFFS, "/").addMiddleware(adminOnly);
#else
    g_webServer.on("/data.json", HTTP_GET, handleDataJson);
    g_webServer.on("/control", HTTP_POST, handleControl);
    g_webServer.on("/logs", HTTP_GET, handleLogs);
    g_webServer.on("/updateEnable", HTTP_POST, handleUpdateEnable);
    g_webServer.on(
      "/update", HTTP_POST, handleUpdateRequest, handleUpdateUpload);
#endif

    g_webServer.onNotFound(
      [](AsyncWebServerRequest* request) { request->send(404); });

    g_webServer.begin();

    logger.info("Web setup done!");
}
