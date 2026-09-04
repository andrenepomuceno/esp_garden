#include "core/config.h"
#include "core/logger.h"
#include "core/role.h"
#include "core/user_store.h"
#include "network/custom_login.h"
#include "network/web_config.h"
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>

// Credentials are masked on the way out and restored on the way back in, so a
// plaintext secret never leaves the device — unlike fullbot, which serves the
// raw /config.json to any authenticated session.
static const char* const g_secretPaths[][2] = {
    { "wifi", "password" },     { "ota", "password" },
    { "thingSpeak", "apiKey" }, { "talkBack", "apiKey" },
    { "mqtt", "password" },
    // The ThingsBoard access token is the whole credential and it lives in
    // mqtt.username, so that field is as sensitive as a password there. It is
    // also a credential on ThingSpeak. Masking it costs nothing and leaving it
    // out served the token in plaintext to any admin session.
    { "mqtt", "username" },
};
static const size_t g_secretCount =
  sizeof(g_secretPaths) / sizeof(g_secretPaths[0]);

void
handleConfigGet(AsyncWebServerRequest* request)
{
    String raw = config.readFile();
    if (raw.isEmpty()) {
        request->send(500, "text/plain", "config unreadable");
        return;
    }

    // ?secrets=1 exports the document verbatim, for a backup that can actually
    // be restored. Masked fields cannot: they come back as asterisks, so a
    // "backup" taken through the normal path silently loses every credential —
    // which is how a filesystem deploy costs you the broker token.
    //
    // This is deliberately an explicit, single-purpose action rather than a
    // browsable path: /spiffs/config* stays 403 and the default GET stays
    // masked. It is ADMIN-only and it says who asked, in the log.
    if (request->hasParam("secrets") &&
        request->getParam("secrets")->value() == "1") {
        logger.warning("Config exported WITH secrets to " +
                       request->client()->remoteIP().toString());
        AsyncWebServerResponse* response =
          request->beginResponse(200, "application/json", raw);
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Content-Disposition",
                            "attachment; filename=\"config-backup.json\"");
        request->send(response);
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

        JSONVar docSection = doc[section];
        if (JSON.typeof(docSection) != "object" ||
            !docSection.hasOwnProperty(key)) {
            continue;
        }

        // An empty field is not masked. There is nothing to hide, and masking
        // it makes the document unsavable: the POST handler cannot restore a
        // mask from an empty stored value, so the mask survives and the final
        // sweep refuses the whole save. That is not hypothetical — a freshly
        // provisioned board has an empty mqtt.username, and a ThingsBoard
        // device authenticates by token with an empty mqtt.password. Either
        // one made every save fail with a 500 naming a field the user never
        // touched.
        JSONVar current = docSection[key];
        if (String((const char*)current).isEmpty()) {
            continue;
        }

        doc[section][key] = g_configSecretMask;
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(doc));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void
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
    bool reauth = false;
    if (!newOtaPassword.isEmpty()) {
        // Named local, not a chained subscript: operator[] returns BY VALUE,
        // so `incoming["ota"]["username"]` cast straight to const char* reads
        // a freed buffer and yields an empty String — which would skip the
        // upsert and leave the new password never taking effect.
        JSONVar otaSection = incoming["ota"];
        JSONVar usernameVar = otaSection["username"];
        const String username = (const char*)usernameVar;
        const int index = username.isEmpty() ? -1 : userStore.find(username);

        // "ota.password arrived unmasked" is NOT the same as "the password
        // changed". The documented backup/restore round trip is
        // GET ?secrets=1 -> edit -> POST, and that GET returns the password in
        // plaintext, so an ordinary restore echoes the SAME password back. Left
        // unchecked this re-salts it, rewrites /users.json and signs every
        // admin out mid-restore with nothing actually altered. Comparing
        // against the stored hash costs one SHA-256 and skips two blocking
        // LittleFS writes on the common path.
        bool changed = true;
        if (index >= 0) {
            const StoredUser& stored = userStore.at((size_t)index);
            changed = (CustomLogin::hashPassword(stored.salt, newOtaPassword) !=
                       stored.passwordHash);
        }

        if (username.isEmpty()) {
            logger.warning("ota.password was set but ota.username is empty; "
                           "the login credential was left untouched.");
        } else if (!changed) {
            logger.info("ota.password is unchanged; sessions were left alone.");
        } else {
            userStore.upsert(username, newOtaPassword, Role::ADMIN);
            userStore.save();

            // This is the SECOND door onto a password, and it owes the same
            // invalidation POST /users does — a veto placed at one call site is
            // the one that gets forgotten. It used to log "existing sessions
            // stay valid until logout", which was true and was the hole: while
            // only one persistent session per user could exist, the next login
            // revoked the old token by accident. Nothing does that now.
            //
            // find() is re-read after the upsert because a brand-new account is
            // appended, and its index is only known afterwards.
            const int saved = userStore.find(username);
            if (saved >= 0) {
                reauth =
                  customLogin.invalidateUserSessions((size_t)saved, request);
            }
            // How many sessions actually ended is logged by
            // invalidateUserSessions(), which is the only place that counts
            // them. Claiming it here would overstate on an account that had
            // none, and /logs is the only durable audit trail this device keeps
            // for a credential write.
            logger.warning("Login credential updated for '" + username + "'.");
        }
    }

    // Nothing re-reads config.json at runtime.
    AsyncWebServerResponse* response = request->beginResponse(
      200,
      "application/json",
      reauth ? "{\"saved\":true,\"restartRequired\":true,\"reauth\":true}"
             : "{\"saved\":true,\"restartRequired\":true,\"reauth\":false}");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}
