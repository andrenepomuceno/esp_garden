#include "core/probe_health.h"
#include "core/sensors.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/moisture_model.h"
#include "core/tasks.h"
#include <new>
#include <string.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>

#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

// Constructed in tasksSetup(), not at static-init time: DHT_Unified copies the
// pin in its constructor, and at static-init config.json has not been read yet,
// so a file-scope instance permanently runs on the compiled default pin.
alignas(DHT_Unified) static uint8_t g_dhtStorage[sizeof(DHT_Unified)];
static DHT_Unified* g_dht = nullptr;
AccumulatorV2 g_temperature(g_mqttTaskPeriod / g_dhtTaskPeriod);
AccumulatorV2 g_airHumidity(g_mqttTaskPeriod / g_dhtTaskPeriod);
unsigned g_dhtReadErrors = 0;
unsigned g_dhtTotalReads = 0;

AccumulatorV2 g_soilMoisture[MOISTURE_MAX];

AccumulatorV2 g_luminosity(g_mqttTaskPeriod / g_ioTaskPeriod);

#define ADC_TO_WATER_LEVEL(v) (9.0 - 12.0 * sin(4.04 - 1.61 * (3.3 * v / 4095.0)))
AccumulatorV2 g_waterLevel(g_mqttTaskPeriod / g_ioTaskPeriod);

// The ISR does nothing but count. Everything else — rate, totals, logging —
// happens in the io task, because an ISR that allocates or takes a lock is how
// an ESP32 ends up in a reset loop.
static volatile uint32_t g_flowPulses = 0;
static portMUX_TYPE g_flowMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR
flowPulseISR()
{
    portENTER_CRITICAL_ISR(&g_flowMux);
    ++g_flowPulses;
    portEXIT_CRITICAL_ISR(&g_flowMux);
}

AccumulatorV2 g_flowRate(g_mqttTaskPeriod / g_ioTaskPeriod);
// Cumulative volume since boot, in litres. Not an accumulator: a running total
// has no window.
static double g_flowTotalLitres = 0.0;

// A contact, not a level: one bit, debounced by requiring two agreeing reads.
static bool g_floatRaised = false;

// Published by the io task, read by anyone. A spinlock rather than a mutex
// because both sides are microseconds and one of them is a request handler.
static portMUX_TYPE g_moistureSnapshotMux = portMUX_INITIALIZER_UNLOCKED;
static MoistureReading g_moistureSnapshot[MOISTURE_MAX] = {};

// Owned by the io task; only the report below crosses to another thread.
static ProbeHealth g_probeHealth[MOISTURE_MAX];
static ProbeHealthReport g_probeHealthSnapshot[MOISTURE_MAX] = {};

// The raw count of whatever channel was converted last, which is the step the
// next probe's first conversion has to recover from.
static int g_lastAdcRaw = 0;

// Enough readings to regress on before a verdict, and how the evidence ages.
// At the 1 Hz io task a window of 600 with a half-life every 600 keeps roughly
// twenty minutes of it in hand: long enough to be sure, short enough that a
// probe plugged back in clears its own accusation while somebody is still
// standing at the pot.
static const uint32_t g_probeHealthMinSamples = 120;
static const uint32_t g_probeHealthWindow = 600;
static const double g_probeHealthDecay = 0.5;

// The effect size and the confidence a verdict needs.
//
// PROVISIONAL, and deliberately loose. The null hypothesis is a number — a
// stiff source couples 0 % of the previous channel — so no healthy baseline is
// needed to run the test, but choosing where to draw the line does want one,
// and every probe on this board is currently disconnected. 5 % coupling at
// t >= 5 will not fire on a real sensor; it may under-report a marginal one.
// Tighten it once a probe known to be connected has published a slope.
static const double g_probeHealthMinSlope = 0.05;
static const double g_probeHealthMinT = 5.0;

// Sample-to-sample spread, in ADC counts, above which this stops being soil.
//
// Both sides are measured on this board, and the margin is narrower than the
// first reading of it suggested. A connected channel is not always quiet: the
// luminosity input sits at 6 counts at midday but reached 164 while the light
// was actually changing. The three unplugged probes hold 1113-1773.
//
//   worst CONNECTED observed     164 counts
//   threshold                    400 counts   (2.4x above, 2.8x below)
//   lowest FLOATING observed    1113 counts
//
// 400 is close to the geometric mean of those two extremes (428), which is
// where a one-sided threshold belongs when both sides are known. Soil moisture
// cannot change like sunset light, so the real headroom for a probe is larger
// than the luminosity figure implies — but the figure is what was measured,
// and it is the one quoted.
static const double g_probeHealthMaxSd = 400.0;

ProbeHealthReport
probeHealthReport(unsigned index)
{
    ProbeHealthReport out = { 0, 0.0f, 0.0f, 0 };
    if (index >= MOISTURE_MAX) {
        return out;
    }
    portENTER_CRITICAL(&g_moistureSnapshotMux);
    out = g_probeHealthSnapshot[index];
    portEXIT_CRITICAL(&g_moistureSnapshotMux);
    return out;
}

MoistureReading
moistureReading(unsigned index)
{
    MoistureReading out = { 0.0f, 0 };
    if (index >= MOISTURE_MAX) {
        return out;
    }
    portENTER_CRITICAL(&g_moistureSnapshotMux);
    out = g_moistureSnapshot[index];
    portEXIT_CRITICAL(&g_moistureSnapshotMux);
    return out;
}

String
moistureState(unsigned index)
{
    // Past the fitted count is a probe that does not exist, not one that is
    // merely uncalibrated — both return "", but for different reasons.
    if (index >= config.moistureCount) {
        return String();
    }

    // The trained model first, the two-point calibration second, nothing
    // third. Deliberately in that order: the model is built from this probe's
    // own watering history and knows the spread of each band, where the
    // two-point anchors know only two readings and split the span in equal
    // thirds. When the model has not earned its gates yet, the anchors are
    // still better than a guess.
    // The snapshot, not the accumulator: this is called from the /moisture.json
    // handler on async_tcp as well as from the io task.
    const MoistureReading reading = moistureReading(index);
    if (reading.samples == 0) {
        return String();
    }

    double confidence = 0.0;
    const int inferred = moistureModelClassify(index, reading.average, &confidence);
    if (inferred != MOISTURE_UNKNOWN) {
        return String(moistureClassName(inferred));
    }

    const float dry = config.moistureDry[index];
    const float wet = config.moistureWet[index];
    const float span = wet - dry;

    // Uncalibrated. Reporting a band from an unknown scale would be a guess
    // dressed as a measurement, so the probe reports no state at all.
    if (fabsf(span) < 1e-3) {
        return String();
    }

    // Ordering is not assumed: with the 100-ADC% conversion the air reading is
    // the smaller number, but a different probe or conversion can invert that.
    float fraction = (reading.average - dry) / span;
    if (fraction < 0.0) {
        fraction = 0.0;
    } else if (fraction > 1.0) {
        fraction = 1.0;
    }

    if (fraction < 1.0 / 3.0) {
        return String("Dry");
    }
    if (fraction < 2.0 / 3.0) {
        return String("Humid");
    }
    return String("Wet");
}

double
flowTotalLitres()
{
    return g_flowTotalLitres;
}

bool
floatRaised()
{
    return g_floatRaised;
}

void
sensorsSetup()
{
    // Every scalar accumulator sizes its window from the MQTT period at
    // construction, so each average covers exactly one publish interval. An
    // array cannot pass a constructor argument, so the probes are sized here
    // instead — otherwise they silently keep the 120-sample default and their
    // averages span a different interval from every other channel.
    // Sized for every slot, not just the fitted ones: an unfitted probe is
    // never fed, so the window costs nothing, and a probe added in the web UI
    // is correctly sized on the next boot without a second code path.
    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        g_soilMoisture[i].setMaxLen(g_mqttTaskPeriod / g_ioTaskPeriod);
    }

    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        probeHealthReset(g_probeHealth[i]);
    }

    pinMode(config.buttonPin, INPUT);

    // A probe's power pin is driven OFF before it is ever driven on. Until a
    // pin is configured it floats, and a floating gate on whatever switches
    // the sensor is an undefined amount of time with the electrodes live —
    // which is the exact thing power gating exists to avoid.
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const uint8_t pin = config.soilMoisturePowerPin[i];
        if (pin != ConfigFile::kNoPin) {
            digitalWrite(pin, config.soilMoisturePowerOn[i] ? LOW : HIGH);
            pinMode(pin, OUTPUT);
            digitalWrite(pin, config.soilMoisturePowerOn[i] ? LOW : HIGH);
        }
    }

    // Only peripherals config.json declares get their pins touched. Attaching
    // an interrupt to a pin nothing is wired to would count noise as flow.
    if (config.flowFitted) {
        pinMode(config.flowPin, INPUT_PULLUP);
        attachInterrupt(
          digitalPinToInterrupt(config.flowPin), flowPulseISR, FALLING);
        logger.info("Flow sensor on GPIO " + String(config.flowPin) + ", " +
                    String(config.flowPulsesPerLitre, 1) + " pulses/litre");
    }

    if (config.floatFitted) {
        pinMode(config.floatPin, INPUT_PULLUP);
        logger.info("Float switch on GPIO " + String(config.floatPin) +
                    ", active " + String(config.floatActiveLevel));
    }

    logger.info("Sensors: " + String(config.moistureCount) + " moisture" +
                (config.luminosityFitted ? ", luminosity" : "") +
                (config.dhtFitted ? ", DHT" : "") +
                (config.waterLevelFitted ? ", water level" : "") +
                (config.flowFitted ? ", flow" : "") +
                (config.floatFitted ? ", float switch" : ""));
}

// Energises every probe that has a power pin, waits the longest settle any of
// them asked for, and reports whether anything was switched on.
//
// Coalesced on purpose: two probes on one MOSFET is the normal wiring, and
// powering them one at a time would pay the settle delay twice for no reason.
static bool
moisturePowerUp()
{
    uint16_t settle = 0;
    bool any = false;

    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const uint8_t pin = config.soilMoisturePowerPin[i];
        if (pin == ConfigFile::kNoPin) {
            continue;
        }
        digitalWrite(pin, config.soilMoisturePowerOn[i] ? HIGH : LOW);
        any = true;
        if (config.soilMoistureSettleMs[i] > settle) {
            settle = config.soilMoistureSettleMs[i];
        }
    }

    if (any && settle > 0) {
        delay(settle);
    }
    return any;
}

static void
moisturePowerDown()
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const uint8_t pin = config.soilMoisturePowerPin[i];
        if (pin != ConfigFile::kNoPin) {
            digitalWrite(pin, config.soilMoisturePowerOn[i] ? LOW : HIGH);
        }
    }
}

void
sensorsReadIo()
{
    const bool powered = moisturePowerUp();
    (void)powered;

    for (unsigned i = 0; i < config.moistureCount; ++i) {
        const uint8_t pin = config.soilMoisturePin[i];

        // TWO conversions, and the second is the reading.
        //
        // The first carries charge from the previous channel through the SAR
        // hold capacitor, so on a stiff source it equals the second and on a
        // high-impedance one it is dragged toward wherever the ADC just was.
        // That difference is the only thing measured here that can tell a
        // disconnected probe from soil — see core/probe_health.h for the three
        // passive statistics that cannot.
        //
        // It costs one extra conversion per probe per second, about 100 us,
        // and it improves the value it diagnoses: the second read is the
        // settled one.
        const int first = analogRead(pin);
        const int second = analogRead(pin);
        probeHealthAdd(g_probeHealth[i], g_lastAdcRaw, first, second);
        g_lastAdcRaw = second;

        if (g_probeHealth[i].samples >= g_probeHealthWindow) {
            probeHealthDecay(g_probeHealth[i], g_probeHealthDecay);
        }

        const double pct = ADC_TO_PERCENT(second);
        // Per probe, not one sign for the board: a capacitive module reads
        // lower as the soil wets, a resistive divider reads higher, and a
        // board can carry one of each.
        g_soilMoisture[i].add(config.moistureInvert[i] ? (100.0 - pct) : pct);
    }

    if (powered) {
        moisturePowerDown();
    }

    if (config.luminosityFitted) {
        g_luminosity.add(ADC_TO_PERCENT(analogRead(config.luminosityPin)));
    }

    if (config.waterLevelFitted) {
        g_waterLevel.add(ADC_TO_WATER_LEVEL(analogRead(config.waterLevelPin)));
    }

    if (config.flowFitted) {
        portENTER_CRITICAL(&g_flowMux);
        const uint32_t pulses = g_flowPulses;
        g_flowPulses = 0;
        portEXIT_CRITICAL(&g_flowMux);

        // The io task runs at a known period, so pulses per tick converts
        // directly. Litres per minute is the unit a flow meter is specified in.
        const double litres = (double)pulses / (double)config.flowPulsesPerLitre;
        g_flowTotalLitres += litres;
        g_flowRate.add((float)(litres * 60000.0 / (double)g_ioTaskPeriod));
    }

    // Publish the snapshot for readers on other threads. Last, so it reflects
    // this tick's samples.
    //
    // The VALUES ARE COMPUTED OUTSIDE THE LOCK, and the critical section is a
    // 32-byte copy. The first version called getAverage() inside it, which
    // walks the sample list TWICE — once for the mean, once for the variance —
    // and writes a member on the way. Four probes at a 60-sample window is
    // ~480 pointer-chased list nodes with interrupts disabled on this core,
    // every one a potential cache miss that fetches from flash.
    //
    // CLAUDE.md states this rule about relayWrite and I broke it here anyway.
    // The board panicked with InterruptWDTTimoutCPU1.
    MoistureReading fresh[MOISTURE_MAX];
    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        fresh[i].average = g_soilMoisture[i].getAverage();
        fresh[i].samples = g_soilMoisture[i].getSamples();
    }

    // Computed OUTSIDE the lock, for the reason the comment above gives: the
    // regression walks five doubles per probe, and doing that with interrupts
    // disabled is the mistake that panicked this board once already.
    ProbeHealthReport health[MOISTURE_MAX];
    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        health[i].verdict = probeHealthVerdict(g_probeHealth[i],
                                               g_probeHealthMinSamples,
                                               g_probeHealthMaxSd,
                                               g_probeHealthMinSlope,
                                               g_probeHealthMinT);
        health[i].slope = (float)probeHealthSlope(g_probeHealth[i]);
        health[i].t = (float)probeHealthT(g_probeHealth[i]);
        health[i].stepSd = (float)probeHealthStepSd(g_probeHealth[i]);
        health[i].rail = (int8_t)probeHealthRail(g_probeHealth[i]);
        health[i].samples = g_probeHealth[i].samples;
    }

    portENTER_CRITICAL(&g_moistureSnapshotMux);
    memcpy(g_moistureSnapshot, fresh, sizeof(fresh));
    memcpy(g_probeHealthSnapshot, health, sizeof(health));
    portEXIT_CRITICAL(&g_moistureSnapshotMux);

    if (config.floatFitted) {
        // Two agreeing reads a tick apart: a float bobbing on the surface
        // chatters, and a single read would report it as level changes.
        static bool lastRead = false;
        const bool raised =
          (digitalRead(config.floatPin) == config.floatActiveLevel);
        if (raised == lastRead) {
            g_floatRaised = raised;
        }
        lastRead = raised;
    }
}

void
sensorsSetupDht()
{
    if (!config.dhtFitted) {
        return; // leaves g_dht null, which sensorsReadDht() already handles
    }

    g_dht = new (g_dhtStorage) DHT_Unified(config.dhtPin, DHT11);
    g_dht->begin();
    logger.info("DHT11 on GPIO " + String(config.dhtPin));
}

void
sensorsReadDht()
{
    if (g_dht == nullptr) {
        return;
    }

    sensors_event_t event;
    bool error = false;

    g_dht->temperature().getEvent(&event);
    if (isnan(event.temperature) == false) {
        g_temperature.add(event.temperature);
    } else {
        error = true;
    }

    g_dht->humidity().getEvent(&event);
    if (isnan(event.relative_humidity) == false) {
        g_airHumidity.add(event.relative_humidity);
    } else {
        error = true;
    }

    ++g_dhtTotalReads;
    if (error) {
        ++g_dhtReadErrors;
    }
}
