#pragma once

#include "core/role.h"
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

  private:
    static constexpr size_t kTokenLen = 64;
    static constexpr size_t kNonceLen = 32;
    // Eight, not four. Four slots shared across every user makes multi-device
    // unusable — two people on two devices each fills the table — and a script
    // that authenticates in a loop then evicts whoever is using the browser,
    // which this repo has already paid for once during an OTA. The cost is one
    // Session struct per extra slot of static RAM.
    static constexpr size_t kMaxSessions = 8;
    static constexpr size_t kMaxNonces = 8;
    static constexpr size_t kMaxAttemptTrackers = 16;
    static constexpr uint32_t kSessionTtlMs = 24UL * 60UL * 60UL * 1000UL;
    static constexpr uint32_t kNonceTtlMs = 30UL * 1000UL;
    static constexpr uint32_t kLockoutMs = 60UL * 1000UL;
    static constexpr uint8_t kMaxFailures = 5;
    static constexpr const char* kSessionFile = "/sessions.json";

    struct Session
    {
        char token[kTokenLen + 1];
        size_t userIndex;
        uint32_t lastSeenMs;
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

    void generateRandomHex(char* out, size_t hexLen);
    bool consumeNonce(const String& candidate);
    void purgeExpiredNonces(uint32_t now);
    void purgeExpiredSessions(uint32_t now);
    Session* findSessionByToken(const String& token);
    // Index of the slot serving this request, or kMaxSessions for none.
    size_t sessionSlotIndex(AsyncWebServerRequest* request);
    Session* allocateSessionSlot();
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
