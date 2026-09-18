#include "core/task_events.h"
#include "BuildConfig.h"
#include "core/cloud_model.h"
#include "core/config.h"
#include "core/et0_model.h"
#include "core/logger.h"
#include "core/relays.h"
#include "core/sensors.h"
#include "core/tasks.h"
#include "core/telemetry.h"
#include "network/thingsboard.h"

// The pre-watering baseline, one per probe. Written by relayStartedHook() on
// WHICHEVER thread asked for the relay, read by reportWateringResponse() on the
// checkMoisture task — which is exactly why the value stored is
// moistureReading(), the snapshot the io task publishes under a spinlock, and
// never the accumulator itself.
//
// It is private to this file because the hook that fills it and the check that
// reads it are the two halves of one piece of bookkeeping. In tasks.cpp they sat
// on opposite sides of nine task handlers with the array visible to all of them.
static float g_moistureBeforeWatering[MOISTURE_MAX] = { 0.0 };

// Seam with src/relays.cpp: the reservoir interlock. Lives here rather than in
// relays.cpp because it couples a relay to a SENSOR, and relays.cpp is the one
// module that must stay about switching.
bool
relayStartAllowed(unsigned index, String& reason)
{
    // loadFile() clears floatInterlock when no float switch is declared, so a
    // sensor removed in /devices.html cannot leave a veto behind that refuses
    // every watering on a reading nothing produces.
    if (!config.floatInterlock) {
        return true;
    }

    // The refill relay is the remedy for an empty reservoir. Blocking it would
    // mean an empty tank could never be filled — the interlock deadlocking the
    // system it exists to protect.
    if ((int)index == config.floatFillRelay) {
        return true;
    }

    if (!floatRaised()) {
        reason = config.floatName + " reads empty";
        return false;
    }

    return true;
}

// Turns the bits the switching code recorded into telemetry, once a second.
//
// The device used to report relays by SAMPLING them into the 1-minute payload,
// so a five-second watering was invisible: the sample landed between the
// events. The history record solved this with a sticky mask long ago and the
// telemetry never got it. Now there are both — an immediate event for the
// transition, and a sticky mask so the periodic payload says "this ran during
// the period" rather than "it happens to be on right now".

// A relay IRRIGATES when some probe names it in `moisture[i].relay`. That is
// the config the classifier already uses to label its training data, so this
// adds no key and no second place to keep in step — and the reservoir pump,
// which no probe names, is excluded for the right reason rather than by index.
//
// It used to be `index != 0`, the single-relay contract from when relay 0 WAS
// the watering relay. On a multi-zone board that silently counted one zone and
// ignored the rest: measured on 6224, a 10 s run of Zona 3 (relay 1) left
// `wateringCycles` at 0 and published no `wateringMs`.
static bool
relayWaters(unsigned index)
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (config.moistureRelay[i] >= 0 &&
            (unsigned)config.moistureRelay[i] == index) {
            return true;
        }
    }
    return false;
}

void
publishRelayEvents()
{
    RelayPendingEvent pending[RELAY_MAX];
    relayTakePendingEvents(pending);

    for (unsigned i = 0; i < config.relayCount; ++i) {
        if (pending[i].refused) {
            JSONVar refusal;
            refusal["relayRefused"] = config.relayName[i];
            refusal["relay"] = (int)i;
            refusal["reason"] = pending[i].reason;
            tbPublishEvent(JSON.stringify(refusal));
        }

        if (!pending[i].started && !pending[i].ended) {
            continue;
        }

        JSONVar event;
        const String key = "relay" + String(i + 1) + "Event";
        // Both flags can be set in the same second — a 500 ms activation
        // starts and finishes between two drains. Reporting "ended" then is
        // right: what the operator needs to know is that it ran and is done.
        event[key.c_str()] = pending[i].started
                               ? (pending[i].ended ? "ran" : "started")
                               : "stopped";
        // No bare `relay` key. It carried a ZERO-based index while every
        // relayN telemetry key is one-based, so a stored record read
        // {relay: "2", relayName: "Reservatorio", relay3Event: "started"} and
        // anything joining the two was off by one. relayNEvent already names
        // the relay at the correct index, which makes the bare key redundant as
        // well as wrong — and dropping it is the fix that does not create a
        // second meaning for a key already in the stored series, which
        // renumbering it would.
        event["relayName"] = config.relayName[i];
        if (pending[i].started) {
            event["durationMs"] = (int)pending[i].duration;
        }
        tbPublishEvent(JSON.stringify(event));

        // The watering bookkeeping relayStartedHook could not do safely from a
        // request handler. All of it touches state only this task may touch.
        //
        // Derived from pending[] rather than from a single shared slot, which
        // is what it used to be: two zones starting inside one 1 Hz drain
        // overwrote each other and one cycle went uncounted. `wateringMs` is
        // still one value and the last zone in the tick wins it — the per-relay
        // truth is in relayNEvent's own durationMs, published just above.
        if (pending[i].started && relayWaters(i)) {
            ++g_wateringCycles;
            g_pendingWateringMs = pending[i].duration;
#if USE_THINGSPEAK
            mqttAddField(g_wateringField, String(pending[i].duration));
#endif
            // Through a named function rather than the task object: every
            // g_*Task in this firmware lives in tasks.cpp, so the file holding
            // the registration order is also the only file that can arm, park
            // or re-period a task. Same statement it always was, one indirection
            // out.
            armMoistureCheck();
        }
    }
}

// The float switch has exactly the relay's problem and a worse consequence: a
// reservoir that runs empty and is refilled between two publishes never
// happened, as far as the cloud is concerned — and "the tank ran dry" is the
// one event an operator most needs to see. So the TRANSITION is published, not
// the level.
void
publishFloatEvents()
{
    if (!config.floatFitted) {
        return;
    }

    static bool known = false;
    static bool lastRaised = false;

    const bool raised = floatRaised();
    if (known && raised == lastRaised) {
        return;
    }

    // The first reading after boot is reported too: an operator coming back to
    // a device needs to know the current state, not only the next change.
    JSONVar event;
    event["reservoirRaised"] = raised;
    event["reservoirEvent"] = known ? (raised ? "refilled" : "emptied")
                                    : "initial reading";
    tbPublishEvent(JSON.stringify(event));

    known = true;
    lastRaised = raised;
}

// A cloud transient lasts seconds to minutes and the periodic payload is built
// once every five minutes, so sampling it would miss most of them entirely --
// the rule this tree wrote down after the telemetry spent months sampling
// relays. An episode is therefore an EVENT: one message when it opens and one
// when it closes, carrying the length nothing else could reconstruct.
//
// The STATE is not here. A sky state sits still for tens of minutes and then
// steps, which is the third mechanism -- publish on change with a heartbeat
// under it -- and it leaves through telemetryPublishStepChanges() with the
// relay states and the reservoir contact.
void
publishCloudEvents(int events)
{
    if ((events & (CLOUD_EVENT_TRANSIENT_BEGAN | CLOUD_EVENT_TRANSIENT_ENDED)) ==
        0) {
        return;
    }

    const CloudReport report = cloudReport();

    JSONVar event;
    event["cloudEvent"] =
      (events & CLOUD_EVENT_TRANSIENT_BEGAN) ? "transient began" : "transient ended";
    event["cloudVariability"] = (double)report.variability;
    if (report.clearness >= 0.0f) {
        event["cloudClearness"] = (double)report.clearness;
    }
    if (events & CLOUD_EVENT_TRANSIENT_ENDED) {
        // Only meaningful at the close: the peak and the length are what an
        // operator reads the episode by, and neither is knowable while it runs.
        event["cloudTransientMin"] = (int)report.transientMinutes;
        event["cloudTransientPeak"] = (double)report.transientPeak;
    }
    tbPublishEvent(JSON.stringify(event));
}

// A day's reference evapotranspiration, published the moment the day closes.
//
// It is an EVENT rather than a step value, and the sampling rule is why: ET0
// updates exactly once every 24 hours, so riding the periodic payload would
// restate the same number 287 times a day, and riding the step publisher would
// re-send it on every 900 s heartbeat for the same nothing. One message a day,
// carrying the whole observation.
//
// The extremes and the coverage go WITH it, in the same message. A daily
// maximum has no averaging in it -- one short excursion owns the day, and this
// board has produced exactly that: a 38-minute patch of direct sun took the
// sensor to 45 C on 2026-09-03 and more than doubled that day's estimate. The
// number alone cannot be re-examined later; the number beside the range it came
// from can. Same reasoning as the `Sd` error bars on the continuous channels.
void
publishEt0Event()
{
    const Et0Report report = et0Report();

    JSONVar event;
    event["et0"] = (double)report.et0Mm;
    event["et0TempMin"] = (double)report.tMin;
    event["et0TempMax"] = (double)report.tMax;
    event["et0Hours"] = (int)report.hoursCovered;
    tbPublishEvent(JSON.stringify(event));
}

// Seam with src/relays.cpp: startRelay() calls this once the relay is
// energised, so relay switching itself stays free of the watering bookkeeping.
void
relayStartedHook(unsigned index, unsigned int duration)
{
    if (!relayWaters(index)) {
        return;
    }

    // This runs on WHICHEVER THREAD asked for the relay — async_tcp for
    // /control, loop() for TalkBack and schedules. So it does exactly one
    // thing that is safe from all of them, and everything else waits for the
    // io task in publishRelayEvents().
    //
    // What used to be here and could not stay:
    //   - g_soilMoisture[i].getAverage(), which walks a list the io task is
    //     writing and updates a shared member. The documented trap, again.
    //   - mqttAddField(), which appends to the global String telemetryPublish()
    //     concurrently reads and clears — an unsynchronised reallocation.
    //   - g_checkMoistureTask.enableDelayed(), which mutates the scheduler's
    //     task list while execute() walks it; the library documents that as
    //     unsafe after start().
    //
    // moistureReading() IS safe from any thread — it is the snapshot the io
    // task publishes under a spinlock — so the pre-watering baseline is taken
    // here rather than deferred, where it would be a second late and a second
    // of watering wrong.
    // Only the probes THIS relay feeds. Resetting every probe's baseline was
    // harmless on a one-relay board, where every probe was in the watered zone;
    // on a multi-zone board it threw away the baseline of a zone that was not
    // watered, so its next rise was measured from the wrong starting point.
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (config.moistureRelay[i] >= 0 &&
            (unsigned)config.moistureRelay[i] == index) {
            g_moistureBeforeWatering[i] = moistureReading(i).average;
        }
    }
    (void)duration; // the count and the duration are taken from pending[]
}

// The other half of the baseline above: armed by publishRelayEvents() through
// armMoistureCheck() and run once, four hours later, by the checkMoisture task.
//
// A probe that did not respond to its own pump is the cheapest evidence that
// something physical is wrong — a disconnected probe, the wrong pot, a pump that
// never actually ran — which is the same thing the classifier's watering
// RESPONSE check reports from the other direction, days later.
//
// The task's own disable() stays in tasks.cpp: this reads the bookkeeping, it
// does not touch the scheduler.
void
reportWateringResponse()
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        float moistureDelta =
          g_soilMoisture[i].getAverage() - g_moistureBeforeWatering[i];

        if (moistureDelta < 0.5) {
            logger.warning("Probe " + String(i) +
                           ": no moisture gain after watering. Delta: " +
                           FLOAT_TO_STRING(moistureDelta));
        }
    }
}
