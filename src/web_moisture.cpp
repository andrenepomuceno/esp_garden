#include "core/config.h"
#include "core/moisture_model.h"
#include "core/probe_health.h"
#include "core/sensors.h"
#include "network/web_moisture.h"
#include <Arduino_JSON.h>
#include <math.h>

// Everything about the classifier, including the parts that say not to trust
// it. A model whose parameters cannot be inspected is a threshold that
// appeared from nowhere, and this firmware already has one documented case of
// a confident-looking clustering result that was nonsense — so the endpoint
// reports the gates and the evidence alongside the answer.
void
handleMoistureJson(AsyncWebServerRequest* request)
{
    const MoistureModelState& state = moistureModelState();

    JSONVar doc;

    doc["trainedAt"] = (double)state.trainedAt;
    doc["recordsScanned"] = (int)state.recordsScanned;
    doc["samplesUsed"] = (int)state.samplesUsed;
    doc["outliersDropped"] = (int)state.outliersDropped;

    // The gates, sent rather than hardcoded in the page: a reader has to be
    // able to see WHY a probe reports nothing.
    doc["gates"]["minWeightPerClass"] = g_moistureMinWeightPerClass;
    doc["gates"]["minSeparation"] = g_moistureMinSeparation;
    doc["gates"]["minEvents"] = (int)g_moistureMinEvents;
    doc["gates"]["decayPerRun"] = g_moistureDecayPerRun;

    static const char* const classKey[MOISTURE_CLASS_COUNT] = { "dry",
                                                                "humid",
                                                                "wet" };

    for (unsigned p = 0; p < config.moistureCount; ++p) {
        const MoistureProbeModel& model = state.probe[p];

        doc["probes"][p]["index"] = (int)p;
        doc["probes"][p]["name"] = config.soilMoistureName[p];
        doc["probes"][p]["relay"] = (int)config.moistureRelay[p];
        doc["probes"][p]["usable"] = model.usable;
        doc["probes"][p]["separation"] = (double)model.separation;
        doc["probes"][p]["wateringEvents"] = (int)model.wateringEvents;
        doc["probes"][p]["response"] = (double)model.response;

        // Seconds, 0 while unmeasured. Reported because it is the one number
        // that says how long after a watering this probe's readings mean
        // anything, and because a tau that drifts is a probe losing contact
        // with its soil long before the separation gate notices.
        doc["probes"][p]["tauSec"] = (double)model.tauSec;

        // Is there a sensor on the pin at all? Reported whether or not a model
        // exists, because it is the question that comes first and the one the
        // model cannot answer for months.
        const ProbeHealthReport health = probeHealthReport(p);
        doc["probes"][p]["health"]["verdict"] =
          probeVerdictName(health.verdict);
        doc["probes"][p]["health"]["couplingSlope"] = (double)health.slope;
        doc["probes"][p]["health"]["t"] = (double)health.t;
        doc["probes"][p]["health"]["stepSd"] = (double)health.stepSd;
        doc["probes"][p]["health"]["rail"] = (int)health.rail;
        doc["probes"][p]["health"]["samples"] = (int)health.samples;

        double totalWeight = 0.0;
        for (int c = 0; c < MOISTURE_CLASS_COUNT; ++c) {
            totalWeight += model.classes[c].weight;
        }

        for (int c = 0; c < MOISTURE_CLASS_COUNT; ++c) {
            const GaussianStats& stats = model.classes[c];
            const char* key = classKey[c];

            doc["probes"][p]["classes"][key]["weight"] = stats.weight;
            doc["probes"][p]["classes"][key]["mean"] = gaussianMean(stats);
            doc["probes"][p]["classes"][key]["sd"] =
              sqrt(gaussianVariance(stats));
            doc["probes"][p]["classes"][key]["prior"] =
              (totalWeight > 0.0) ? (stats.weight / totalWeight) : 0.0;
        }

        // The live inference, so the page shows the classification and the
        // parameters that produced it in one place rather than asking the
        // reader to trust that they match.
        // moistureReading(), never the accumulator: this handler runs on the
        // async_tcp task while the io task pushes into that std::list at 1 Hz,
        // and walking a list across a pop_front dereferences a freed node.
        // CLAUDE.md calls this out by name and I did it anyway.
        const MoistureReading snapshot = moistureReading(p);
        const double reading = snapshot.average;
        const bool sampled = snapshot.samples > 0;
        if (sampled) {
            doc["probes"][p]["reading"] = reading;
        }

        double confidence = 0.0;
        const int inferred =
          sampled ? moistureModelClassify(p, reading, &confidence)
                  : MOISTURE_UNKNOWN;

        if (inferred != MOISTURE_UNKNOWN) {
            doc["probes"][p]["inferred"] = moistureClassName(inferred);
            doc["probes"][p]["confidence"] = confidence;
            doc["probes"][p]["source"] = "model";
        } else {
            // Say which gate it failed. "No badge" with no reason is the thing
            // that makes a classifier impossible to debug from the outside.
            // Checked in the same order the gates are, and the per-class
            // weight gate is checked EXPLICITLY here rather than being left
            // inside moistureModelIsUsable(). It used to fall through to the
            // ordering message, so a probe that simply had not accumulated
            // enough evidence yet was reported as one whose labelling
            // contradicted the physics — the alarming diagnosis for the
            // harmless case.
            String reason;
            double weakest = -1.0;
            for (int c = 0; c < MOISTURE_CLASS_COUNT; ++c) {
                if (weakest < 0.0 || model.classes[c].weight < weakest) {
                    weakest = model.classes[c].weight;
                }
            }

            const ProbeHealthReport wiring = probeHealthReport(p);

            if (!sampled) {
                reason = "no reading from this probe yet";
            } else if (wiring.verdict == PROBE_FLOATING) {
                // Ahead of everything statistical, for the reason the response
                // check is: it names a physical cause. The separation gate
                // would refuse this probe too, days later, as "bands overlap".
                reason = "nothing appears to be connected: this pin follows the "
                         "ADC channel read before it (" +
                         String(wiring.slope * 100.0, 1) +
                         "% coupling), which a real sensor does not";
            } else if (wiring.verdict == PROBE_NOISY) {
                // The strongest and fastest of the three, and the only one
                // with a measured control on the same ADC: the luminosity
                // channel sits four orders of magnitude below this.
                reason = "this reading jumps " + String(wiring.stepSd, 0) +
                         " ADC counts between consecutive readings, which soil "
                         "cannot do however fast it is watered - the sensor is "
                         "not measuring anything";
            } else if (wiring.verdict == PROBE_RAILED) {
                // The two rails do not mean the same thing, and saying so is
                // the difference between a diagnosis and a guess: a healthy
                // capacitive probe lifted out of the soil reads full scale
                // too, exactly like a pin tied to 3V3.
                reason = (wiring.rail > 0)
                           ? "this pin sits at the supply rail. Either it is "
                             "shorted to 3V3, or the probe is out of the soil "
                             "or in bone-dry ground - a capacitive module "
                             "reads full scale in air"
                           : "this pin sits at ground: shorted, or the sensor "
                             "module has no supply";
            } else if (wiring.verdict == PROBE_STUCK) {
                reason = "this reading has not moved at all: the sensor is "
                         "driving a level but no longer measuring";
            } else if (model.wateringEvents >= 2 &&
                       fabs(model.response) < 0.5) {
                // Said BEFORE the statistical gates, because it names a
                // physical cause rather than a symptom. The separation gate
                // would refuse this probe too, several days later, with
                // "bands overlap" — true, and useless for fixing it.
                reason = "this probe does not respond to its pump (rise of " +
                         String(model.response, 2) +
                         " across waterings): check the probe, the pot it is "
                         "in, and whether the pump runs";
            } else if (config.moistureRelay[p] < 0) {
                reason = "no relay assigned: nothing labels this probe";
            } else if (state.trainedAt == 0) {
                reason = "not trained yet";
            } else if (model.wateringEvents < g_moistureMinEvents) {
                reason = "only " + String(model.wateringEvents) + " of " +
                         String(g_moistureMinEvents) +
                         " watering events seen so far";
            } else if (weakest < g_moistureMinWeightPerClass) {
                reason = "thinnest class has " + String(weakest, 1) +
                         " of the " + String(g_moistureMinWeightPerClass, 0) +
                         " weight needed";
            } else if (model.separation < g_moistureMinSeparation) {
                reason = "bands overlap: separation J=" +
                         String(model.separation, 1) + " below " +
                         String(g_moistureMinSeparation, 1);
            } else {
                reason = "class means are not ordered dry..humid..wet";
            }
            doc["probes"][p]["blockedBy"] = reason;

            // The fallback the dashboard is actually showing, if any.
            const String fallback = moistureState(p);
            doc["probes"][p]["source"] =
              fallback.length() > 0 ? "two-point calibration" : "none";
            if (fallback.length() > 0) {
                doc["probes"][p]["inferred"] = fallback;
            }
        }

        doc["probes"][p]["calibration"]["dry"] = (double)config.moistureDry[p];
        doc["probes"][p]["calibration"]["wet"] = (double)config.moistureWet[p];
    }

    AsyncWebServerResponse* response =
      request->beginResponse(200, "application/json", JSON.stringify(doc));
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}
