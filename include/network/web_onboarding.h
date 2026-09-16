#pragma once

// The first-boot setup portal: the soft AP, the three unauthenticated routes
// behind it, and the marker file that keeps a board which has ever reached the
// network out of arm 2 for good.
//
// WHEN any of this happens is decided in core/onboarding.h, which is
// Arduino-free and host-tested. This header is the half that needs a radio, a
// filesystem and a web server.
//
// `onboardingBegin()` has exactly ONE caller — the early return at the top of
// webSetup() — and that is the property worth checking rather than trusting:
// `grep -n onboardingBegin src/` is the whole audit. An endpoint that rewrites
// /config.json without a token must not exist on a configured device, and the
// way to guarantee that is one decision in one place rather than a middleware
// somebody can forget to attach.

#include "core/onboarding.h"

class AsyncWebServer;

// True while this boot is serving the setup portal. Read by tasksSetup(),
// which must not spend two minutes waiting for an internet connection that
// cannot exist, and by web.cpp's disconnect handler, which must not drag the
// STA back up underneath the AP.
bool
onboardingActive();

// Raises the AP, starts the captive-portal DNS and registers the portal's
// routes on `server`. The caller has already decided; this does not re-decide.
void
onboardingBegin(AsyncWebServer& server, onboarding::Reason reason);

// Pumps the captive-portal DNS. Called from tasksLoop(); a no-op otherwise.
void
onboardingLoop();

// Does /provisioned.pending exist? Asked once, in webSetup(), and only on a
// board whose config loaded.
bool
onboardingMarkerPresent();

// Deletes it. Called on the first boot that associates, which is what takes
// arm 2 away for the life of the board.
void
onboardingMarkerClear();
