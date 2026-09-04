#include "core/user_store.h"
#include "core/logger.h"
#include "network/custom_login.h"
#include <Arduino_JSON.h>

UserStore userStore;

UserStore::UserStore()
{
    filename = "/users.json";
}

int
UserStore::find(const String& username) const
{
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].username == username) {
            return (int)i;
        }
    }
    return -1;
}

void
UserStore::upsert(const String& username, const String& password, Role role)
{
    StoredUser user;
    user.username = username;
    user.salt = CustomLogin::generateSalt();
    user.passwordHash = CustomLogin::hashPassword(user.salt, password);
    user.role = role;

    int idx = find(username);
    if (idx >= 0) {
        entries[idx] = user;
    } else {
        entries.push_back(user);
    }
}

// A fresh salt every time, exactly as upsert() does: re-typing the same
// password must not reproduce the same hash on disk. What it deliberately does
// NOT touch is the role — a password door has no business deciding one.
bool
UserStore::setPassword(const String& username, const String& password)
{
    int idx = find(username);
    if (idx < 0) {
        return false;
    }
    entries[idx].salt = CustomLogin::generateSalt();
    entries[idx].passwordHash =
      CustomLogin::hashPassword(entries[idx].salt, password);
    return true;
}

bool
UserStore::setRole(const String& username, Role role)
{
    int idx = find(username);
    if (idx < 0) {
        return false;
    }
    entries[idx].role = role;
    return true;
}

bool
UserStore::remove(const String& username)
{
    int idx = find(username);
    if (idx < 0) {
        return false;
    }
    entries.erase(entries.begin() + idx);
    return true;
}

bool
UserStore::load(FS& filesystem,
                const String& legacyUsername,
                const String& legacyPassword)
{
    logger.info("Loading " + filename + "...");
    entries.clear();

    bool dirty = false;

    if (filesystem.exists(filename)) {
        File file = filesystem.open(filename, FILE_READ);
        if (file == false) {
            logger.error("Failed to open " + filename + ".");
        } else {
            String data = file.readString();
            file.close();

            JSONVar arr = JSON.parse(data);
            if (JSON.typeof(arr) == "array") {
                for (int i = 0; i < arr.length(); ++i) {
                    JSONVar entry = arr[i];
                    if (JSON.typeof(entry) != "object") {
                        continue;
                    }

                    StoredUser user;
                    user.role = Role::OPERATOR;
                    if (JSON.typeof(entry["username"]) == "string") {
                        user.username = (const char*)entry["username"];
                    }
                    if (JSON.typeof(entry["passwordHash"]) == "string") {
                        user.passwordHash = (const char*)entry["passwordHash"];
                    }
                    if (JSON.typeof(entry["salt"]) == "string") {
                        user.salt = (const char*)entry["salt"];
                    }
                    if (JSON.typeof(entry["role"]) != "undefined") {
                        user.role = (Role)(int)entry["role"];
                    }

                    // A half-written entry is dropped rather than stored with
                    // an empty hash, which would match nothing and lock the
                    // account out silently.
                    if (!user.username.isEmpty() &&
                        !user.passwordHash.isEmpty() && !user.salt.isEmpty()) {
                        entries.push_back(user);
                    }
                }
            } else {
                logger.error("Failed to parse " + filename);
            }
        }
    } else {
        logger.warning(filename + " not found.");
        dirty = true;
    }

    // One-time migration of the legacy ota.{username,password} pair from
    // /config.json. This is deliberately the only seeding path that produces a
    // usable account: no default password is compiled into the firmware.
    if (!legacyUsername.isEmpty() && !legacyPassword.isEmpty() &&
        find(legacyUsername) < 0) {
        logger.info("Migrating legacy OTA credentials into " + filename);
        upsert(legacyUsername, legacyPassword, Role::ADMIN);
        dirty = true;
    }

    if (entries.empty()) {
        logger.fatal("No users defined. The web UI is unreachable until "
                     "ota.username/ota.password are set in config.json.");
    }

    if (dirty && !entries.empty()) {
        save(filesystem);
    }

    return !entries.empty();
}

bool
UserStore::save(FS& filesystem)
{
    JSONVar arr;
    for (size_t i = 0; i < entries.size(); ++i) {
        JSONVar entry;
        entry["username"] = entries[i].username;
        entry["passwordHash"] = entries[i].passwordHash;
        entry["salt"] = entries[i].salt;
        entry["role"] = (int)entries[i].role;
        arr[i] = entry;
    }
    String json = JSON.stringify(arr);

    File file = filesystem.open(filename, FILE_WRITE);
    if (file == false) {
        logger.error("Failed to open " + filename + " for writing.");
        return false;
    }
    if (file.print(json) == 0) {
        logger.error("Failed to write " + filename + ".");
        file.close();
        return false;
    }
    file.close();

    return true;
}
