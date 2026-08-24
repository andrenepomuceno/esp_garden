#include "core/moisture_model.h"
#include "core/config.h"
#include "core/io_history.h"
#include "core/logger.h"
#include <SPIFFS.h>
#include <math.h>
#include <string.h>

// Enough accumulated weight per class to mean something. At a 60 s history
// period a single 30-minute wet window contributes 30 samples, so this is
// roughly "two or three watering cycles have been seen", before decay.
const double g_moistureMinWeightPerClass = 20.0;

// Fisher's J = 4 puts the dry and wet means two pooled standard deviations
// apart. Below that the bands overlap enough that a badge would be a coin
// toss, and the honest output is no badge.
const double g_moistureMinSeparation = 4.0;

// Events, not samples: a model fitted to one watering cycle describes that
// cycle. Six is roughly a week of once-daily watering.
const unsigned g_moistureMinEvents = 6;

const double g_moistureDecayPerRun = 0.93;

// How long after a watering the soil counts as wet, and how long before the
// next one it counts as dry. The wet window is not zero because absorption
// takes time — the reading at the pump's own edge is still the old soil.
static const uint32_t g_wetWindowSec = 30 * 60;
static const uint32_t g_dryWindowSec = 60 * 60;

// A sample further than this from its class mean is discarded on the second
// pass. This is how a floating input — which reads at a rail — is kept from
// dragging a class mean to a value the soil never had. It is also, precisely,
// the component BIC found when the history was clustered blind.
static const double g_outlierZ = 3.0;

// Per probe, per training run. A day holds a handful of waterings; the cap
// costs 4 bytes each and bounds the pass.
static const unsigned g_maxEventsPerRun = 32;

static const char* const g_modelPath = "/moisture_model.bin";
static const uint32_t g_modelMagic = 0x4D4F4931UL; // "MOI1"

static MoistureModelState g_state = {};

const MoistureModelState&
moistureModelState()
{
    return g_state;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

struct ModelFileHeader
{
    uint32_t magic;
    uint16_t stateSize;
    uint16_t probes;
};

static void
moistureModelSave()
{
    File file = SPIFFS.open(g_modelPath, FILE_WRITE);
    if (file == false) {
        logger.error("[moisture] cannot write " + String(g_modelPath));
        return;
    }

    const ModelFileHeader header = { g_modelMagic,
                                     (uint16_t)sizeof(MoistureModelState),
                                     (uint16_t)MOISTURE_MAX };
    file.write((const uint8_t*)&header, sizeof(header));
    file.write((const uint8_t*)&g_state, sizeof(g_state));
    file.close();
}

static void
moistureModelLoad()
{
    memset(&g_state, 0, sizeof(g_state));

    if (!SPIFFS.exists(g_modelPath)) {
        return;
    }

    File file = SPIFFS.open(g_modelPath, FILE_READ);
    if (file == false) {
        return;
    }

    ModelFileHeader header = {};
    const bool ok =
      file.read((uint8_t*)&header, sizeof(header)) == (int)sizeof(header) &&
      header.magic == g_modelMagic &&
      header.stateSize == sizeof(MoistureModelState) &&
      header.probes == MOISTURE_MAX &&
      file.read((uint8_t*)&g_state, sizeof(g_state)) == (int)sizeof(g_state);
    file.close();

    if (!ok) {
        // A layout change discards the model rather than reinterpreting old
        // bytes as parameters. Weeks of evidence are cheaper to rebuild than a
        // wrong band is to notice.
        logger.warning("[moisture] stored model unreadable; starting over");
        memset(&g_state, 0, sizeof(g_state));
        SPIFFS.remove(g_modelPath);
    }
}

void
moistureModelSetup()
{
    moistureModelLoad();

    if (g_state.trainedAt == 0) {
        return;
    }

    unsigned usable = 0;
    for (unsigned i = 0; i < config.moistureCount; ++i) {
        if (g_state.probe[i].usable) {
            ++usable;
        }
    }
    logger.info("[moisture] model loaded, " + String(usable) + "/" +
                String(config.moistureCount) + " probes usable");
}

// ---------------------------------------------------------------------------
// Training
// ---------------------------------------------------------------------------

// Which relay waters which probe, validated against what this board has.
static int
probeRelay(unsigned probe)
{
    const int relay = config.moistureRelay[probe];
    return (relay < 0 || relay >= (int)config.relayCount) ? -1 : relay;
}

// Every probe is trained in the SAME three passes over the buffer, not three
// passes each. At 1440 records a pass is a second or two of SPIFFS reads, and
// this is a background task — one that blocks every other background task for
// its duration, so twelve passes on a four-probe board would be a visible
// stall in MQTT and TalkBack once a day.
struct ScanContext
{
    // Pass 1: watering edges per probe.
    uint32_t events[MOISTURE_MAX][g_maxEventsPerRun];
    unsigned eventCount[MOISTURE_MAX];
    bool relayWasOn[MOISTURE_MAX];

    // Passes 2 and 3: the fit in progress.
    GaussianStats fit[MOISTURE_MAX][MOISTURE_CLASS_COUNT];
    unsigned nextEvent[MOISTURE_MAX];
    uint32_t previousWatering[MOISTURE_MAX];

    // Pass 3 only.
    const GaussianStats (*reference)[MOISTURE_CLASS_COUNT];
    uint32_t samplesUsed;
    uint32_t outliersDropped;

    int relayOf[MOISTURE_MAX];
    unsigned probes;
};

static bool
collectEvents(const IoRecord& record, uint32_t, void* ctx)
{
    ScanContext& scan = *(ScanContext*)ctx;

    for (unsigned p = 0; p < scan.probes; ++p) {
        const int relay = scan.relayOf[p];
        if (relay < 0) {
            continue;
        }
        const bool on = (record.relayMask & (uint16_t)(1u << relay)) != 0;
        // Rising edge only. The mask is sticky across a whole record period,
        // so one long watering spans several records and must count once.
        if (on && !scan.relayWasOn[p] && record.timestamp > 0 &&
            scan.eventCount[p] < g_maxEventsPerRun) {
            scan.events[p][scan.eventCount[p]++] = record.timestamp;
        }
        scan.relayWasOn[p] = on;
    }
    return true;
}

// Wet shortly after a watering, dry shortly before the next, humid between.
// Nothing at all before the first event the buffer contains, where there is no
// cycle to place the reading in.
static int
labelFor(uint32_t timestamp, uint32_t previousWatering, uint32_t nextWatering)
{
    if (previousWatering != 0 &&
        timestamp <= previousWatering + g_wetWindowSec) {
        return MOISTURE_WET;
    }
    if (nextWatering != 0 && timestamp + g_dryWindowSec >= nextWatering) {
        return MOISTURE_DRY;
    }
    if (previousWatering != 0 && nextWatering != 0) {
        return MOISTURE_HUMID;
    }
    return MOISTURE_UNKNOWN;
}

static bool
accumulate(const IoRecord& record, uint32_t, void* ctx)
{
    ScanContext& scan = *(ScanContext*)ctx;

    if (record.timestamp == 0) {
        return true;
    }

    for (unsigned p = 0; p < scan.probes; ++p) {
        if (scan.relayOf[p] < 0 || scan.eventCount[p] == 0) {
            continue;
        }
        const double value = record.moisture[p];
        if (!isfinite(value)) {
            continue; // not fitted at the time, or the slot was never written
        }

        while (scan.nextEvent[p] < scan.eventCount[p] &&
               scan.events[p][scan.nextEvent[p]] <= record.timestamp) {
            scan.previousWatering[p] = scan.events[p][scan.nextEvent[p]];
            ++scan.nextEvent[p];
        }
        const uint32_t nextWatering =
          (scan.nextEvent[p] < scan.eventCount[p])
            ? scan.events[p][scan.nextEvent[p]]
            : 0;

        const int label =
          labelFor(record.timestamp, scan.previousWatering[p], nextWatering);
        if (label == MOISTURE_UNKNOWN) {
            continue;
        }

        if (scan.reference != nullptr &&
            moistureZScore(scan.reference[p][label], value) > g_outlierZ) {
            ++scan.outliersDropped;
            continue;
        }

        gaussianAdd(scan.fit[p][label], value);
        ++scan.samplesUsed;
    }
    return true;
}

static void
resetFit(ScanContext& scan)
{
    for (unsigned p = 0; p < MOISTURE_MAX; ++p) {
        scan.nextEvent[p] = 0;
        scan.previousWatering[p] = 0;
        for (int c = 0; c < MOISTURE_CLASS_COUNT; ++c) {
            gaussianReset(scan.fit[p][c]);
        }
    }
    scan.samplesUsed = 0;
    scan.outliersDropped = 0;
}

void
moistureModelTrain()
{
    if (!ioHistory.ready() || config.moistureCount == 0) {
        return;
    }

    // ~1.6 KB on the stack for four probes. The task that calls this is a
    // background one with the default stack, so it is heap-allocated rather
    // than risking a silent overflow on a path that runs once a day.
    ScanContext* scan = (ScanContext*)calloc(1, sizeof(ScanContext));
    if (scan == nullptr) {
        logger.error("[moisture] no memory for a training run");
        return;
    }

    scan->probes = config.moistureCount;
    for (unsigned p = 0; p < MOISTURE_MAX; ++p) {
        scan->relayOf[p] = (p < scan->probes) ? probeRelay(p) : -1;
    }

    // Pass 1 — where the waterings were.
    ioHistory.forEach(collectEvents, scan);

    // Pass 2 — fit with everything, so pass 3 has a mean to measure against.
    resetFit(*scan);
    scan->reference = nullptr;
    ioHistory.forEach(accumulate, scan);

    GaussianStats first[MOISTURE_MAX][MOISTURE_CLASS_COUNT];
    memcpy(first, scan->fit, sizeof(first));

    // Pass 3 — refit, rejecting anything beyond 3 sigma of that first fit.
    // Two passes are needed because the rejection threshold is itself a
    // function of the fit it is protecting.
    resetFit(*scan);
    scan->reference = first;
    g_state.recordsScanned = (uint32_t)ioHistory.forEach(accumulate, scan);
    g_state.samplesUsed = scan->samplesUsed;
    g_state.outliersDropped = scan->outliersDropped;

    for (unsigned p = 0; p < config.moistureCount; ++p) {
        MoistureProbeModel& model = g_state.probe[p];

        if (scan->relayOf[p] < 0) {
            // No pump feeds this probe, so nothing labels its readings. The
            // two-point calibration can still classify it; the model cannot.
            model.usable = false;
            logger.info("[moisture] probe " + String(p) +
                        ": no relay assigned, no model");
            continue;
        }

        // Age what is already known, then fold in what today added. This is
        // what lets a model exist at all: one day supplies one or two watering
        // cycles, which is not a model of anything.
        for (int c = 0; c < MOISTURE_CLASS_COUNT; ++c) {
            gaussianDecay(model.classes[c], g_moistureDecayPerRun);
            model.classes[c].weight += scan->fit[p][c].weight;
            model.classes[c].sum += scan->fit[p][c].sum;
            model.classes[c].sumSq += scan->fit[p][c].sumSq;
        }
        model.wateringEvents =
          (uint32_t)(model.wateringEvents * g_moistureDecayPerRun) +
          scan->eventCount[p];

        model.separation = (float)moistureSeparation(model.classes);
        model.usable = (model.wateringEvents >= g_moistureMinEvents) &&
                       moistureModelIsUsable(model.classes,
                                             g_moistureMinWeightPerClass,
                                             g_moistureMinSeparation);

        logger.info(
          "[moisture] probe " + String(p) + ": +" +
          String(scan->eventCount[p]) + " events (" +
          String(model.wateringEvents) + " total), J=" +
          String(model.separation, 1) + ", dry/humid/wet = " +
          String(gaussianMean(model.classes[MOISTURE_DRY]), 1) + "/" +
          String(gaussianMean(model.classes[MOISTURE_HUMID]), 1) + "/" +
          String(gaussianMean(model.classes[MOISTURE_WET]), 1) +
          (model.usable ? " -> usable" : " -> not usable yet"));
    }

    free(scan);

    g_state.trainedAt = (uint32_t)time(NULL);
    moistureModelSave();

    logger.info("[moisture] trained on " + String(g_state.recordsScanned) +
                " records, " + String(g_state.samplesUsed) + " samples, " +
                String(g_state.outliersDropped) + " outliers dropped");
}

int
moistureModelClassify(unsigned index, double value, double* confidence)
{
    if (confidence != nullptr) {
        *confidence = 0.0;
    }
    if (index >= config.moistureCount || !g_state.probe[index].usable) {
        return MOISTURE_UNKNOWN;
    }

    return moistureClassify(g_state.probe[index].classes,
                            value,
                            g_moistureMinWeightPerClass,
                            g_moistureMinSeparation,
                            confidence);
}
