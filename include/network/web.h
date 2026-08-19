#pragma once

extern bool g_wifiConnected;
extern bool g_hasNetwork;

void
webSetup();

// Rebuild the /data.json payload. Called from a scheduler task, i.e. from
// loop(), because the accumulators it reads are mutated from there: building it
// inside the request handler walks those std::lists on the async_tcp task while
// the io task is pushing into them.
void
webUpdateDataCache();