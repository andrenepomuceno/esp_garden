#pragma once

#include "core/role.h"
#include <FS.h>
#include "core/filesystem.h"
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
    bool load(FS& filesystem = FILESYSTEM,
              const String& legacyUsername = String(),
              const String& legacyPassword = String());

    bool save(FS& filesystem = FILESYSTEM);

    // Index of `username`, or -1.
    int find(const String& username) const;

    size_t size() const { return entries.size(); }
    const StoredUser& at(size_t i) const { return entries[i]; }

    // Writes the whole entry: a fresh salt, a new hash AND `role`. Every caller
    // therefore has to supply the role it wants preserved, and a caller that
    // does not is a silent promotion — which is exactly what the ota.password
    // door did until setPassword() below existed. Use this only where the role
    // is genuinely part of what is being written (POST /users, which validates
    // it and guards the last admin), or where the account is being created.
    void upsert(const String& username, const String& password, Role role);
    // The two single-attribute writers. setRole() has always existed; its twin
    // is what a password door wants, because a door that only writes a password
    // should not have to read a role back in order to leave it alone. Returns
    // false when there is no such account, which is the caller's cue that this
    // is a CREATE and it must decide the role deliberately.
    bool setPassword(const String& username, const String& password);
    bool setRole(const String& username, Role role);
    bool remove(const String& username);

  private:
    String filename;
    std::vector<StoredUser> entries;
};

extern UserStore userStore;
