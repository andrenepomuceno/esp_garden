#include "network/custom_login.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/session_slots.h"
#include "core/tasks.h" // g_safeTimestamp: one definition of "the clock is usable"
#include "core/user_store.h"
#include <Arduino_JSON.h>
#include "core/filesystem.h"
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

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
        session.createdAtEpoch = 0;
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

// Runs at the top of authorizeFunction(), i.e. on EVERY guarded request, on the
// single async_tcp task that also carries a 1.2 MB OTA upload. It must
// therefore never touch flash: the rewrite is queued here and written by the io
// task at 1 Hz, the same shape relaysTick() uses to hand a bit to the io task
// rather than build a message under a 50 ms deadline.
//
// Deferring is safe HERE and would not be safe on the revocation paths, and the
// difference is what the queue rests on. Everything this function drops is
// either past an ABSOLUTE TTL — so a reboot inside the one-second window
// restores it and the very next purge drops it again, converging — or an
// idempotent timestamp stamp. A token dropped by invalidateUserSessions() or
// displaced by handleLogin() has no such re-check: restore it once and it lives
// out its natural life. Those still write synchronously, which is also the
// reboot the operator is most likely to trigger, since POST /config.json
// answers restartRequired.
void
CustomLogin::purgeExpiredSessions(uint32_t now)
{
    const time_t wallClock = time(NULL);
    const bool clockUsable = (wallClock >= g_safeTimestamp);

    for (auto& session : sessions) {
        if (!session.active) {
            continue;
        }

        if (session.persistent) {
            switch (session_slots::persistentVerdict(clockUsable,
                                                     session.createdAtEpoch,
                                                     (uint32_t)wallClock,
                                                     kPersistentTtlSec)) {
                case session_slots::PurgeVerdict::Keep:
                    break;
                case session_slots::PurgeVerdict::Stamp:
                    session.createdAtEpoch = (uint32_t)wallClock;
                    sessionFile.markStale();
                    break;
                case session_slots::PurgeVerdict::Drop:
                    dropSession(session);
                    sessionFile.markStale();
                    break;
            }
            continue;
        }

        if (session_slots::idledOut(now, session.lastSeenMs, kSessionTtlMs)) {
            dropSession(session);
        }
    }
}

// The consumer, called from the io task at 1 Hz. It is the only place a session
// file write happens off the request path, and it fires a handful of times in a
// device's life — a 30-day expiry, or the one-off stamping of an entry written
// by a firmware that stored no date.
void
CustomLogin::flushPendingSessionSave()
{
    if (sessionFile.takePending()) {
        savePersistentSessions();
    }
}

void
CustomLogin::dropSession(Session& session)
{
    session.active = false;
    session.persistent = false;
    session.createdAtEpoch = 0;
    memset(session.token, 0, sizeof(session.token));
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
    const int index = session_slots::allocate(sessions, kMaxSessions, millis());
    return (index != session_slots::kNoSlot) ? &sessions[(size_t)index]
                                             : nullptr;
}

bool
CustomLogin::enforcePersistentCap()
{
    if (session_slots::countPersistent(sessions, kMaxSessions) <
        kMaxPersistentSessions) {
        return false;
    }

    const int oldest =
      session_slots::oldestPersistent(sessions, kMaxSessions, millis());
    if (oldest == session_slots::kNoSlot) {
        return false;
    }

    logger.info("Session cap reached: forgetting the oldest remembered device.");
    dropSession(sessions[(size_t)oldest]);
    return true;
}

CustomLogin::Session*
CustomLogin::sessionForRequest(AsyncWebServerRequest* request)
{
    if (!request || !request->hasHeader("Authorization-Token")) {
        return nullptr;
    }
    return findSessionByToken(
      request->getHeader("Authorization-Token")->value());
}

int
CustomLogin::sessionSlotIndex(AsyncWebServerRequest* request)
{
    Session* session = sessionForRequest(request);
    return session ? (int)(session - sessions) : session_slots::kNoSlot;
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

    const bool remembered = (remember == "true");

    // Free a remembered slot BEFORE allocating, so a seventh remembered device
    // takes the slot the oldest one just gave up instead of evicting a live
    // ephemeral session.
    bool fileStale = remembered && enforcePersistentCap();

    Session* slot = allocateSessionSlot();
    if (!slot) {
        request->send(503, "text/plain", "No session slots available");
        return;
    }

    // Overwriting the slot revokes whatever token it held. When that token was
    // a REMEMBERED one it also lives in /sessions.json, and the file has to be
    // rewritten even if this new login is not itself persistent — otherwise the
    // next boot restores a token this firmware already threw away, and since
    // purgeExpiredSessions() does not idle out a persistent slot, it would be
    // live forever. Removing the per-user cap is exactly what makes a table
    // full of remembered slots normal, so this is the common path, not a
    // corner case.
    if (slot->active && slot->persistent) {
        fileStale = true;
    }

    // The slot is marked dead for the whole fill and revived on the last line.
    // /sessions.json is now also written from the io task, so this is the one
    // place another thread can observe a slot mid-write, and a torn token
    // reaching flash would be restored at the next boot as a live session
    // matching nothing. Single-threaded behaviour is unchanged.
    slot->active = false;
    generateRandomHex(slot->token, kTokenLen);
    slot->userIndex = (size_t)userIdx;
    slot->lastSeenMs = now;
    slot->ip = ip;
    slot->persistent = false;
    slot->createdAtEpoch = 0;

    if (remembered) {
        // Several remembered devices per user, bounded by kMaxPersistentSessions
        // rather than by the old one-per-user rule, which made "remember me" on
        // a phone silently un-remember a laptop. What that eviction also did,
        // invisibly, was revoke the old token whenever a password change was
        // followed by a fresh login — so removing it is only safe alongside
        // invalidateUserSessions(), which every password-writing path calls.
        const time_t wallClock = time(NULL);
        slot->createdAtEpoch =
          (wallClock >= g_safeTimestamp) ? (uint32_t)wallClock : 0;
        slot->persistent = true;
        fileStale = true;
    }

    slot->active = true;

    // Synchronous, deliberately: a displaced remembered token that only lives
    // on flash would be restored at the next boot and, being exempt from the
    // idle TTL, would never expire — the bug the purge's deferral is careful
    // not to reintroduce.
    if (fileStale) {
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
    Session* session = sessionForRequest(request);
    if (session) {
        String name = (session->userIndex < userStore.size())
                        ? userStore.at(session->userIndex).username
                        : String("?");
        logger.info("Logout: user='" + name + "'");

        const bool wasPersistent = session->persistent;
        dropSession(*session);
        if (wasPersistent) {
            savePersistentSessions();
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
        json += ",\"c\":";
        json += String(session.createdAtEpoch);
        json += "}";
    }
    json += "]";

    // This snapshot is strictly newer than anything the purge queued, so it
    // supersedes it. Without this a deferred write could land AFTER a login and
    // put a file on flash that predates the token it just issued.
    sessionFile.markWritten();

    File file = FILESYSTEM.open(kSessionFile, FILE_WRITE);
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
    if (!FILESYSTEM.exists(kSessionFile)) {
        return;
    }

    File file = FILESYSTEM.open(kSessionFile, FILE_READ);
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
    int discarded = 0;
    for (int i = 0; i < arr.length(); ++i) {
        JSONVar entry = arr[i];
        if (JSON.typeof(entry) != "object") {
            ++discarded;
            continue;
        }

        String token = (const char*)entry["t"];
        int userIndex = (int)entry["u"];
        JSONVar createdVar = entry["c"];
        const uint32_t createdAtEpoch = (JSON.typeof(createdVar) == "number")
                                          ? (uint32_t)(double)createdVar
                                          : 0;
        if (token.length() != kTokenLen) {
            ++discarded;
            continue;
        }
        if (userIndex < 0 || (size_t)userIndex >= userStore.size()) {
            ++discarded;
            continue;
        }

        // allocateSessionSlot() EVICTS rather than failing, which is right for
        // a login and wrong here — a file with more entries than slots would
        // restore each one over the last and report a count the table does not
        // hold. Take only free slots, and only up to the persistent cap, or a
        // full file would leave no room for an ordinary login at all.
        const int index = ((size_t)restored < kMaxPersistentSessions)
                            ? session_slots::allocateFree(sessions, kMaxSessions)
                            : session_slots::kNoSlot;
        if (index == session_slots::kNoSlot) {
            ++discarded;
            continue;
        }

        Session* slot = &sessions[(size_t)index];
        token.toCharArray(slot->token, kTokenLen + 1);
        slot->userIndex = (size_t)userIndex;
        slot->lastSeenMs = millis();
        slot->createdAtEpoch = createdAtEpoch;
        slot->active = true;
        slot->persistent = true;
        ++restored;
    }

    if (restored > 0) {
        logger.info("Restored " + String(restored) +
                    " persistent session(s) from " + String(kSessionFile));
    }

    // Anything the table could not take is REWRITTEN AWAY, not merely skipped.
    // Left on flash it would warn on every boot, and — because
    // invalidateUserSessions() can only rewrite the file from the slots it
    // loaded — a token living solely in that untouched tail would survive the
    // password change that was supposed to kill it.
    if (discarded > 0) {
        logger.warning("Dropped " + String(discarded) + " unusable or surplus "
                       "entr(y/ies) from " + String(kSessionFile) + ".");
        savePersistentSessions();
    }
}

void
CustomLogin::begin()
{
    loadPersistentSessions();
}

void
CustomLogin::invalidateAllSessions()
{
    session_slots::invalidateAll(
      sessions, kMaxSessions, session_slots::kNoSlot, [](Session& session) {
          session.createdAtEpoch = 0;
          memset(session.token, 0, sizeof(session.token));
      });
    savePersistentSessions();
    logger.warning("All sessions invalidated; every client must sign in again.");
}

bool
CustomLogin::invalidateUserSessions(size_t userIndex,
                                    AsyncWebServerRequest* request)
{
    const int callerSlot = sessionSlotIndex(request);

    const session_slots::InvalidateResult result =
      session_slots::invalidateUser(sessions,
                                    kMaxSessions,
                                    userIndex,
                                    callerSlot,
                                    [](Session& session) {
                                        session.createdAtEpoch = 0;
                                        memset(session.token,
                                               0,
                                               sizeof(session.token));
                                    });

    if (result.persistentDropped) {
        savePersistentSessions();
    }
    if (result.dropped > 0) {
        const String name = (userIndex < userStore.size())
                              ? userStore.at(userIndex).username
                              : String("?");
        logger.warning("Ended " + String((unsigned)result.dropped) +
                       " session(s) for '" + name +
                       "'; that account must sign in again.");
    }
    return result.droppedSlot;
}

void
CustomLogin::authorizeFunction(AsyncWebServerRequest* request,
                               ArMiddlewareNext next)
{
    uint32_t now = millis();
    purgeExpiredSessions(now);

    Session* session = sessionForRequest(request);
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
    Session* session = sessionForRequest(request);
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
