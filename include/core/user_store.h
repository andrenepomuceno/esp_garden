#pragma once

#include "core/role.h"
#include <FS.h>
#include <SPIFFS.h>
#include <WString.h>
#include <vector>

struct StoredUser
{
    String username;
    String passwordHash;
    String salt;
    Role role;
};

// Persistent user database, kept out of /config.json so a config dump can never
// carry credentials. On-disk file: /users.json (SPIFFS) — an array of
// {username, passwordHash, salt, role}. Passwords are stored only as
// sha256(salt + ":" + password).
class UserStore
{
  public:
    UserStore();

    // Loads /users.json. When `legacyUsername`/`legacyPassword` are supplied
    // (config.json's ota.* pair) and that user is not stored yet, they are
    // migrated in as ADMIN — this is how an existing device gets its first
    // account without a hardcoded password baked into the firmware.
    bool load(FS& filesystem = SPIFFS,
              const String& legacyUsername = String(),
              const String& legacyPassword = String());

    bool save(FS& filesystem = SPIFFS);

    // Index of `username`, or -1.
    int find(const String& username) const;

    size_t size() const { return entries.size(); }
    const StoredUser& at(size_t i) const { return entries[i]; }

    void upsert(const String& username, const String& password, Role role);
    bool setRole(const String& username, Role role);
    bool remove(const String& username);

  private:
    String filename;
    std::vector<StoredUser> entries;
};

extern UserStore userStore;
