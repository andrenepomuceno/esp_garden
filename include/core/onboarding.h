#pragma once

// First-boot onboarding: WHEN a board raises its own setup AP, and never a line
// about how. Arduino-free, so `test_onboarding` can hold the decision to its
// truth table — the same reason `pin_rules.h`, `segment_index.h` and
// `step_publisher.h` exist.
//
// The decision is one function because the property that matters is a NEGATIVE
// one, and a negative property spread across three call sites is a property
// nobody can check. A board that switches real pumps on a home network must
// never fall back to an access point whose password is printed in this
// repository — so `decide()` is the only thing in the tree that can answer
// "portal", and it is written so that a reader can see the answer for a
// configured board without following a single branch.
//
// -------------------------------------------------------------------------
// THE TWO ARMS, AND WHY THE SECOND ONE NEEDS A MARKER FILE
// -------------------------------------------------------------------------
//
// ARM 1 — `loadConfigFile()` said no. Missing, unparseable, an `id` from
// another chip, or one of the seven length-checked strings under four
// characters. Unambiguous, and today it is terminal: `main.cpp` continues on
// compiled defaults (`ssid "undefined"`) that cannot associate, so the device
// is unreachable without USB. Raising a portal there costs nothing that was
// not already lost.
//
// ARM 2 — the credentials are WRONG rather than malformed. A mistyped Wi-Fi
// password produces a perfectly valid document: `loadFile()` accepts it, arm 1
// never fires, and the board is off the network for ever with no way in. So
// association failure has to be able to reach the portal too.
//
// And "failed to associate, therefore raise an AP" is exactly the rule that
// must NOT exist, because every router reboot, every channel change and every
// ISP outage would satisfy it — on the live garden, which would then broadcast
// an AP with a publicly known password and an unauthenticated endpoint that
// rewrites its own `/config.json`.
//
// The marker file is what separates the two. `POST /onboarding` writes
// `/config.json` AND `kMarkerPath`. The very first boot that associates
// deletes the marker, and from that moment `markerPresent` is false for the
// life of the board — so `decide()` returns `Normal` whatever the radio does.
// A board that has ever reached the network is therefore outside arm 2
// STRUCTURALLY, not by a timeout, a counter or a heuristic. That is the whole
// design, and `test_onboarding` pins it as a truth table over every input.

#include <stdint.h>
#include <string.h>

namespace onboarding {

// Written beside /config.json by the onboarding handler and deleted by the
// first boot that associates. Spelled here, not in web_onboarding.cpp, because
// `uploadPathIsProtected()` also needs it: an ADMIN able to PUT this file onto
// a configured device would re-arm arm 2 on a board that had left it for ever.
//
// 20 characters, inside FILESYSTEM_MAX_PATH (31).
static const char* const kMarkerPath = "/provisioned.pending";

// How long `webSetup()` waits for an IP before falling to the portal, and it
// is only ever spent on a board carrying the marker — i.e. between onboarding
// and the first successful association, and never again. 60 s is
// `g_bootWaitMaxMs`, the bound `tasksSetup()` already puts on its own two
// waits; the same number rather than a second one to argue about.
static const unsigned long kProbationMs = 60UL * 1000UL;

enum class Mode : uint8_t
{
    Normal, // the route table in web.cpp, authentication and all
    Portal, // the AP, three unauthenticated routes, nothing else
};

enum class Reason : uint8_t
{
    Configured,      // not in the portal
    NoUsableConfig,  // arm 1
    NeverAssociated, // arm 2
};

struct Decision
{
    Mode mode;
    Reason reason;
    // True exactly once in a board's life: the boot that proves the stored
    // credentials work. Deleting the marker is what takes arm 2 away for good.
    bool clearMarker;
};

// Whether association even has to be OBSERVED. A configured board with no
// marker never waits for one, so the probation costs a normal boot nothing and
// cannot delay the live garden by a millisecond.
inline bool
mustProbeAssociation(bool configLoaded, bool markerPresent)
{
    return configLoaded && markerPresent;
}

// `associated` is meaningless unless mustProbeAssociation() said to look; pass
// false when it did not, and note that the first branch below never reads it.
inline Decision
decide(bool configLoaded, bool markerPresent, bool associated)
{
    Decision out;

    if (!configLoaded) {
        out.mode = Mode::Portal;
        out.reason = Reason::NoUsableConfig;
        out.clearMarker = false;
        return out;
    }

    // THE GUARANTEE. No marker, config loaded: Normal, unconditionally, with
    // `associated` never consulted. This is the branch the live garden takes on
    // every boot.
    if (!markerPresent) {
        out.mode = Mode::Normal;
        out.reason = Reason::Configured;
        out.clearMarker = false;
        return out;
    }

    if (associated) {
        out.mode = Mode::Normal;
        out.reason = Reason::Configured;
        out.clearMarker = true;
        return out;
    }

    out.mode = Mode::Portal;
    out.reason = Reason::NeverAssociated;
    out.clearMarker = false;
    return out;
}

inline const char*
reasonText(Reason reason)
{
    switch (reason) {
        case Reason::NoUsableConfig:
            return "/config.json is missing, unparseable, addressed at another "
                   "chip, or short of a required field";
        case Reason::NeverAssociated:
            return "the credentials written by the last setup have never "
                   "associated";
        default:
            return "configured";
    }
}

// The CA a backend's chain terminates in. Crossing the two fails TLS silently
// — every connect returns -9984 while the dashboard still reports MQTT enabled,
// which is how channel 1348790 received nothing for three years — so a template
// that ships the wrong pairing is a device that never publishes and never says
// why.
//
// The Python twin is BACKEND_CA in scripts/provision_config.py; the two are
// separate implementations on purpose, and `test_onboarding` holds this one to
// its answers. Returns nullptr for a backend neither file knows.
inline const char*
expectedCaFor(const char* backend)
{
    if (backend == nullptr) {
        return nullptr;
    }
    if (strcmp(backend, "thingsboard") == 0) {
        return "/thingsboard.pem";
    }
    if (strcmp(backend, "thingspeak") == 0) {
        return "/thingspeak.pem";
    }
    return nullptr;
}

} // namespace onboarding
