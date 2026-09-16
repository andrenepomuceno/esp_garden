// The first-boot setup portal. See include/core/onboarding.h for WHEN this
// runs; this file is what it does once it is running.
//
// -------------------------------------------------------------------------
// SAY THE SECURITY PROPERTY OUT LOUD
// -------------------------------------------------------------------------
//
// The AP password is `espgarden`, it is printed in this repository, and
// POST /onboarding takes NO token. So while the portal is up, anyone in radio
// range can write this board's /config.json — their Wi-Fi credentials, their
// admin account — and walk away with the device. That is the flow the operator
// asked for and it is the flow every consumer device ships, but it is a real
// exposure on a controller that switches real pumps, so it is stated here
// rather than left to be discovered.
//
// What bounds it is WHEN the portal can exist at all, and that is the whole
// point of the marker file:
//
//   - a board that has ever associated carries no marker and can never enter
//     the portal again, whatever happens to the router afterwards;
//   - the window is therefore "between flashing and the first successful
//     association", which on a working setup is one boot.
//
// What does NOT bound it: the route table. That is bounded separately and
// absolutely — webSetup() registers EITHER these three routes OR the normal
// ones, never both, so on a configured device this endpoint does not exist.
//
// -------------------------------------------------------------------------
// NO FILESYSTEM DEPENDENCY, DELIBERATELY
// -------------------------------------------------------------------------
//
// One of the two ways arm 1 fires is `FILESYSTEM.begin(true)` reformatting a
// partition that would not mount — which takes every web asset with it. So the
// page below is a string in flash, loads no script, no stylesheet and no font,
// and the templates are compiled in for the same reason. A portal that needs
// /bootstrap.css to render is a portal that is blank in half the cases it
// exists for.

#include "BuildConfig.h"
#include "core/config.h"
#include "core/filesystem.h"
#include "core/logger.h"
#include "core/onboarding.h"
#include "core/onboarding_templates.h"
#include "core/role.h"
#include "core/tasks.h"
#include "core/user_store.h"
#include "network/web_onboarding.h"
#include <Arduino_JSON.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <stdlib.h>

// Selected the same way core/pin_rules.h and core/default_pins.h are: from
// CONFIG_IDF_TARGET_*, which the framework defines from the env's `board` line.
// A build flag here would be a second statement of "this is an S3", free to
// disagree with the first — and a template from the wrong chip puts relays on
// the S3 carrier's flow input, float switch and user button, and probes on its
// octal flash bus. An unrecognised target is a hard error rather than a
// fallback, for the reason config_pins.cpp gives.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
namespace board = onboarding_templates::esp32s3;
static const char* const kFamilyName = "esp32s3";
#elif defined(CONFIG_IDF_TARGET_ESP32)
namespace board = onboarding_templates::wroom32;
// The namespace's own name, and the same word scripts/dev_server.py takes from
// ESP_GARDEN_PIN_FAMILY. One spelling across the firmware, the mirror and the
// tests, so a disagreement reads as a disagreement rather than a rename.
static const char* const kFamilyName = "wroom32";
#else
#error "Unknown CONFIG_IDF_TARGET: add an onboarding_templates namespace."
#endif

// WPA2 needs eight characters; this is nine. Not a secret and not treated as
// one — see the header comment.
static const char* const kApPassword = "espgarden";

static DNSServer g_dns;
static bool g_dnsUp = false;
static bool g_active = false;
static onboarding::Reason g_reason = onboarding::Reason::Configured;

bool
onboardingActive()
{
    return g_active;
}

bool
onboardingMarkerPresent()
{
    return FILESYSTEM.exists(onboarding::kMarkerPath);
}

void
onboardingMarkerClear()
{
    if (!FILESYSTEM.exists(onboarding::kMarkerPath)) {
        return;
    }
    if (FILESYSTEM.remove(onboarding::kMarkerPath)) {
        logger.info("Associated on the stored credentials; removed " +
                    String(onboarding::kMarkerPath) +
                    ". This board can no longer raise a setup AP.");
    } else {
        // Not fatal, and deliberately loud: the board works, but it will spend
        // 60 s probing at every boot and can still fall to the portal if the
        // router is down. Deleting it by hand through /spiffs is the fix.
        logger.error("Could not remove " + String(onboarding::kMarkerPath) +
                     "; this board stays eligible for the setup portal.");
    }
}

static bool
markerWrite()
{
    File file = FILESYSTEM.open(onboarding::kMarkerPath, FILE_WRITE);
    if (file == false) {
        return false;
    }
    // Content is irrelevant — only existence is read — but an empty file is
    // indistinguishable from a torn write, and one line costs nothing in a
    // 4 KB block that is already spent.
    file.print("written by POST /onboarding; deleted by the first boot that "
               "associates\n");
    file.close();
    return true;
}

static String
onboardingApSsid()
{
    return "espgarden-" + String(config.deviceId, 16);
}

// ---------------------------------------------------------------------------
// The page. One file, no assets, ~4 KB.
// ---------------------------------------------------------------------------
static const char kPage[] = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP Garden setup</title>
<style>
:root{color-scheme:light dark}
body{margin:0;padding:1.2rem;font:16px/1.45 system-ui,sans-serif;max-width:32rem}
h1{font-size:1.3rem;margin:0 0 .2rem}
p.sub{margin:0 0 1.2rem;opacity:.7;font-size:.9rem}
label{display:block;margin:.8rem 0 .2rem;font-weight:600;font-size:.9rem}
input,select{width:100%;box-sizing:border-box;padding:.55rem;font-size:1rem;
 border:1px solid #8888;border-radius:.35rem;background:transparent;color:inherit}
small{display:block;opacity:.7;margin-top:.25rem}
.rev{display:flex;align-items:center;gap:.5rem;font-weight:400;margin-top:1rem}
.rev input{width:auto;margin:0}
button{margin-top:1.4rem;width:100%;padding:.7rem;font-size:1rem;font-weight:600;
 border:0;border-radius:.35rem;background:#2e7d32;color:#fff}
button[disabled]{opacity:.5}
#msg{margin-top:1rem;padding:.7rem;border-radius:.35rem;white-space:pre-wrap}
.err{background:#c62828;color:#fff}
.ok{background:#2e7d32;color:#fff}
</style></head><body>
<h1>ESP Garden setup</h1>
<p class="sub" id="sub">loading...</p>
<form id="f">
<label for="tpl">Board</label>
<select id="tpl" name="template"></select>
<small id="tplsum"></small>

<label for="ssid">Wi-Fi network</label>
<input id="ssid" name="ssid" required minlength="1" autocapitalize="off"
 autocorrect="off" spellcheck="false">

<label for="pw">Wi-Fi password</label>
<input id="pw" name="password" type="password" required minlength="4">
<small>At least 4 characters. This firmware cannot join an open network.</small>

<label for="user">Admin user</label>
<input id="user" name="username" value="admin" required minlength="4"
 autocapitalize="off" autocorrect="off" spellcheck="false">

<label for="apw">Admin password</label>
<input id="apw" name="adminPassword" type="password" required minlength="4">

<label class="rev"><input id="reveal" type="checkbox"> Show passwords</label>
<small>Typed on a phone, standing next to the board. A mistyped Wi-Fi
password is the one error this page cannot catch: the document is valid, the
board accepts it and reboots, and only the provisioning marker keeps it from
being unreachable.</small>

<label for="host">Hostname</label>
<input id="host" name="hostname" autocapitalize="off" autocorrect="off"
 spellcheck="false">
<small>Optional. Used for mDNS: &lt;hostname&gt;.local</small>

<button id="go">Save and restart</button>
</form>
<div id="msg" hidden></div>
<script>
var msg=document.getElementById('msg');
function show(t,cls){msg.textContent=t;msg.className=cls;msg.hidden=false;}
document.getElementById('reveal').onchange=function(){
  var t=this.checked?'text':'password';
  document.getElementById('pw').type=t;
  document.getElementById('apw').type=t;
};
fetch('/onboarding.json').then(function(r){return r.json();}).then(function(d){
  document.getElementById('sub').textContent =
    'device '+d.id+' | '+d.chip+' | firmware '+d.firmware+' | '+d.reason;
  var sel=document.getElementById('tpl'), sum=document.getElementById('tplsum');
  d.templates.forEach(function(t){
    var o=document.createElement('option');o.value=t.id;o.textContent=t.name;
    o.dataset.sum=t.summary;sel.appendChild(o);});
  function upd(){var o=sel.options[sel.selectedIndex];
    sum.textContent=o?o.dataset.sum:'';}
  sel.addEventListener('change',upd);upd();
  if(d.hostname){document.getElementById('host').value=d.hostname;}
}).catch(function(e){show('Could not read /onboarding.json: '+e,'err');});
document.getElementById('f').addEventListener('submit',function(ev){
  ev.preventDefault();
  var b=document.getElementById('go');b.disabled=true;
  var body=new URLSearchParams(new FormData(ev.target)).toString();
  fetch('/onboarding',{method:'POST',body:body,
    headers:{'Content-Type':'application/x-www-form-urlencoded'}})
  .then(function(r){return r.text().then(function(t){return [r.ok,t];});})
  .then(function(p){
    if(!p[0]){show(p[1],'err');b.disabled=false;return;}
    show('Saved. The device is restarting and will try to join the network.\n'
      +'This access point disappears in a few seconds. If it comes back, the '
      +'credentials did not work — reconnect and try again.','ok');
  }).catch(function(e){show('Request failed: '+e,'err');b.disabled=false;});
});
</script></body></html>
)PAGE";

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

static void
handlePage(AsyncWebServerRequest* request)
{
    // The String overload copies ~4 KB. On the board this runs on that is an
    // idle heap with ~200 KB free serving one client, which is nothing like the
    // parallel-asset load that made /history.json stream — see web.cpp.
    request->send(200, "text/html", kPage);
}

static void
handleInfo(AsyncWebServerRequest* request)
{
    // Hand-built rather than JSONVar: every string below is a compiled literal
    // or a hex number, so there is nothing to escape, and the payload is read
    // exactly once per portal visit.
    String out;
    out.reserve(768);
    out += "{\"id\":\"";
    out += String(config.deviceId, 16);
    out += "\",\"chip\":\"";
    out += kFamilyName;
    out += "\",\"firmware\":\"" FW_VERSION "\",\"ap\":\"";
    out += onboardingApSsid();
    out += "\",\"hostname\":\"";
    out += onboardingApSsid();
    out += "\",\"reason\":\"";
    out += onboarding::reasonText(g_reason);
    out += "\",\"templates\":[";
    for (unsigned i = 0; i < board::kCount; ++i) {
        if (i) {
            out += ",";
        }
        out += "{\"id\":\"";
        out += board::kTemplates[i].id;
        out += "\",\"name\":\"";
        out += board::kTemplates[i].name;
        out += "\",\"summary\":\"";
        out += board::kTemplates[i].summary;
        out += "\"}";
    }
    out += "]}";

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", out);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

static String
formValue(AsyncWebServerRequest* request, const char* name)
{
    if (!request->hasParam(name, true)) {
        return String();
    }
    return request->getParam(name, true)->value();
}

static void
refuse(AsyncWebServerRequest* request, int code, const String& why)
{
    logger.warning("Onboarding refused: " + why);
    request->send(code, "text/plain", why);
}

// Every check loadFile() will run at the next boot, run here instead — because
// the alternative is the user submitting, the board rebooting, the portal
// coming back and nothing saying why. Same reason documentPinsAreUsable()
// exists for POST /config.json: boot is too late, the document is already on
// flash.
//
// The Python twin is refusals() in scripts/provision_config.py. The two are
// separate implementations of the same list on purpose; the pairing table they
// share is onboarding::expectedCaFor(), which test_onboarding pins.
static bool
mergedDocumentIsUsable(JSONVar& doc, String& problem)
{
    if (!configDocumentIsUsable(doc, problem)) {
        return false;
    }

    // The id check handleConfigPost does. Unreachable through the form — the
    // handler sets the field itself — but a template shipping a stale "1a2b"
    // that somehow survived the assignment would be written, refused at boot,
    // and leave the board on compiled defaults it cannot associate with.
    JSONVar idNode = doc["id"];
    const String id = (const char*)idNode;
    char* end;
    if (strtol(id.c_str(), &end, 16) != (long)config.deviceId) {
        problem = "the document's id '" + id + "' is not this chip's";
        return false;
    }

    JSONVar mqtt = doc["mqtt"];
    if (JSON.typeof(mqtt) != "object") {
        problem = "the template has no mqtt block";
        return false;
    }
    const bool useTls =
      mqtt.hasOwnProperty("useTLS") ? (bool)mqtt["useTLS"] : true;
    if (!useTls) {
        // A self-hosted broker on 1883 has no certificate to pin, and
        // mqttSetup() never opens the file.
        return true;
    }

    JSONVar backendNode = mqtt["backend"];
    JSONVar cacertNode = mqtt["cacert"];
    const String backend = (const char*)backendNode;
    const String cacert = (const char*)cacertNode;
    const char* expected = onboarding::expectedCaFor(backend.c_str());
    if (expected == nullptr) {
        problem = "mqtt.backend '" + backend +
                  "' is neither thingsboard nor thingspeak";
        return false;
    }
    if (cacert != expected) {
        // Crossing them fails TLS SILENTLY: every connect returns -9984 while
        // the dashboard keeps reporting MQTT enabled. Channel 1348790 received
        // nothing for three years on exactly this.
        problem = "mqtt.backend is '" + backend + "' but mqtt.cacert is '" +
                  cacert + "'; it should be '" + String(expected) + "'";
        return false;
    }
    return true;
}

static void
handleSubmit(AsyncWebServerRequest* request)
{
    const String templateId = formValue(request, "template");
    const String ssid = formValue(request, "ssid");
    const String wifiPassword = formValue(request, "password");
    const String username = formValue(request, "username");
    const String adminPassword = formValue(request, "adminPassword");
    const String hostname = formValue(request, "hostname");

    // The four fields the user typed get their own messages. Everything else
    // falls through to mergedDocumentIsUsable(), whose `problem` names a JSON
    // path — right for a template fault, useless as feedback on a form.
    const unsigned minChar = g_configMinStringLength;
    if (ssid.length() < minChar) {
        refuse(request, 400,
               "The Wi-Fi network name must be at least " + String(minChar) +
                 " characters.");
        return;
    }
    if (wifiPassword.length() < minChar) {
        // Not a policy this handler invented: loadFile() rejects the whole
        // document when any of its seven strings is shorter, so an open network
        // cannot be joined by this firmware at all. Saying so here is better
        // than a refusal at the next boot on a board nobody can reach.
        refuse(request, 400,
               "The Wi-Fi password must be at least " + String(minChar) +
                 " characters; this firmware cannot join an open network.");
        return;
    }
    if (username.length() < minChar || adminPassword.length() < minChar) {
        refuse(request, 400,
               "The admin user and password must each be at least " +
                 String(minChar) + " characters.");
        return;
    }
    if (hostname.length() > 0 && hostname.length() < minChar) {
        refuse(request, 400,
               "The hostname must be at least " + String(minChar) +
                 " characters, or left empty.");
        return;
    }

    const onboarding_templates::Template* chosen = nullptr;
    for (unsigned i = 0; i < board::kCount; ++i) {
        if (templateId == board::kTemplates[i].id) {
            chosen = &board::kTemplates[i];
            break;
        }
    }
    if (chosen == nullptr) {
        refuse(request, 400,
               "Unknown board template '" + templateId + "' for a " +
                 String(kFamilyName) + " build.");
        return;
    }

    JSONVar doc = JSON.parse(chosen->json);
    if (JSON.typeof(doc) != "object") {
        // A typo in a compiled string literal. test_onboarding cannot parse
        // JSON, so this is the only thing that would ever catch it.
        refuse(request, 500,
               "The compiled template '" + templateId + "' does not parse.");
        return;
    }

    // The six holes, and only these six. The id is the field that bricks a
    // board when it is wrong, and this is the whole reason onboarding removes
    // that failure mode: the firmware knows ESP.getEfuseMac() % 0x10000 and
    // writes it itself, so nobody has to read it off a boot line.
    const String id = String(config.deviceId, 16);
    doc["id"] = id.c_str();
    if (hostname.length() > 0) {
        doc["hostname"] = hostname.c_str();
    }
    // Written through parent[key][...], never by returning a JSONVar by value:
    // operator[] returns BY VALUE in this library and a chained read of the
    // result is a freed buffer. That cost a live incident here once.
    doc["wifi"]["ssid"] = ssid.c_str();
    doc["wifi"]["password"] = wifiPassword.c_str();
    doc["ota"]["username"] = username.c_str();
    doc["ota"]["password"] = adminPassword.c_str();

    String problem;
    if (!mergedDocumentIsUsable(doc, problem)) {
        refuse(request, 400, "Refused before writing anything: " + problem);
        return;
    }

    if (!config.saveFile(JSON.stringify(doc))) {
        refuse(request, 500, "Failed to write /config.json.");
        return;
    }

    // AFTER the config, so a board that loses power between the two comes up
    // with the new document and no marker: it simply behaves like any
    // configured board, and if the credentials are wrong it is unreachable —
    // the failure this feature exists to remove, but not a NEW one. The other
    // order would leave a marker with the old config, which is worse: a working
    // board that spends 60 s probing at every boot.
    const bool marker = markerWrite();
    if (!marker) {
        logger.error("Could not write " + String(onboarding::kMarkerPath) +
                     "; if these credentials do not associate, this board "
                     "will not come back to the portal.");
    }

#if USE_CUSTOM_LOGIN
    // The login password lives in /users.json, not /config.json. Without this
    // the account only appears at the NEXT boot, and only when /users.json is
    // absent — UserStore::load() migrates ota.* just once, and a board reaching
    // arm 2 has a populated store already.
    //
    // upsert() rather than setPassword(): this IS the create, and ADMIN is the
    // same seeding rule UserStore::load() applies to an empty store. It is also
    // a real promotion door — whoever is in radio range during the portal
    // window gets an admin account on this device. That is inherent in an
    // unauthenticated setup endpoint, not something this line adds.
    userStore.upsert(username, adminPassword, Role::ADMIN);
    if (!userStore.save()) {
        logger.error("Saved /config.json but not /users.json; the account "
                     "will be migrated from ota.* at the next boot instead.");
    }
#endif

    logger.warning("Onboarding wrote /config.json for template '" + templateId +
                   "' (ssid '" + ssid + "', admin '" + username +
                   "'). Restarting.");

    AsyncWebServerResponse* response = request->beginResponse(
      200,
      "application/json",
      String("{\"saved\":true,\"restarting\":true,\"marker\":") +
        (marker ? "true" : "false") + ",\"ssid\":\"" + ssid + "\"}");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);

    // NEVER ESP.restart() here. request->send() only QUEUES the response and
    // the async_tcp task that flushes it is the one this handler runs on, so a
    // reboot in place guarantees the phone sees a reset instead of the
    // confirmation. 3 s rather than /control's 500 ms: the client is on the AP
    // that is about to disappear, and there is no second chance to tell it
    // anything.
    requestRestart(3000);
}

// A captive portal is the difference between "connect and the page opens" and
// "connect, then find out the device is on 192.168.4.1". Everything the phone
// probes — /generate_204, /hotspot-detect.html, /ncsi.txt — resolves here
// because the DNS below answers every name with the AP's own address, and a
// 302 is what turns that into the sign-in sheet the OS pops up.
static void
handleCaptive(AsyncWebServerRequest* request)
{
    AsyncWebServerResponse* response = request->beginResponse(302);
    // Kept from a diagnostic session. Without it the next line would
    // dereference null on a heap too tired to build a 302, and a panic on
    // the portal is a board nobody can configure.
    if (!response) {
        logger.error("portal: no heap for a redirect response.");
        return;
    }
    response->addHeader("Location",
                        "http://" + WiFi.softAPIP().toString() + "/");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void
onboardingBegin(AsyncWebServer& server, onboarding::Reason reason)
{
    // Set BEFORE the radio is touched: web.cpp's disconnect handler reads it,
    // and WiFi.disconnect(true) below raises exactly that event. Without this
    // the handler would call WiFi.begin() and bring the STA back up underneath
    // the AP.
    g_active = true;
    g_reason = reason;

    const String ssid = onboardingApSsid();

    logger.warning("SETUP PORTAL: " + String(onboarding::reasonText(reason)));
    logger.warning("Raising AP '" + ssid + "' with the well-known password '" +
                   String(kApPassword) +
                   "'. Until this device joins a network, anyone in radio "
                   "range can write its configuration.");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(ssid.c_str(), kApPassword)) {
        logger.fatal("softAP() failed; the setup portal has no radio and this "
                     "device cannot be reached at all.");
    }

    const IPAddress ip = WiFi.softAPIP();
    logger.warning("Portal: http://" + ip.toString() + "/  (or http://" + ssid +
                   ".local/)");

    // Mirrors the SSID so the two names an operator might type are the same
    // name. mDNS on a soft AP works for the clients associated to it.
    if (MDNS.begin(ssid.c_str())) {
        MDNS.addService("http", "tcp", 80);
    } else {
        logger.warning("mDNS did not start; use the IP address.");
    }

    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dnsUp = g_dns.start(53, "*", ip);
    if (!g_dnsUp) {
        logger.warning("Captive-portal DNS did not start; browse to the IP.");
    }

    // THE WHOLE ROUTE TABLE. Three routes and a catch-all: no /data.json, no
    // /control, no /config.json, no /spiffs, no /update, no login. There is
    // nothing here to authenticate against — UserStore has no account on a
    // board whose config never loaded — and nothing here that a token would
    // protect that the AP password does not.
    server.on("/", HTTP_GET, handlePage);
    server.on("/onboarding.json", HTTP_GET, handleInfo);
    server.on("/onboarding", HTTP_POST, handleSubmit);
    server.onNotFound(handleCaptive);
}

void
onboardingLoop()
{
    if (g_active && g_dnsUp) {
        g_dns.processNextRequest();
    }
}
