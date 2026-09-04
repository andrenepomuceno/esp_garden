#pragma once

#include "core/role.h"
#include "core/session_slots.h"
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <IPAddress.h>

// Nonce + SHA-256 challenge authentication, ported from fullbot-firmware.
//
// The password never crosses the wire, and neither does a reusable hash:
//   1. GET  /nonce?username=<u>  -> {nonce, salt, ttlMs}
//   2. passwordHash = sha256hex(salt + ":" + password)
//   3. response     = sha256hex(nonce + ":" + passwordHash)
//   4. POST /login  with username, nonce, response [, remember=true] -> {token}
//   5. every later request carries the header `Authorization-Token: <token>`
//
// There is no HTTP Basic path: `curl -u user:pass` gets 401 on every guarded
// route.
class CustomLogin
{
  public:
    static void sha256Hex(const String& input, char* outHex /*[65]*/);
    static String hashPassword(const String& salt, const String& password);
    static String generateSalt();

    AsyncMiddlewareFunction authenticationMiddleware;

    CustomLogin();
    ~CustomLogin();

    void begin(); // call after the filesystem is mounted and users are loaded

    void handleNonce(AsyncWebServerRequest* request);
    void handleLogin(AsyncWebServerRequest* request);
    void handleLogout(AsyncWebServerRequest* request);

    // Allocates one middleware per guarded route, at webSetup() time only.
    AsyncMiddlewareFunction* requireRole(Role minRole);

    // Drops every session, persistent ones included. MUST be called after any
    // UserStore mutation that reorders entries: a Session stores the user's
    // INDEX, and removing an entry shifts every later one, so live sessions
    // would silently start resolving to a different account — and to its role.
    void invalidateAllSessions();

    // Drops every session belonging to ONE user, the caller's own included.
    // Every path that writes a password owes this call: several sessions per
    // user are allowed now, so logging in again no longer revokes the previous
    // token as a side effect. Safe for upsert(), which replaces in place or
    // appends and therefore never shifts an index; a REMOVAL still owes
    // invalidateAllSessions() instead.
    //
    // `request` is the session making the change, used only to report whether
    // it signed itself out. Returns true when it did, so the handler can answer
    // {"reauth":true} and the page can sign itself out — exactly as the delete
    // path already does.
    bool invalidateUserSessions(size_t userIndex,
                                AsyncWebServerRequest* request);

    // Writes /sessions.json when the session purge has marked it stale. Call it
    // from a BACKGROUND task, once a second is ample: the purge runs at the top
    // of the authorization middleware, on the same single async_tcp task that
    // carries an OTA upload, and a LittleFS write there lands on whichever
    // request happens to arrive first. See purgeExpiredSessions() for why this
    // one write may be deferred and the revocation writes may not.
    void flushPendingSessionSave();

  private:
    static constexpr size_t kTokenLen = 64;
    static constexpr size_t kNonceLen = 32;
    // Eight, not four. Four slots shared across every user makes multi-device
    // unusable — two people on two devices each fills the table — and a script
    // that authenticates in a loop then evicts whoever is using the browser,
    // which this repo has already paid for once during an OTA. The cost is one
    // Session struct per extra slot of static RAM.
    static constexpr size_t kMaxSessions = 8;
    // At most six of the eight slots may be REMEMBERED. A persistent slot is
    // exempt from the idle TTL, so without a cap eight remembered logins hold
    // the whole table forever and every ordinary login then evicts somebody's
    // remembered device — the failure raising kMaxSessions was meant to
    // relieve, merely relocated. Two free-or-ephemeral slots always remain, and
    // session_slots::allocate() drains those first, so an authenticating loop
    // churns them and never reaches a remembered browser.
    static constexpr size_t kMaxPersistentSessions = 6;
    static constexpr size_t kMaxNonces = 8;
    static constexpr size_t kMaxAttemptTrackers = 16;
    static constexpr uint32_t kSessionTtlMs = 24UL * 60UL * 60UL * 1000UL;
    // Remembered sessions do not idle out — that is what "remember me" means —
    // so they expire on ABSOLUTE age instead, measured in wall-clock seconds so
    // it survives the reboots that reset millis(). Thirty days.
    static constexpr uint32_t kPersistentTtlSec = 30UL * 24UL * 60UL * 60UL;
    static constexpr uint32_t kNonceTtlMs = 30UL * 1000UL;
    static constexpr uint32_t kLockoutMs = 60UL * 1000UL;
    static constexpr uint8_t kMaxFailures = 5;
    static constexpr const char* kSessionFile = "/sessions.json";

    struct Session
    {
        char token[kTokenLen + 1];
        size_t userIndex;
        uint32_t lastSeenMs;
        // Wall-clock seconds at creation, 0 when the clock was not yet synced.
        // millis() cannot carry this: loadPersistentSessions() would restamp
        // every restored session at every boot, so a millis-based expiry on a
        // device that reboots weekly would never fire — a TTL in name only.
        uint32_t createdAtEpoch;
        IPAddress ip;
        bool active;
        bool persistent; // survives reboots via /sessions.json
    };

    struct PendingNonce
    {
        char value[kNonceLen + 1];
        uint32_t createdAtMs;
        bool active;
    };

    struct AttemptTracker
    {
        IPAddress ip;
        uint8_t failures;
        uint32_t lockedUntilMs;
        uint32_t lastFailureMs;
        bool active;
    };

    Session sessions[kMaxSessions];
    PendingNonce nonces[kMaxNonces];
    AttemptTracker attempts[kMaxAttemptTrackers];
    // Marked on the request path, consumed by flushPendingSessionSave().
    session_slots::SaveQueue sessionFile;

    void generateRandomHex(char* out, size_t hexLen);
    bool consumeNonce(const String& candidate);
    void purgeExpiredNonces(uint32_t now);
    void purgeExpiredSessions(uint32_t now);
    Session* findSessionByToken(const String& token);
    // The session serving this request, or nullptr. Four call sites used to
    // repeat the header read and the lookup; one of them getting the guard
    // wrong is an authentication bypass.
    Session* sessionForRequest(AsyncWebServerRequest* request);
    // Its slot, or session_slots::kNoSlot.
    int sessionSlotIndex(AsyncWebServerRequest* request);
    Session* allocateSessionSlot();
    // Frees a remembered slot when the cap is already met, so a new
    // remember=true login always has one. Returns true when one was given up.
    bool enforcePersistentCap();
    void dropSession(Session& session);
    int findUser(const String& username) const;
    AttemptTracker* getOrCreateTracker(const IPAddress& ip);
    bool isLocked(const IPAddress& ip, uint32_t now);
    void registerFailure(const IPAddress& ip, uint32_t now);
    void clearFailures(const IPAddress& ip);
    bool sessionRole(AsyncWebServerRequest* request, Role& outRole);
    void authorizeFunction(AsyncWebServerRequest* request,
                           ArMiddlewareNext next);
    static bool constantTimeEquals(const char* a, const char* b, size_t len);
    void savePersistentSessions();
    void loadPersistentSessions();
};

extern CustomLogin customLogin;
