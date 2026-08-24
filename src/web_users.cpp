#include "core/config.h"
#include "core/logger.h"
#include "core/role.h"
#include "core/user_store.h"
#include "network/custom_login.h"
#include "network/web_users.h"
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>

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
void
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

void
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
