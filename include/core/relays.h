#pragma once

#include "BuildConfig.h"
#include <Arduino.h>

struct RelayState
{
    bool on;
    unsigned long startTime;
    unsigned long duration;
};

// The relay task runs on its own FreeRTOS task while startRelay() is called
// from loop() (TalkBack) and from async_tcp (the /control handler), so every
// read-modify-write of g_relay goes through this spinlock.
extern portMUX_TYPE g_relayMux;

// Sticky record of which relays fired since the last history append. A history
// record samples state once per historyPeriodSec (60 s by default) while a
// watering defaults to 5 s and is capped at 30 — so an instantaneous sample
// misses roughly nine activations in ten, and the moisture rise that follows
// appears in the file with no cause.
//
// EVERY access takes g_relayMux, producers included. `|=` is a
// read-modify-write, and the producers run on the critical runner and on
// async_tcp while the consumer clears from loop(): guarding only the consumer
// lets a producer store a stale word back over the clear, or the clear discard
// an activation that arrived mid-sequence. portENTER_CRITICAL masks the current
// core, so it excludes nothing unless both sides take it.
// Which relays fired since each consumer last looked.
//
// One mask per consumer, deliberately. It is a TAKE-AND-CLEAR flag, so a single
// shared mask means whichever reader gets there first steals the event from the
// other — the history record would silently lose every watering the telemetry
// happened to report first.
enum RelayStickyConsumer
{
    RELAY_STICKY_HISTORY = 0,
    RELAY_STICKY_TELEMETRY = 1,
    RELAY_STICKY_CONSUMERS = 2
};

// Reads and clears one consumer's mask. Safe from any task.
uint16_t
relayStickyTake(RelayStickyConsumer consumer);

// One pending start/stop per relay, recorded by whoever switches it and drained
// by the io task. The critical runner sets a bit and nothing more: building a
// String or touching a queue from there is how a 50 ms deadline becomes a
// watchdog reset.
struct RelayPendingEvent
{
    bool started;
    bool ended;
    bool refused;
    unsigned duration; // ms, from the start that set it
    String reason;     // why it was refused, empty otherwise
};

// Moves the pending events out under the lock and returns them. Publishing is
// the caller's job, and must happen outside it.
void
relayTakePendingEvents(RelayPendingEvent out[RELAY_MAX]);

// Longest a relay may stay energised in one activation. Every path to a pump
// — the web UI, TalkBack, a schedule, a ThingsBoard RPC — goes through
// startRelay() and inherits this ceiling.
extern const unsigned g_relayMaxTime;

extern const unsigned int g_wateringDefaultTime;
extern unsigned g_wateringCycles;

// Duration of the last watering, pending publication. ThingSpeak carries it in
// field 2 through g_mqttMessage; ThingsBoard reads it here, since its payload
// is rebuilt from scratch each period.
extern unsigned g_pendingWateringMs;

// Relay pin setup that is not covered by relayPinsSafeInit(). Called from
// tasksSetup().
void
relaysSetup();

// Body of the relays task: switches every relay off once its timer expires and
// keeps the sticky mask up to date. Runs on the critical runner.
void
relaysTick();

// Energise relay `index` for `duration` ms. Rejects an out-of-range index, a
// zero duration and anything above g_relayMaxTime. A relay already running is
// left alone rather than retriggered.
//
// Returns whether the relay was actually started, so a caller answering a
// remote command can say why nothing happened instead of reporting success.
bool
startRelay(unsigned index, unsigned int duration);

// Switches relay `index` off early. Returns whether it was running.
bool
stopRelay(unsigned index);

bool
relayIsOn(unsigned index);

// Remaining on-time in ms, 0 when the relay is idle.
unsigned long
relayRemaining(unsigned index);

// Relay 0 is the watering relay on every board.
void
startWatering(unsigned int wateringTime = g_wateringDefaultTime);

// The mirror of relayStartedHook: a veto asked BEFORE the relay is energised,
// also implemented in tasks.cpp. Returns false and fills `reason` to refuse,
// so every caller — the web UI, TalkBack, a schedule, a ThingsBoard RPC — gets
// the same answer through the same path. Relay switching itself knows nothing
// about reservoirs.
bool
relayStartAllowed(unsigned index, String& reason);

// The other seam between this module and the rest of the firmware. startRelay()
// calls it right after the relay is energised; it is implemented in tasks.cpp,
// which owns the watering bookkeeping (telemetry field 2, the pre-watering
// moisture snapshot and the checkMoisture task) that relay switching itself
// knows nothing about.
void
relayStartedHook(unsigned index, unsigned int duration);

// Records a refusal for the io task to report. Called from whichever thread
// asked for the relay — including async_tcp — so it does nothing but store.
void
relayRecordRefusal(unsigned index, const String& reason);
