#include "core/relays.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/logger.h"

const unsigned int g_wateringDefaultTime = 5 * 1000;
const unsigned g_relayMaxTime = 30 * 1000;

unsigned g_pendingWateringMs = 0;

#if USE_WATERING_PWM
static const unsigned g_wateringPWMChannel = 0;
static const unsigned g_wateringPWMTime = 2 * 1000;
#endif

static RelayState g_relay[RELAY_MAX] = {};

portMUX_TYPE g_relayMux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t g_relaySticky[RELAY_STICKY_CONSUMERS] = { 0, 0 };
static RelayPendingEvent g_relayPending[RELAY_MAX] = {};

// Marks a relay as having fired, for every consumer at once. Called with
// g_relayMux already held.
static void
relayMarkSticky(unsigned index)
{
    for (unsigned c = 0; c < RELAY_STICKY_CONSUMERS; ++c) {
        g_relaySticky[c] |= (uint16_t)(1u << index);
    }
}

uint16_t
relayStickyTake(RelayStickyConsumer consumer)
{
    if (consumer >= RELAY_STICKY_CONSUMERS) {
        return 0;
    }
    portENTER_CRITICAL(&g_relayMux);
    const uint16_t mask = g_relaySticky[consumer];
    g_relaySticky[consumer] = 0;
    portEXIT_CRITICAL(&g_relayMux);
    return mask;
}

void
relayRecordRefusal(unsigned index, const String& reason)
{
    if (index >= RELAY_MAX) {
        return;
    }
    // The reason is a String, so it is stored OUTSIDE the spinlock — assigning
    // one allocates, and allocating with interrupts disabled is what panicked
    // this board once. The flag under the lock is what makes it visible.
    g_relayPending[index].reason = reason;

    portENTER_CRITICAL(&g_relayMux);
    g_relayPending[index].refused = true;
    portEXIT_CRITICAL(&g_relayMux);
}

void
relayTakePendingEvents(RelayPendingEvent out[RELAY_MAX])
{
    // The String is copied outside the lock for the same reason it is written
    // outside it. Only the flags need to be atomic with respect to the
    // producers; a reason belongs to the flag that was set with it.
    for (unsigned i = 0; i < RELAY_MAX; ++i) {
        out[i].reason = g_relayPending[i].reason;
    }

    portENTER_CRITICAL(&g_relayMux);
    for (unsigned i = 0; i < RELAY_MAX; ++i) {
        out[i].started = g_relayPending[i].started;
        out[i].ended = g_relayPending[i].ended;
        out[i].refused = g_relayPending[i].refused;
        out[i].duration = g_relayPending[i].duration;
        g_relayPending[i].started = false;
        g_relayPending[i].ended = false;
        g_relayPending[i].refused = false;
    }
    portEXIT_CRITICAL(&g_relayMux);
}

unsigned g_wateringCycles = 0;

static void
relayWrite(unsigned index, bool on)
{
    // A declared relay with no "pin" key keeps the sentinel, and the config
    // editor produces exactly that for a row left blank. Without this check
    // the index bound passes, startRelay() reports success and the dashboard
    // counts down a relay while digitalWrite(255, ...) does nothing.
    if (config.relayPin[index] == ConfigFile::kNoPin) {
        return;
    }

    const uint8_t level = on ? config.relayPinOn[index] : !config.relayPinOn[index];

#if USE_WATERING_PWM
    if (index == 0) {
        ledcWrite(g_wateringPWMChannel, level ? 1023 : 0);
        return;
    }
#endif

    digitalWrite(config.relayPin[index], level);
}

void
relaysSetup()
{
    // Relay pins were already parked by relayPinsSafeInit() before and after
    // the config load; this only covers the PWM variant's channel setup.
#if USE_WATERING_PWM
    ledcAttachPin(config.relayPin[0], g_wateringPWMChannel);
    ledcSetup(g_wateringPWMChannel, 10e3, 10);
    ledcWrite(g_wateringPWMChannel, 0);
#endif
}

void
relaysTick()
{
    for (unsigned i = 0; i < config.relayCount; ++i) {
        bool expired = false;

        portENTER_CRITICAL(&g_relayMux);
        if (g_relay[i].on &&
            (millis() - g_relay[i].startTime >= g_relay[i].duration)) {
            g_relay[i].on = false;
            expired = true;
            // A bit, and nothing more. This runs on the critical runner with a
            // 50 ms deadline; building the message here is how that deadline
            // becomes a watchdog reset. The io task turns it into telemetry a
            // second later.
            g_relayPending[i].ended = true;
        }
        portEXIT_CRITICAL(&g_relayMux);

        if (expired) {
            // Re-checked under the lock, because startRelay() can land in the
            // gap between the decision above and this write: it would see
            // on == false, energise the pin and arm a new timer, and then this
            // write would switch the pump off while g_relay[i].on stayed true.
            // The dashboard would count down a relay that is not running, the
            // sticky mask would record a watering that delivered nothing, and
            // the OTA guard would refuse for its whole phantom duration.
            bool stillOff = false;
            portENTER_CRITICAL(&g_relayMux);
            stillOff = !g_relay[i].on;
            portEXIT_CRITICAL(&g_relayMux);

            if (stillOff) {
                relayWrite(i, false);
            }
        }

        if (relayIsOn(i)) {
            portENTER_CRITICAL(&g_relayMux);
            relayMarkSticky(i);
            portEXIT_CRITICAL(&g_relayMux);
        }
    }
}

bool
relayIsOn(unsigned index)
{
    if (index >= config.relayCount) {
        return false;
    }

    portENTER_CRITICAL(&g_relayMux);
    const bool on = g_relay[index].on;
    portEXIT_CRITICAL(&g_relayMux);

    return on;
}

unsigned long
relayRemaining(unsigned index)
{
    if (index >= config.relayCount) {
        return 0;
    }

    portENTER_CRITICAL(&g_relayMux);
    const RelayState state = g_relay[index];
    portEXIT_CRITICAL(&g_relayMux);

    if (!state.on) {
        return 0;
    }

    const unsigned long elapsed = millis() - state.startTime;
    return (elapsed >= state.duration) ? 0 : (state.duration - elapsed);
}

bool
startRelay(unsigned index, unsigned int duration)
{
    if (index >= config.relayCount) {
        logger.error("Invalid relay index: " + String(index));
        return false;
    }

    if (config.relayPin[index] == ConfigFile::kNoPin) {
        logger.error(config.relayName[index] +
                     " has no pin assigned; refusing to start it.");
        return false;
    }

    if ((duration == 0) || (duration > g_relayMaxTime)) {
        logger.error("Invalid relay time: " + String(duration));
        return false;
    }

    // Asked before the relay is claimed, not after: a refusal must not leave
    // the relay marked on. The check is deliberately in startRelay() rather
    // than at each call site, because there are five of them and the one that
    // gets forgotten is the one that runs a pump dry.
    String veto;
    if (!relayStartAllowed(index, veto)) {
        logger.warning(config.relayName[index] + " refused: " + veto);
        relayRecordRefusal(index, veto);
        return false;
    }

    bool started = false;

    portENTER_CRITICAL(&g_relayMux);
    if (!g_relay[index].on) {
        g_relay[index].on = true;
        g_relay[index].startTime = millis();
        g_relay[index].duration = duration;
        started = true;
    }
    portEXIT_CRITICAL(&g_relayMux);

    if (!started) {
        logger.warning(config.relayName[index] + " already active.");
        return false;
    }

    logger.info("Starting " + config.relayName[index] + " for " +
                String(duration) + " ms");
    portENTER_CRITICAL(&g_relayMux);
    relayMarkSticky(index);
    g_relayPending[index].started = true;
    g_relayPending[index].duration = duration;
    portEXIT_CRITICAL(&g_relayMux);
    relayWrite(index, true);

    // Everything that has to happen *because* a relay started, but that this
    // module does not own: implemented in tasks.cpp.
    relayStartedHook(index, duration);

    return true;
}

// Switches a relay off before its timer expires. Returns whether it was
// actually running, which is what an operator needs to tell "stopped it" from
// "it had already finished" — the relays task would have switched it off
// within 50 ms anyway, so a stop that arrives late is not an error.
bool
stopRelay(unsigned index)
{
    if (index >= config.relayCount) {
        logger.error("Invalid relay index: " + String(index));
        return false;
    }

    bool wasOn = false;

    portENTER_CRITICAL(&g_relayMux);
    if (g_relay[index].on) {
        g_relay[index].on = false;
        wasOn = true;
        g_relayPending[index].ended = true;
    }
    portEXIT_CRITICAL(&g_relayMux);

    // Outside the spinlock on purpose: a digitalWrite is exactly the kind of
    // work that must not run inside portENTER_CRITICAL. Writing it
    // unconditionally also re-parks a relay whose pin somehow drifted.
    relayWrite(index, false);

    if (wasOn) {
        logger.info("Stopped " + config.relayName[index]);
    }

    return wasOn;
}

void
startWatering(unsigned int wateringTime)
{
    startRelay(0, wateringTime);
}
