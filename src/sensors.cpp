#include "core/sensors.h"
#include "BuildConfig.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/tasks.h"
#include <new>
#ifdef HAS_DHT_SENSOR
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#endif

#define ADC_TO_PERCENT(x) ((x * 100.0) / 4095.0)

#ifdef HAS_DHT_SENSOR
// Constructed in tasksSetup(), not at static-init time: DHT_Unified copies the
// pin in its constructor, and at static-init config.json has not been read yet,
// so a file-scope instance permanently runs on the compiled default pin.
alignas(DHT_Unified) static uint8_t g_dhtStorage[sizeof(DHT_Unified)];
static DHT_Unified* g_dht = nullptr;
AccumulatorV2 g_temperature(g_mqttTaskPeriod / g_dhtTaskPeriod);
AccumulatorV2 g_airHumidity(g_mqttTaskPeriod / g_dhtTaskPeriod);
unsigned g_dhtReadErrors = 0;
unsigned g_dhtTotalReads = 0;
#endif

#ifdef HAS_MOISTURE_SENSOR
AccumulatorV2 g_soilMoisture[MOISTURE_SENSOR_COUNT];
#endif

#ifdef HAS_LUMINOSITY_SENSOR
AccumulatorV2 g_luminosity(g_mqttTaskPeriod / g_ioTaskPeriod);
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
#define ADC_TO_WATER_LEVEL(v) (9.0 - 12.0 * sin(4.04 - 1.61 * (3.3 * v / 4095.0)))
AccumulatorV2 g_waterLevel(g_mqttTaskPeriod / g_ioTaskPeriod);
#endif

#ifdef HAS_FLOW_SENSOR
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
#endif

#ifdef HAS_FLOAT_SWITCH
// A contact, not a level: one bit, debounced by requiring two agreeing reads.
static bool g_floatRaised = false;
#endif

#ifdef HAS_MOISTURE_SENSOR
String
moistureState(unsigned index)
{
    if (index >= MOISTURE_SENSOR_COUNT) {
        return String();
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
    float fraction = (g_soilMoisture[index].getAverage() - dry) / span;
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
#endif

#ifdef HAS_FLOW_SENSOR
double
flowTotalLitres()
{
    return g_flowTotalLitres;
}
#endif

#ifdef HAS_FLOAT_SWITCH
bool
floatRaised()
{
    return g_floatRaised;
}
#endif

void
sensorsSetup()
{
    // Every scalar accumulator sizes its window from the MQTT period at
    // construction, so each average covers exactly one publish interval. An
    // array cannot pass a constructor argument, so the probes are sized here
    // instead — otherwise they silently keep the 120-sample default and their
    // averages span a different interval from every other channel.
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        g_soilMoisture[i].setMaxLen(g_mqttTaskPeriod / g_ioTaskPeriod);
    }
#endif

    pinMode(config.buttonPin, INPUT);

#ifdef HAS_FLOW_SENSOR
    pinMode(config.flowPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(config.flowPin), flowPulseISR, FALLING);
    logger.info("Flow sensor on GPIO " + String(config.flowPin) + ", " +
                String(config.flowPulsesPerLitre, 1) + " pulses/litre");
#endif
#ifdef HAS_FLOAT_SWITCH
    pinMode(config.floatPin, INPUT_PULLUP);
    logger.info("Float switch on GPIO " + String(config.floatPin) +
                ", active " + String(config.floatActiveLevel));
#endif
}

void
sensorsReadIo()
{
#ifdef HAS_MOISTURE_SENSOR
    for (unsigned i = 0; i < MOISTURE_SENSOR_COUNT; ++i) {
        g_soilMoisture[i].add(
          100.0 - ADC_TO_PERCENT(analogRead(config.soilMoisturePin[i])));
    }
#endif

#ifdef HAS_LUMINOSITY_SENSOR
    g_luminosity.add(ADC_TO_PERCENT(analogRead(config.luminosityPin)));
#endif

#ifdef HAS_WATER_LEVEL_SENSOR
    g_waterLevel.add(ADC_TO_WATER_LEVEL(analogRead(config.waterLevelPin)));
#endif

#ifdef HAS_FLOW_SENSOR
    {
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
#endif

#ifdef HAS_FLOAT_SWITCH
    {
        // Two agreeing reads a tick apart: a float bobbing on the surface
        // chatters, and a single read would report it as level changes.
        static bool lastRead = false;
        const bool raised = (digitalRead(config.floatPin) == config.floatActiveLevel);
        if (raised == lastRead) {
            g_floatRaised = raised;
        }
        lastRead = raised;
    }
#endif
}

#ifdef HAS_DHT_SENSOR
void
sensorsSetupDht()
{
    g_dht = new (g_dhtStorage) DHT_Unified(config.dhtPin, DHT11);
    g_dht->begin();
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
#endif
