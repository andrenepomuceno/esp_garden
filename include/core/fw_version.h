#pragma once

// Semantic-version comparison, split out of the ThingsBoard FOTA path so it can
// be tested on the host: it is the check that decides whether a firmware image
// pushed from the cloud gets flashed, and getting it wrong either bricks a
// working device or silently ignores every update.
//
// Plain C strings and no Arduino types, so [env:native] can build it.

// Parses up to three dot-separated numbers into out[3]. Missing components read
// as 0, so "2" == "2.0" == "2.0.0". Anything non-numeric in a component stops
// that component (strtol semantics), and a negative number is clamped to 0 —
// version strings do not have signs, and letting one through would make "1.-1.0"
// sort before "1.0.0".
void
fwVersionParse(const char* version, int out[3]);

// -1 / 0 / +1, comparing major, then minor, then patch.
int
fwVersionCompare(const char* a, const char* b);

// True when `offered` is worth flashing over `current`: any difference, in
// either direction. A downgrade is a legitimate rollback — the cloud operator
// assigned that image on purpose — so this is deliberately not `>`.
bool
fwVersionDiffers(const char* offered, const char* current);
