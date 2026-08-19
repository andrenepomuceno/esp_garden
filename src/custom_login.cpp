#include "network/custom_login.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/user_store.h"
#include <Arduino_JSON.h>
#include <SPIFFS.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <string.h>

CustomLogin customLogin;

CustomLogin::CustomLogin()
  : authenticationMiddleware(
      [this](AsyncWebServerRequest* request, ArMiddlewareNext next) {
          this->authorizeFunction(request, next);
      })
{
    for (auto& session : sessions) {
        session.active = false;
        session.persistent = false;
    }
    for (auto& nonce : nonces) {
        nonce.active = false;
    }
    for (auto& attempt : attempts) {
        attempt.active = false;
    }
}

CustomLogin::~CustomLogin() = default;

void
CustomLogin::generateRandomHex(char* out, size_t hexLen)
{
    static const char digits[] = "0123456789abcdef";
    size_t produced = 0;
    while (produced < hexLen) {
        uint32_t r = esp_random();
        for (int i = 0; i < 8 && produced < hexLen; ++i) {
            out[produced++] = digits[(r >> (28 - i * 4)) & 0xF];
        }
    }
    out[hexLen] = '\0';
}

void
CustomLogin::sha256Hex(const String& input, char* outHex)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256, not SHA-224
    mbedtls_sha256_update(
      &ctx, reinterpret_cast<const uint8_t*>(input.c_str()), input.length());
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    for (int i = 0; i < 32; ++i) {
        outHex[i * 2] = digits[(digest[i] >> 4) & 0xF];
        outHex[i * 2 + 1] = digits[digest[i] & 0xF];
    }
    outHex[64] = '\0';
}

String
CustomLogin::hashPassword(const String& salt, const String& password)
{
    char hex[65];
    sha256Hex(salt + ":" + password, hex);
    return String(hex);
}

String
CustomLogin::generateSalt()
{
    static const char digits[] = "0123456789abcdef";
    char dst[17];
    for (int i = 0; i < 16; i += 8) {
        uint32_t r = esp_random();
        for (int j = 0; j < 8 && (i + j) < 16; ++j) {
            dst[i + j] = digits[(r >> (28 - j * 4)) & 0xF];
        }
    }
    dst[16] = '\0';
    return String(dst);
}

// Comparison time must not depend on how many leading characters matched, or
// the response can be recovered one character at a time by timing.
bool
CustomLogin::constantTimeEquals(const char* a, const char* b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return diff == 0;
}

void
CustomLogin::purgeExpiredNonces(uint32_t now)
{
    for (auto& nonce : nonces) {
        if (nonce.active && (now - nonce.createdAtMs) > kNonceTtlMs) {
            nonce.active = false;
        }
    }
}

bool
CustomLogin::consumeNonce(const String& candidate)
{
    if (candidate.length() != kNonceLen) {
        return false;
    }

    uint32_t now = millis();
    purgeExpiredNonces(now);

    for (auto& nonce : nonces) {
        if (!nonce.active) {
            continue;
        }
        if (constantTimeEquals(nonce.value, candidate.c_str(), kNonceLen)) {
            nonce.active = false; // one-shot: no replay
            return true;
        }
    }
    return false;
}

void
CustomLogin::purgeExpiredSessions(uint32_t now)
{
    for (auto& session : sessions) {
        if (!session.active || session.persistent) {
            continue;
        }
        if ((now - session.lastSeenMs) > kSessionTtlMs) {
            session.active = false;
            memset(session.token, 0, sizeof(session.token));
        }
    }
}

CustomLogin::Session*
CustomLogin::findSessionByToken(const String& token)
{
    if (token.length() != kTokenLen) {
        return nullptr;
    }
    for (auto& session : sessions) {
        if (!session.active) {
            continue;
        }
        if (constantTimeEquals(session.token, token.c_str(), kTokenLen)) {
            return &session;
        }
    }
    return nullptr;
}

CustomLogin::Session*
CustomLogin::allocateSessionSlot()
{
    Session* lru = nullptr;
    for (auto& session : sessions) {
        if (!session.active) {
            return &session;
        }
        if (lru == nullptr || session.lastSeenMs < lru->lastSeenMs) {
            lru = &session;
        }
    }
    return lru;
}

int
CustomLogin::findUser(const String& username) const
{
    return userStore.find(username);
}

CustomLogin::AttemptTracker*
CustomLogin::getOrCreateTracker(const IPAddress& ip)
{
    AttemptTracker* freeSlot = nullptr;
    AttemptTracker* lru = nullptr;

    for (auto& attempt : attempts) {
        if (attempt.active && attempt.ip == ip) {
            return &attempt;
        }
        if (!attempt.active) {
            if (!freeSlot) {
                freeSlot = &attempt;
            }
        } else if (!lru || attempt.lastFailureMs < lru->lastFailureMs) {
            lru = &attempt;
        }
    }

    AttemptTracker* slot = freeSlot ? freeSlot : lru;
    if (!slot) {
        return nullptr;
    }

    slot->active = true;
    slot->ip = ip;
    slot->failures = 0;
    slot->lockedUntilMs = 0;
    slot->lastFailureMs = 0;
    return slot;
}

bool
CustomLogin::isLocked(const IPAddress& ip, uint32_t now)
{
    for (auto& attempt : attempts) {
        if (!attempt.active || !(attempt.ip == ip)) {
            continue;
        }
        if (attempt.lockedUntilMs && now < attempt.lockedUntilMs) {
            return true;
        }
        if (attempt.lockedUntilMs && now >= attempt.lockedUntilMs) {
            attempt.failures = 0;
            attempt.lockedUntilMs = 0;
        }
        return false;
    }
    return false;
}

void
CustomLogin::registerFailure(const IPAddress& ip, uint32_t now)
{
    AttemptTracker* attempt = getOrCreateTracker(ip);
    if (!attempt) {
        return;
    }

    if (attempt->failures < 255) {
        ++attempt->failures;
    }
    attempt->lastFailureMs = now;

    if (attempt->failures >= kMaxFailures) {
        attempt->lockedUntilMs = now + kLockoutMs;
    }
}

void
CustomLogin::clearFailures(const IPAddress& ip)
{
    for (auto& attempt : attempts) {
        if (attempt.active && attempt.ip == ip) {
            attempt.failures = 0;
            attempt.lockedUntilMs = 0;
            return;
        }
    }
}

void
CustomLogin::handleNonce(AsyncWebServerRequest* request)
{
    uint32_t now = millis();
    purgeExpiredNonces(now);

    String username;
    if (request->hasParam("username")) {
        username = request->getParam("username")->value();
    }

    // An unknown user still gets a stable, plausible salt, derived from the
    // device id so it is the same on every call. Returning nothing (or a fresh
    // random salt each time) would turn this endpoint into an account
    // enumeration oracle.
    String salt;
    int idx = findUser(username);
    if (idx >= 0) {
        salt = userStore.at((size_t)idx).salt;
    } else {
        char hex[65];
        sha256Hex(String(config.deviceId, HEX) + ":nosuchuser:" + username,
                  hex);
        salt = String(hex).substring(0, 16);
    }

    PendingNonce* slot = nullptr;
    for (auto& nonce : nonces) {
        if (!nonce.active) {
            slot = &nonce;
            break;
        }
    }
    if (!slot) {
        slot = &nonces[0];
        for (auto& nonce : nonces) {
            if (nonce.createdAtMs < slot->createdAtMs) {
                slot = &nonce;
            }
        }
    }

    generateRandomHex(slot->value, kNonceLen);
    slot->createdAtMs = now;
    slot->active = true;

    JSONVar result;
    result["nonce"] = slot->value;
    result["salt"] = salt;
    result["ttlMs"] = (int)kNonceTtlMs;

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(result));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void
CustomLogin::handleLogin(AsyncWebServerRequest* request)
{
    uint32_t now = millis();
    IPAddress ip = request->client()->remoteIP();

    if (isLocked(ip, now)) {
        logger.warning("Login locked out: " + ip.toString());
        AsyncWebServerResponse* response =
          request->beginResponse(429, "text/plain", "Too Many Requests");
        response->addHeader("Retry-After", "60");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        return;
    }

    String username, challengeResponse, nonce, remember;
    for (int i = 0; i < request->params(); ++i) {
        const AsyncWebParameter* param = request->getParam(i);
        if (param->name() == "username") {
            username = param->value();
        } else if (param->name() == "response") {
            challengeResponse = param->value();
        } else if (param->name() == "nonce") {
            nonce = param->value();
        } else if (param->name() == "remember") {
            remember = param->value();
        }
    }

    // Every branch is evaluated before the verdict so an unknown user costs the
    // same as a wrong password.
    bool nonceOk = consumeNonce(nonce);
    int userIdx = findUser(username);

    const String dummyHash = String();
    const String& storedHash = (userIdx >= 0)
                                 ? userStore.at((size_t)userIdx).passwordHash
                                 : dummyHash;
    char expected[65];
    sha256Hex(nonce + ":" + storedHash, expected);

    bool match =
      (challengeResponse.length() == 64) &&
      constantTimeEquals(expected, challengeResponse.c_str(), 64);

    if (!nonceOk || userIdx < 0 || !match) {
        registerFailure(ip, now);
        logger.warning("Unauthorized access attempt from " + ip.toString() +
                       " user='" + username + "'");
        AsyncWebServerResponse* response =
          request->beginResponse(401, "text/plain", "Unauthorized");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        return;
    }

    clearFailures(ip);

    Session* slot = allocateSessionSlot();
    if (!slot) {
        request->send(503, "text/plain", "No session slots available");
        return;
    }

    generateRandomHex(slot->token, kTokenLen);
    slot->userIndex = (size_t)userIdx;
    slot->lastSeenMs = now;
    slot->ip = ip;
    slot->active = true;
    slot->persistent = false;

    if (remember == "true") {
        // One persistent session per user: logging in again from a new browser
        // revokes the old remembered token instead of accumulating them in the
        // four available slots.
        for (auto& session : sessions) {
            if (&session == slot) {
                continue;
            }
            if (session.active && session.persistent &&
                session.userIndex == slot->userIndex) {
                session.active = false;
                memset(session.token, 0, sizeof(session.token));
            }
        }
        slot->persistent = true;
        savePersistentSessions();
    }

    logger.info("Login OK: user='" + userStore.at((size_t)userIdx).username +
                "' from " + ip.toString());

    JSONVar result;
    result["token"] = slot->token;
    result["role"] = (int)userStore.at((size_t)userIdx).role;
    result["ttlMs"] = (int)kSessionTtlMs;

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(result));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void
CustomLogin::handleLogout(AsyncWebServerRequest* request)
{
    if (request->hasHeader("Authorization-Token")) {
        String token = request->getHeader("Authorization-Token")->value();
        Session* session = findSessionByToken(token);
        if (session) {
            String name = (session->userIndex < userStore.size())
                            ? userStore.at(session->userIndex).username
                            : String("?");
            logger.info("Logout: user='" + name + "'");

            bool wasPersistent = session->persistent;
            session->active = false;
            session->persistent = false;
            memset(session->token, 0, sizeof(session->token));
            if (wasPersistent) {
                savePersistentSessions();
            }
        }
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "text/plain", "OK");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void
CustomLogin::savePersistentSessions()
{
    String json = "[";
    bool first = true;
    for (auto& session : sessions) {
        if (!session.active || !session.persistent) {
            continue;
        }
        if (!first) {
            json += ",";
        }
        first = false;
        json += "{\"t\":\"";
        json += session.token;
        json += "\",\"u\":";
        json += String(session.userIndex);
        json += "}";
    }
    json += "]";

    File file = SPIFFS.open(kSessionFile, FILE_WRITE);
    if (file == false) {
        logger.error("savePersistentSessions: failed to open " +
                     String(kSessionFile));
        return;
    }
    file.print(json);
    file.close();
}

void
CustomLogin::loadPersistentSessions()
{
    if (!SPIFFS.exists(kSessionFile)) {
        return;
    }

    File file = SPIFFS.open(kSessionFile, FILE_READ);
    if (file == false) {
        logger.error("loadPersistentSessions: failed to open " +
                     String(kSessionFile));
        return;
    }
    String data = file.readString();
    file.close();

    JSONVar arr = JSON.parse(data);
    if (JSON.typeof(arr) != "array") {
        return;
    }

    int restored = 0;
    for (int i = 0; i < arr.length(); ++i) {
        JSONVar entry = arr[i];
        if (JSON.typeof(entry) != "object") {
            continue;
        }

        String token = (const char*)entry["t"];
        int userIndex = (int)entry["u"];
        if (token.length() != kTokenLen) {
            continue;
        }
        if (userIndex < 0 || (size_t)userIndex >= userStore.size()) {
            continue;
        }

        Session* slot = allocateSessionSlot();
        if (!slot) {
            break;
        }
        token.toCharArray(slot->token, kTokenLen + 1);
        slot->userIndex = (size_t)userIndex;
        slot->lastSeenMs = millis();
        slot->active = true;
        slot->persistent = true;
        ++restored;
    }

    if (restored > 0) {
        logger.info("Restored " + String(restored) +
                    " persistent session(s) from " + String(kSessionFile));
    }
}

void
CustomLogin::begin()
{
    loadPersistentSessions();
}

void
CustomLogin::authorizeFunction(AsyncWebServerRequest* request,
                               ArMiddlewareNext next)
{
    uint32_t now = millis();
    purgeExpiredSessions(now);

    if (!request->hasHeader("Authorization-Token")) {
        AsyncWebServerResponse* response =
          request->beginResponse(401, "text/plain", "Unauthorized");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        return;
    }

    String token = request->getHeader("Authorization-Token")->value();
    Session* session = findSessionByToken(token);
    if (!session) {
        AsyncWebServerResponse* response =
          request->beginResponse(401, "text/plain", "Unauthorized");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        return;
    }

    session->lastSeenMs = now;
    next();
}

bool
CustomLogin::sessionRole(AsyncWebServerRequest* request, Role& outRole)
{
    if (!request->hasHeader("Authorization-Token")) {
        return false;
    }

    String token = request->getHeader("Authorization-Token")->value();
    Session* session = findSessionByToken(token);
    if (!session || session->userIndex >= userStore.size()) {
        return false;
    }

    outRole = userStore.at(session->userIndex).role;
    return true;
}

// Allocated once per guarded route at webSetup() time. This is the only `new`
// in the firmware; it must never be called from a request handler.
AsyncMiddlewareFunction*
CustomLogin::requireRole(Role minRole)
{
    return new AsyncMiddlewareFunction(
      [this, minRole](AsyncWebServerRequest* request, ArMiddlewareNext next) {
          authorizeFunction(request, [this, request, next, minRole]() {
              Role role;
              if (!sessionRole(request, role) ||
                  (uint8_t)role < (uint8_t)minRole) {
                  AsyncWebServerResponse* response =
                    request->beginResponse(403, "text/plain", "Forbidden");
                  response->addHeader("Cache-Control", "no-store");
                  request->send(response);
                  return;
              }
              next();
          });
      });
}
