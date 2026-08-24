#include "core/logger.h"
#include "network/web_files.h"
#include <ESPAsyncWebServer.h>
#include <MD5Builder.h>
#include <SPIFFS.h>

// Written to first, moved into place only once the whole body arrived and the
// checksum matched. Writing straight to the target would let a dropped Wi-Fi
// connection replace a working index.html with half of one.
static const char* const g_uploadTempPath = "/upload.tmp";

static File g_uploadFile;
static MD5Builder g_uploadMD5;
static size_t g_uploadBytes = 0;
static String g_uploadTarget;
static String g_uploadError;

// The same set the /spiffs browse handler shadows with a 403, for the same
// reason: they hold the salted password hashes, the live bearer tokens and the
// plaintext WiFi and MQTT credentials. Uploading over them would be a way to
// install a known password, or to wipe every live session, through a route
// whose entire job is to accept arbitrary bytes.
//
// /config.json is refused too, but for a different reason: POST /config.json
// validates the document, checks the device id and restores masked secrets. A
// raw upload bypasses all three, and an invalid config only fails at the next
// boot — on a device that then cannot reach the network to be fixed.
static bool
uploadPathIsProtected(const String& path)
{
    return path.startsWith("/users") || path.startsWith("/sessions") ||
           path.startsWith("/config");
}

static bool
uploadPathIsUsable(const String& path, String& reason)
{
    if (path.length() < 2 || path[0] != '/') {
        reason = "path must start with a slash";
        return false;
    }
    if (path.indexOf("..") >= 0) {
        reason = "path must not contain ..";
        return false;
    }
    // SPIFFS_OBJ_NAME_LEN is 32 including the terminator. A longer name is
    // truncated silently, so the file lands somewhere the caller did not ask
    // for and every later read misses it.
    if (path.length() > 31) {
        reason = "path longer than 31 characters";
        return false;
    }
    if (path == g_uploadTempPath) {
        reason = "reserved path";
        return false;
    }
    if (uploadPathIsProtected(path)) {
        reason = path.startsWith("/config")
                   ? "use POST /config.json, which validates the document"
                   : "credential store";
        return false;
    }
    return true;
}

void
handleFileUpload(AsyncWebServerRequest* request,
                 String filename,
                 size_t index,
                 uint8_t* data,
                 size_t len,
                 bool final)
{
    if (index == 0) {
        g_uploadError = "";
        g_uploadBytes = 0;
        g_uploadTarget = filename.startsWith("/") ? filename : ("/" + filename);

        String reason;
        if (!uploadPathIsUsable(g_uploadTarget, reason)) {
            g_uploadError = "refused " + g_uploadTarget + ": " + reason;
            logger.warning("[upload] " + g_uploadError);
            return;
        }

        // Both files exist at once until the rename, so the check has to cover
        // the incoming size on top of everything already stored.
        const size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
        const size_t incoming = request->contentLength();
        if (incoming > freeBytes) {
            g_uploadError = "not enough space: " + String(incoming) +
                            " B incoming, " + String(freeBytes) + " B free";
            logger.error("[upload] " + g_uploadError);
            return;
        }

        SPIFFS.remove(g_uploadTempPath);
        g_uploadFile = SPIFFS.open(g_uploadTempPath, FILE_WRITE);
        if (g_uploadFile == false) {
            g_uploadError = "could not open " + String(g_uploadTempPath);
            logger.error("[upload] " + g_uploadError);
            return;
        }

        g_uploadMD5.begin();
        logger.info("[upload] receiving " + g_uploadTarget + " (" +
                    String(incoming) + " B)");
    }

    if (g_uploadError.length() > 0) {
        return; // already refused; drain the body without writing it
    }

    if (len > 0) {
        if (g_uploadFile.write(data, len) != len) {
            g_uploadError = "write failed at byte " + String(g_uploadBytes);
            logger.error("[upload] " + g_uploadError);
            g_uploadFile.close();
            SPIFFS.remove(g_uploadTempPath);
            return;
        }
        g_uploadMD5.add(data, len);
        g_uploadBytes += len;
    }

    if (!final) {
        return;
    }

    g_uploadFile.close();
    g_uploadMD5.calculate();

    if (request->hasParam("MD5", true)) {
        const String expected = request->getParam("MD5", true)->value();
        const String actual = g_uploadMD5.toString();
        if (!expected.equalsIgnoreCase(actual)) {
            g_uploadError =
              "checksum mismatch: expected " + expected + ", got " + actual;
            logger.error("[upload] " + g_uploadError);
            SPIFFS.remove(g_uploadTempPath);
            return;
        }
    }

    // Only now is the old file replaced. SPIFFS.rename refuses an existing
    // destination, so the remove has to come first — that is the one window
    // where the target is missing, and it lasts microseconds rather than the
    // length of an upload.
    SPIFFS.remove(g_uploadTarget);
    if (!SPIFFS.rename(g_uploadTempPath, g_uploadTarget)) {
        g_uploadError = "could not move into place as " + g_uploadTarget;
        logger.error("[upload] " + g_uploadError);
        SPIFFS.remove(g_uploadTempPath);
        return;
    }

    logger.warning("[upload] wrote " + g_uploadTarget + " (" +
                   String(g_uploadBytes) + " B) from " +
                   request->client()->remoteIP().toString());
}

void
handleFileUploadRequest(AsyncWebServerRequest* request)
{
    if (g_uploadError.length() > 0) {
        AsyncWebServerResponse* response = request->beginResponse(
          400,
          "application/json",
          String("{\"ok\":false,\"error\":\"") + g_uploadError + "\"}");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        g_uploadError = "";
        return;
    }

    // A request with no file part reaches here with nothing recorded, which is
    // a caller error rather than a device one.
    if (g_uploadTarget.isEmpty()) {
        request->send(400,
                      "application/json",
                      "{\"ok\":false,\"error\":\"no file in request\"}");
        return;
    }

    AsyncWebServerResponse* response = request->beginResponse(
      200,
      "application/json",
      String("{\"ok\":true,\"path\":\"") + g_uploadTarget +
        "\",\"bytes\":" + String(g_uploadBytes) +
        ",\"free\":" + String(SPIFFS.totalBytes() - SPIFFS.usedBytes()) + "}");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    g_uploadTarget = "";
}

// ---------------------------------------------------------------------------
// Single-file delete
// ---------------------------------------------------------------------------

// The counterpart to the upload, and the reason it exists: a test upload of
// mine (`/probe.js`) sat on the filesystem with no way to remove it short of
// rewriting the whole partition — which is precisely the operation the per-file
// upload was built to avoid.
//
// Same refusals as the upload, for the same reasons: deleting /users.json locks
// every account out, /sessions.json signs everyone out, and /config.json makes
// the device unreachable at its next boot. Nothing here may remove the file
// that lets you fix a mistake made here.
void
handleFileDelete(AsyncWebServerRequest* request)
{
    if (!request->hasParam("path", true)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing path\"}");
        return;
    }

    String path = request->getParam("path", true)->value();
    if (!path.startsWith("/")) {
        path = "/" + path;
    }

    String reason;
    if (!uploadPathIsUsable(path, reason)) {
        logger.warning("[delete] refused " + path + ": " + reason);
        request->send(400,
                      "application/json",
                      String("{\"ok\":false,\"error\":\"refused ") + path +
                        ": " + reason + "\"}");
        return;
    }

    // Refusing a path that is not there, rather than reporting success, is what
    // tells a caller their path was wrong instead of leaving them to wonder why
    // the file is still being served.
    if (!SPIFFS.exists(path)) {
        request->send(404,
                      "application/json",
                      String("{\"ok\":false,\"error\":\"") + path +
                        " does not exist\"}");
        return;
    }

    if (!SPIFFS.remove(path)) {
        request->send(500,
                      "application/json",
                      String("{\"ok\":false,\"error\":\"could not remove ") +
                        path + "\"}");
        return;
    }

    logger.warning("[delete] removed " + path + " from " +
                   request->client()->remoteIP().toString());

    AsyncWebServerResponse* response = request->beginResponse(
      200,
      "application/json",
      String("{\"ok\":true,\"path\":\"") + path + "\",\"free\":" +
        String(SPIFFS.totalBytes() - SPIFFS.usedBytes()) + "}");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}
