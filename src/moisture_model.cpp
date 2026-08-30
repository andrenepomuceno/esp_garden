#include "core/moisture_model.h"
#include "core/config.h"
#include "core/io_history.h"
#include "core/logger.h"
#include "core/filesystem.h"
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

// How long the soil takes to actually take up the water. Samples inside this
// window after a watering carry a reduced weight, ramping to full: the probe is
// still reading the old soil for the first minutes.
static const uint32_t g_absorptionLagSec = 5 * 60;

// The absorption capture runs LONGER than the wet labelling window. The two
// answer different questions: 30 minutes is how long a reading counts as wet,
// while the settle check needs roughly four time constants of curve before it
// will believe a plateau. Capturing an hour doubles the time constants that can
// be measured at no cost, because the records are already there — they are just
// labelled HUMID rather than WET.
static const uint32_t g_riseWindowSec = 60 * 60;

// Samples kept per curve. The capture DECIMATES when it fills — every second
// sample is dropped and the stride doubles — so the array bounds the memory
// without bounding the window. A fixed cap would have silently tied the
// estimator to a 60 s history period, and history.periodSec is configurable
// down to 10 s: at that period 32 samples cover 310 s of a 3600 s window, the
// plateau is read off a curve that is still climbing, and tau collapses.
static const unsigned g_riseMaxSamples = 32;

// Fewer samples than this and a "time constant" is a line drawn through noise.
static const unsigned g_riseMinSamples = 5;

// A rise smaller than this is not a response. Without the floor the fit would
// return a confident time constant for a probe that never answered its pump --
// exactly the probe the response check and the separation gate exist to catch.
static const double g_riseMinPoints = 1.0;

// How far from a window's edge a HUMID sample has to be to count fully. Inside
// it the reading is a transition, and transitions belong to neither neighbour.
static const uint32_t g_taperSec = 10 * 60;

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
    File file = FILESYSTEM.open(g_modelPath, FILE_WRITE);
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

    if (!FILESYSTEM.exists(g_modelPath)) {
        return;
    }

    File file = FILESYSTEM.open(g_modelPath, FILE_READ);
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
        FILESYSTEM.remove(g_modelPath);
    }
}

// Which relay waters which probe, validated against what this board has.
static int
probeRelay(unsigned probe)
{
    const int relay = config.moistureRelay[probe];
    return (relay < 0 || relay >= (int)config.relayCount) ? -1 : relay;
}

// Evidence belongs to a physical probe, not to a slot. Deleting a probe in
// /devices.html shifts every later index down, and the model that moves into
// the slot would otherwise describe another pot entirely and say so
// confidently.
//
// Called from setup AND from training. It used to live only inside the
// training loop, which the "no new watering cycles" early return skips — so a
// probe deleted on a device watered once a day kept the old pot's Gaussians,
// usable and confident, until a cycle happened to complete.

// FNV-1a over the probe's kind label. A hash rather than the string itself
// because this lives in the persisted model, where a String would mean a
// variable-length record; a collision costs a model that should have been
// discarded and was not, which is the same risk the label is guarding against
// and no worse than having no label at all.
static uint16_t
probeTag(const String& kind)
{
    uint32_t hash = 2166136261UL;
    for (unsigned i = 0; i < kind.length(); ++i) {
        hash ^= (uint8_t)kind[i];
        hash *= 16777619UL;
    }
    return (uint16_t)(hash ^ (hash >> 16));
}

static void
discardMovedProbes()
{
    for (unsigned p = 0; p < config.moistureCount; ++p) {
        MoistureProbeModel& model = g_state.probe[p];
        const uint8_t pin = config.soilMoisturePin[p];
        const int8_t relay = (int8_t)probeRelay(p);
        const uint16_t tag = probeTag(config.moistureKind[p]);
        const bool invert = config.moistureInvert[p];

        if (model.sourcePin == pin && model.sourceRelay == relay &&
            model.sourceTag == tag && model.sourceInvert == invert) {
            continue;
        }

        if (model.wateringEvents > 0 || model.usable) {
            String why;
            if (model.sourcePin != pin) {
                why += " pin " + String(model.sourcePin) + "->" + String(pin);
            }
            if (model.sourceRelay != relay) {
                why += " relay " + String(model.sourceRelay) + "->" +
                       String(relay);
            }
            if (model.sourceInvert != invert) {
                why += " polarity";
            }
            if (model.sourceTag != tag) {
                why += " kind '" + config.moistureKind[p] + "'";
            }
            logger.warning("[moisture] probe " + String(p) +
                           " is not the probe this model was fitted to (" +
                           why + " ); discarding it");
        }
        memset(&model, 0, sizeof(model));
        model.sourcePin = pin;
        model.sourceRelay = relay;
        model.sourceTag = tag;
        model.sourceInvert = invert;
    }
}

void
moistureModelSetup()
{
    moistureModelLoad();

    // Before anything reports a class: a model loaded off flash for a probe
    // that has since been moved is worse than no model, because it answers.
    discardMovedProbes();

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
    unsigned newEventCount[MOISTURE_MAX]; // only those past consumedUntil
    bool relayWasOn[MOISTURE_MAX];
    bool seenAnyRecord[MOISTURE_MAX];

    // Passes 2 and 3: the fit in progress.
    GaussianStats fit[MOISTURE_MAX][MOISTURE_CLASS_COUNT];
    unsigned nextEvent[MOISTURE_MAX];
    uint32_t previousWatering[MOISTURE_MAX];

    // Pass 2 only: one absorption curve at a time per probe, plus the running
    // mean of the time constants this run measured.
    float riseValue[MOISTURE_MAX][g_riseMaxSamples];
    uint16_t riseDt[MOISTURE_MAX][g_riseMaxSamples];
    uint8_t riseCount[MOISTURE_MAX];
    uint8_t riseStride[MOISTURE_MAX]; // 1, then doubled on each decimation
    uint32_t riseSeen[MOISTURE_MAX];  // samples offered to the capture
    uint32_t riseFrom[MOISTURE_MAX];
    float lastValue[MOISTURE_MAX];
    uint32_t lastTimestamp[MOISTURE_MAX];
    bool lastValid[MOISTURE_MAX];
    double tauSum[MOISTURE_MAX];
    unsigned tauCount[MOISTURE_MAX];

    // Pass 3 only.
    const GaussianStats (*reference)[MOISTURE_CLASS_COUNT];
    uint32_t samplesUsed;
    uint32_t outliersDropped;

    // The half-open window of history this run is allowed to consume.
    // Wet-vs-dry means for this run only, used to report the watering response.
    double wetSum[MOISTURE_MAX];
    double wetWeight[MOISTURE_MAX];
    double drySum[MOISTURE_MAX];
    double dryWeight[MOISTURE_MAX];

    uint32_t consumedFrom; // exclusive: everything at or before is spent
    uint32_t consumeUntil; // exclusive: the newest watering, whose own cycle
                           // is not complete until the NEXT one arrives

    int relayOf[MOISTURE_MAX];
    unsigned probes;
};

static bool
collectEvents(const IoRecord& record, uint32_t, void* ctx)
{
    ScanContext& scan = *(ScanContext*)ctx;

    if (record.timestamp == 0) {
        return true;
    }

    for (unsigned p = 0; p < scan.probes; ++p) {
        const int relay = scan.relayOf[p];
        if (relay < 0) {
            continue;
        }
        const bool on = (record.relayMask & (uint16_t)(1u << relay)) != 0;

        // Rising edge only. The mask is sticky across a whole record period,
        // so one long watering spans several records and must count once.
        //
        // `seenAnyRecord` is what keeps a watering that was ALREADY RUNNING at
        // the oldest surviving record from looking like a rising edge. The
        // buffer wraps, so its first record is an arbitrary moment, and a
        // watering straddling that boundary would otherwise register as an
        // event that never started — poisoning the DRY class with mid-watering
        // samples and advancing the six-event gate on a phantom.
        if (on && scan.seenAnyRecord[p] && !scan.relayWasOn[p] &&
            scan.eventCount[p] < g_maxEventsPerRun) {
            scan.events[p][scan.eventCount[p]++] = record.timestamp;
            // Counted separately from the array, because the ARRAY has to hold
            // events on both sides of the watermark — an already-consumed
            // watering is still the boundary that labels the cycle after it —
            // while the COUNTER must only see what is new. Without this the
            // samples were deduped by consumedUntil and the event counter was
            // not, so two reboots in a day pushed the six-event gate across on
            // three real waterings.
            if (record.timestamp > scan.consumedFrom) {
                ++scan.newEventCount[p];
            }
        }
        scan.relayWasOn[p] = on;
        scan.seenAnyRecord[p] = true;
    }
    return true;
}

// Wet shortly after a watering, dry shortly before the next, humid between.
// Nothing at all before the first event the buffer contains, where there is no
// cycle to place the reading in.
static int
labelFor(uint32_t timestamp,
         uint32_t previousWatering,
         uint32_t nextWatering,
         double tauSec,
         double& confidence)
{
    // The label is not equally trustworthy across its window, and saying so is
    // worth more than the label itself. gaussianAdd() has always taken a
    // weight; until now every sample was fed at 1.0, so a reading taken at the
    // exact instant the pump started counted as firmly "wet" as one taken
    // twenty minutes later. Boundary samples are the ones that blur the class
    // means together, and blurred means are what fails the separation gate.
    confidence = 1.0;

    if (previousWatering != 0 &&
        timestamp <= previousWatering + g_wetWindowSec) {
        // Soil does not become wet the moment a pump starts. Over the first
        // few minutes the probe is still reading the OLD soil, so those
        // samples are weighted down to near zero and the confidence ramps to
        // full once absorption has had time to happen.
        //
        // Absorption is diffusion, and diffusion is not a ramp: the reading
        // approaches its new level as 1 - e^(-t/tau). Where this probe's own
        // time constant has been measured, that curve IS the confidence, so a
        // fast probe reaches full weight in three minutes and a slow one is
        // still discounted at fifteen. The fixed ramp stands in only while no
        // measurement exists -- the same shape to first order, and it never
        // claims more than it knows.
        const uint32_t since = timestamp - previousWatering;
        confidence = (tauSec > 0.0)
                       ? moistureAbsorptionConfidence((double)since, tauSec)
                       : ((since >= g_absorptionLagSec)
                            ? 1.0
                            : ((double)since / (double)g_absorptionLagSec));
        return MOISTURE_WET;
    }

    if (nextWatering != 0 && timestamp + g_dryWindowSec >= nextWatering) {
        // Moisture falls monotonically between waterings, so the closer to the
        // next one, the drier — and the more confidently "dry". The sample at
        // the far edge of the window is barely drier than humid.
        const uint32_t until = nextWatering - timestamp;
        confidence = 1.0 - ((double)until / (double)g_dryWindowSec);
        if (confidence < 0.0) {
            confidence = 0.0;
        }
        return MOISTURE_DRY;
    }

    if (previousWatering != 0 && nextWatering != 0) {
        // Full confidence in the middle, tapering toward both neighbours: a
        // reading one minute outside the wet window is not meaningfully more
        // "humid" than one a minute inside it.
        const uint32_t sinceWet = timestamp - (previousWatering + g_wetWindowSec);
        const uint32_t untilDry = (nextWatering > timestamp + g_dryWindowSec)
                                    ? (nextWatering - g_dryWindowSec - timestamp)
                                    : 0;
        const uint32_t edge = (sinceWet < untilDry) ? sinceWet : untilDry;
        confidence = (edge >= g_taperSec) ? 1.0
                                          : ((double)edge / (double)g_taperSec);
        return MOISTURE_HUMID;
    }

    return MOISTURE_UNKNOWN;
}

// Halves the capture in place, keeping every second sample, and doubles the
// stride so the rest of the curve is sampled at the new rate. Element 0 — the
// pre-watering baseline — is always index 0 and always survives.
static void
decimateRise(ScanContext& scan, unsigned p)
{
    unsigned kept = 1;
    for (unsigned i = 2; i < scan.riseCount[p]; i += 2) {
        scan.riseValue[p][kept] = scan.riseValue[p][i];
        scan.riseDt[p][kept] = scan.riseDt[p][i];
        ++kept;
    }
    scan.riseCount[p] = (uint8_t)kept;
    scan.riseStride[p] = (uint8_t)(scan.riseStride[p] * 2);
}

// Closes the capture open for this probe, folding its time constant into the
// run's mean when the curve was good enough to fit. Idempotent, so it can be
// called on every new watering and again at the end of the pass.
static void
finishRise(ScanContext& scan, unsigned p)
{
    const double tau = moistureTimeConstant(scan.riseValue[p],
                                            scan.riseDt[p],
                                            scan.riseCount[p],
                                            g_riseMinSamples,
                                            g_riseMinPoints);
    if (tau > 0.0) {
        scan.tauSum[p] += tau;
        ++scan.tauCount[p];
    }
    scan.riseCount[p] = 0;
    scan.riseFrom[p] = 0;
}

static bool
accumulate(const IoRecord& record, uint32_t, void* ctx)
{
    ScanContext& scan = *(ScanContext*)ctx;

    // Beyond the last complete cycle this run can label: stop before a reading
    // is labelled HUMID now when the watering that would make it DRY has not
    // happened yet.
    if (record.timestamp >= scan.consumeUntil) {
        return true;
    }

    // At or before consumedFrom the evidence is already folded in and must not
    // be counted twice — but pass 2 still has to WATCH those records, because
    // the last of them is the pre-watering baseline of a watering sitting
    // exactly on the boundary.
    //
    // Skipping them outright is why this never measured anything in the case
    // that matters. With one watering a day and a daily training run, the
    // window is (C_prev, C_new): the only watering edge inside it is C_prev at
    // the very first record, and its baseline is one record on the other side
    // of the bound. lastValid was therefore false, no capture ever started,
    // tau stayed 0 forever, and the fixed ramp kept running while the endpoint
    // reported "unmeasured" as though it were merely waiting for data.
    const bool spent = (record.timestamp <= scan.consumedFrom);

    for (unsigned p = 0; p < scan.probes; ++p) {
        if (scan.relayOf[p] < 0 || scan.eventCount[p] == 0) {
            continue;
        }
        const double value = record.moisture[p];
        if (!isfinite(value)) {
            continue; // not fitted at the time, or the slot was never written
        }

        if (spent) {
            if (scan.reference == nullptr) {
                scan.lastValue[p] = (float)value;
                scan.lastTimestamp[p] = record.timestamp;
                scan.lastValid[p] = true;
            }
            continue;
        }

        const uint32_t wasWatering = scan.previousWatering[p];
        while (scan.nextEvent[p] < scan.eventCount[p] &&
               scan.events[p][scan.nextEvent[p]] <= record.timestamp) {
            scan.previousWatering[p] = scan.events[p][scan.nextEvent[p]];
            ++scan.nextEvent[p];
        }
        const uint32_t nextWatering =
          (scan.nextEvent[p] < scan.eventCount[p])
            ? scan.events[p][scan.nextEvent[p]]
            : 0;

        // Pass 2 only: pass 3 walks the same records and would count every
        // curve twice. The outlier rejection below does not apply here either
        // -- a rise is a shape, and rejecting its samples against a class mean
        // would flatten exactly the transient being measured.
        if (scan.reference == nullptr) {
            if (scan.previousWatering[p] != wasWatering) {
                // A new watering. Close whatever was open and start a capture
                // whose element 0 is the last reading BEFORE the pump: at the
                // pump's own edge the soil still reads its old value, and that
                // is the baseline the exponential rises from. Using the first
                // post-watering sample instead would measure the rise from a
                // point already part of the way up it.
                finishRise(scan, p);

                // The baseline has to be RECENT. After a gap — a power cycle,
                // a wrapped ring — the last reading on file can be hours old,
                // and the soil dried across it. The "rise" is then the real
                // absorption plus that drying, the target is crossed far too
                // early, and a bogus small tau is folded in and then takes ten
                // decayed runs to work back out.
                const uint32_t maxAge = 2 * (uint32_t)config.historyPeriodSec;
                const bool fresh =
                  scan.lastValid[p] &&
                  scan.previousWatering[p] >= scan.lastTimestamp[p] &&
                  (scan.previousWatering[p] - scan.lastTimestamp[p]) <= maxAge;

                if (fresh) {
                    scan.riseFrom[p] = scan.previousWatering[p];
                    scan.riseValue[p][0] = scan.lastValue[p];
                    scan.riseDt[p][0] = 0;
                    scan.riseCount[p] = 1;
                    scan.riseStride[p] = 1;
                    scan.riseSeen[p] = 0;
                }
            }

            if (scan.riseFrom[p] != 0) {
                if (record.timestamp > scan.riseFrom[p] + g_riseWindowSec) {
                    finishRise(scan, p);
                } else if (record.timestamp > scan.riseFrom[p]) {
                    const uint32_t dt = record.timestamp - scan.riseFrom[p];
                    if (scan.riseSeen[p] % scan.riseStride[p] == 0) {
                        if (scan.riseCount[p] >= g_riseMaxSamples) {
                            decimateRise(scan, p);
                        }
                        scan.riseValue[p][scan.riseCount[p]] = (float)value;
                        scan.riseDt[p][scan.riseCount[p]] = (uint16_t)dt;
                        ++scan.riseCount[p];
                    }
                    ++scan.riseSeen[p];
                }
            }

            scan.lastValue[p] = (float)value;
            scan.lastTimestamp[p] = record.timestamp;
            scan.lastValid[p] = true;
        }

        double confidence = 1.0;
        const int label = labelFor(record.timestamp,
                                   scan.previousWatering[p],
                                   nextWatering,
                                   g_state.probe[p].tauSec,
                                   confidence);
        if (label == MOISTURE_UNKNOWN) {
            continue;
        }

        if (scan.reference != nullptr &&
            moistureZScore(scan.reference[p][label], value) > g_outlierZ) {
            ++scan.outliersDropped;
            continue;
        }

        // A sample nobody is confident about contributes almost nothing rather
        // than being dropped outright — it is still weak evidence.
        gaussianAdd(scan.fit[p][label], value, confidence);
        ++scan.samplesUsed;

        // The watering RESPONSE, tracked per probe: how far the wet window sits
        // above the dry one. It is the cheapest check that this probe is
        // actually in the pot this pump waters, and it is free here because the
        // labels are already computed. A probe whose response stays near zero
        // after several waterings is either disconnected, in the wrong pot, or
        // downstream of a pump that is not running.
        //
        // Selected by POSITION in the window, not by confidence. The confidence
        // is now a function of this probe's own tau, so a `confidence > 0.5`
        // gate would admit samples from 150 s in on a fast probe and only from
        // 1250 s in on a slow one — and `response` is then compared against a
        // fixed 0.5-point threshold to decide whether a probe answers its pump
        // at all. A diagnosis that tells an operator to go and check a probe,
        // a pot and a pump must not depend on which probe it is looking at.
        const uint32_t sinceWatering =
          (scan.previousWatering[p] != 0 &&
           record.timestamp > scan.previousWatering[p])
            ? (record.timestamp - scan.previousWatering[p])
            : 0;
        if (label == MOISTURE_WET && sinceWatering * 3 >= g_wetWindowSec * 2) {
            scan.wetSum[p] += value;
            scan.wetWeight[p] += 1.0;
        } else if (label == MOISTURE_DRY && confidence > 0.5) {
            scan.drySum[p] += value;
            scan.dryWeight[p] += 1.0;
        }
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
        scan.wetSum[p] = 0.0;
        scan.wetWeight[p] = 0.0;
        scan.drySum[p] = 0.0;
        scan.dryWeight[p] = 0.0;

        // Deliberately NOT tauSum/tauCount: only pass 2 measures those, and
        // resetFit runs again before pass 3.
        scan.riseCount[p] = 0;
        scan.riseStride[p] = 1;
        scan.riseSeen[p] = 0;
        scan.riseFrom[p] = 0;
        scan.lastValid[p] = false;
        scan.lastTimestamp[p] = 0;
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

    // consumedFrom before pass 1, not after: collectEvents needs it to tell a
    // watering it has already counted from one it has not.
    scan->consumedFrom = g_state.consumedUntil;

    // Pass 1 — where the waterings were.
    ioHistory.forEach(collectEvents, scan);

    // The window this run may consume. `consumeUntil` is the NEWEST watering
    // across all probes: its own cycle is not finished until the next watering
    // bounds it, so everything from it onward waits for a later run. Without
    // that bound a reading would be folded in as HUMID today and be DRY
    // tomorrow, and the first answer is the one that sticks.
    scan->consumeUntil = 0;
    for (unsigned p = 0; p < scan->probes; ++p) {
        if (scan->eventCount[p] > 0) {
            const uint32_t newest = scan->events[p][scan->eventCount[p] - 1];
            if (newest > scan->consumeUntil) {
                scan->consumeUntil = newest;
            }
        }
    }

    // Before the early return, not after: a probe that moved must lose its model
    // whether or not a watering cycle completed since the last run.
    discardMovedProbes();

    if (scan->consumeUntil <= scan->consumedFrom) {
        // No watering has completed a cycle since the last run. Nothing to
        // learn, and — critically — nothing to decay: ageing the evidence on a
        // run that adds none would quietly drain the model on a device that is
        // rebooted often.
        logger.info("[moisture] no new watering cycles since the last run");
        free(scan);
        g_state.trainedAt = (uint32_t)time(NULL);
        moistureModelSave();
        return;
    }

    // Pass 2 — fit with everything, so pass 3 has a mean to measure against.
    resetFit(*scan);
    scan->reference = nullptr;
    ioHistory.forEach(accumulate, scan);

    // The newest watering inside the window leaves a capture open, with no
    // later record to close it.
    for (unsigned p = 0; p < scan->probes; ++p) {
        finishRise(*scan, p);
    }

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
          scan->newEventCount[p];

        // The watering response: how far this run's wet readings sat above its
        // dry ones. Decayed with the rest of the evidence so it follows a soil
        // that changes, and reported by /moisture.json because a response that
        // stays near zero is a finding — a probe that does not answer its own
        // pump is not in that pot, not connected, or downstream of a pump that
        // is not running. The separation gate would eventually refuse such a
        // probe anyway; this says WHY several days earlier.
        if (scan->wetWeight[p] > 0.0 && scan->dryWeight[p] > 0.0) {
            const float response =
              (float)((scan->wetSum[p] / scan->wetWeight[p]) -
                      (scan->drySum[p] / scan->dryWeight[p]));
            model.response = (model.response == 0.0f)
                               ? response
                               : (float)(model.response * g_moistureDecayPerRun +
                                         response * (1.0 - g_moistureDecayPerRun));
        }

        // The absorption time constant, from this run's own rises. Decayed
        // with everything else: how fast water reaches a probe depends on the
        // soil, the pot and the probe's contact with both, all of which change
        // slowly, so one watering measured through noise must not rewrite it.
        if (scan->tauCount[p] > 0) {
            const float tau = (float)(scan->tauSum[p] / scan->tauCount[p]);
            model.tauSec =
              (model.tauSec <= 0.0f)
                ? tau
                : (float)(model.tauSec * g_moistureDecayPerRun +
                          tau * (1.0 - g_moistureDecayPerRun));
        }

        model.separation = (float)moistureSeparation(model.classes);
        model.usable = (model.wateringEvents >= g_moistureMinEvents) &&
                       moistureModelIsUsable(model.classes,
                                             g_moistureMinWeightPerClass,
                                             g_moistureMinSeparation);

        logger.info(
          "[moisture] probe " + String(p) + ": +" +
          String(scan->newEventCount[p]) + " events (" +
          String(model.wateringEvents) + " total), J=" +
          String(model.separation, 1) + ", dry/humid/wet = " +
          String(gaussianMean(model.classes[MOISTURE_DRY]), 1) + "/" +
          String(gaussianMean(model.classes[MOISTURE_HUMID]), 1) + "/" +
          String(gaussianMean(model.classes[MOISTURE_WET]), 1) +
          ", response " + String(model.response, 2) + ", tau " +
          (model.tauSec > 0.0f ? String(model.tauSec / 60.0f, 1) + " min"
                               : String("unmeasured")) +
          (model.usable ? " -> usable" : " -> not usable yet"));
    }

    const uint32_t consumed = scan->consumeUntil;
    free(scan);

    g_state.consumedUntil = consumed;
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
