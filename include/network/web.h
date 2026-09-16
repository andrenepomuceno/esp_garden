#pragma once

extern bool g_wifiConnected;
extern bool g_hasNetwork;

// `configLoaded` is loadConfigFile()'s own answer, passed rather than looked up
// because it is the first arm of the onboarding decision and that decision
// lives in exactly one place — the top of webSetup(). A global would let a
// second reader form a second opinion; see include/core/onboarding.h.
void
webSetup(bool configLoaded);

// Rebuild the /data.json payload. Called from a scheduler task, i.e. from
// loop(), because the accumulators it reads are mutated from there: building it
// inside the request handler walks those std::lists on the async_tcp task while
// the io task is pushing into them.
void
webUpdateDataCache();