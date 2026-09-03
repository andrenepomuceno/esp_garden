#pragma once

#include <math.h>
#include <stdint.h>

// Change detection for a telemetry key that steps rather than drifts.
//
// A relay state, a reservoir contact, a reboot counter and a cumulative litre
// total are not levels that need sampling: they sit still, then step. Sampling
// them once per publish spends a datapoint per key per tick re-stating what the
// cloud already knows, and still reports the step late — by up to a whole
// publish period. So the rule is publish-on-change, with a heartbeat under it.
//
// This lives in a header of its own, free of Arduino, so `test_step_publisher`
// can reach it. That is the same reason `segment_index.h` exists, and for the
// same kind of arithmetic: both are small enough to look obviously right and
// wrong in a way that produces plausible answers rather than failures.
struct StepValue
{
    bool valid;      ///< false until the first value has been published
    double value;    ///< what the broker was last told
    uint32_t lastMs; ///< when, from millis()
};

// True when this key should go out now. Records the value as published when it
// says yes, and leaves the record untouched when it says no.
//
// Three rules, each of which was a defect in some earlier shape of this:
//
//  - The FIRST value is always published. A key absent from the cloud is
//    indistinguishable from a device that never booted, and change-based
//    publishing has to start from something.
//
//  - A float is compared against a DEADBAND, never with !=. Two doubles that
//    represent the same measurement differ in their last bits for ever, so an
//    exact comparison would report a change on every single tick and the whole
//    mechanism would buy nothing. `deadband` is the smallest change worth a
//    datapoint on that channel — a question about the sensor, not about
//    floating point. Exact keys (counters, contacts) pass 0.0, which is correct
//    rather than sloppy: their values are whole numbers a double holds exactly.
//
//  - The heartbeat is compared with UNSIGNED subtraction. `now - lastMs` wraps
//    correctly when millis() rolls over at 49 days; `now < lastMs` does not, and
//    would stall every key for another 49 days at the one moment a long-running
//    device most needs to prove it is alive.
inline bool
stepValueDue(StepValue& step,
             double value,
             double deadband,
             uint32_t nowMs,
             uint32_t heartbeatMs)
{
    if (!step.valid) {
        step.valid = true;
        step.value = value;
        step.lastMs = nowMs;
        return true;
    }

    const bool stale = (uint32_t)(nowMs - step.lastMs) >= heartbeatMs;
    if (fabs(value - step.value) <= deadband && !stale) {
        return false;
    }

    step.value = value;
    step.lastMs = nowMs;
    return true;
}
