#include "core/sensors.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/moisture_model.h"
#include "core/tasks.h"
#include <new>
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

    pinMode(config.buttonPin, INPUT);

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

void
sensorsReadIo()
{
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        g_soilMoisture[i].add(
          100.0 - ADC_TO_PERCENT(analogRead(config.soilMoisturePin[i])));
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
    portENTER_CRITICAL(&g_moistureSnapshotMux);
    for (unsigned i = 0; i < MOISTURE_MAX; ++i) {
        g_moistureSnapshot[i].average = g_soilMoisture[i].getAverage();
        g_moistureSnapshot[i].samples = g_soilMoisture[i].getSamples();
    }
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
