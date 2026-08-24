#include "core/accumulator_v2.h"

#include <math.h>
#include <new>

AccumulatorV2::AccumulatorV2(unsigned windowLength)
  : variance(0.0f)
  , samples(nullptr)
  , capacity(0)
  , count(0)
  , head(0)
  , sinceResync(0)
  , sum(0.0)
  , sumSq(0.0)
  , last(0.0f)
{
    setMaxLen(windowLength);
}

AccumulatorV2::~AccumulatorV2()
{
    delete[] samples;
}

void
AccumulatorV2::setMaxLen(unsigned len)
{
    if (len == 0 || len == capacity) {
        return;
    }

    // new[] rather than calloc so the host test's operator-new counter can see
    // it. That is not cosmetic: with calloc the "steady state allocates
    // nothing" assertion would pass even if this class allocated on every
    // sample, which is the exact bug the test exists to catch.
    //
    // nothrow because the null check below is the intended failure path — this
    // runs on a device with no exception handling worth the name.
    float* grown = new (std::nothrow) float[len]();
    if (grown == nullptr) {
        // Keeping the old window is the safe failure: the channel carries on
        // with the length it had rather than losing its history or, worse,
        // pointing at nothing.
        return;
    }

    // Copy the most recent min(count, len) samples, oldest first, so shrinking
    // a window drops the OLDEST rather than truncating arbitrarily — which is
    // what the list version's pop_front loop did.
    const unsigned keep = (count < len) ? count : len;
    for (unsigned i = 0; i < keep; ++i) {
        const unsigned from = (head + capacity - keep + i) % capacity;
        grown[i] = samples[from];
    }

    delete[] samples;
    samples = grown;
    capacity = len;
    count = keep;
    head = (keep == len) ? 0 : keep;

    resync();
}

void
AccumulatorV2::resync()
{
    sum = 0.0;
    sumSq = 0.0;
    for (unsigned i = 0; i < count; ++i) {
        const double v = samples[i];
        sum += v;
        sumSq += v * v;
    }
    sinceResync = 0;
}

void
AccumulatorV2::add(float value)
{
    if (capacity == 0 || !isfinite(value)) {
        // A non-finite sample would poison the running sums permanently — every
        // later mean and variance would be NaN with no way back short of a
        // reboot. Dropping it costs one reading.
        return;
    }

    if (count == capacity) {
        // Full: the slot about to be overwritten leaves the window.
        const double old = samples[head];
        sum -= old;
        sumSq -= old * old;
    } else {
        ++count;
    }

    samples[head] = value;
    sum += value;
    sumSq += (double)value * value;
    head = (head + 1) % capacity;
    last = value;

    // Incremental add/subtract accumulates rounding error over millions of
    // updates — a channel sampled at 1 Hz sees three million in a month. One
    // full recompute per window keeps it bounded at amortised O(1).
    if (++sinceResync >= capacity) {
        resync();
    }
}

float
AccumulatorV2::getLast() const
{
    // The web server answers before the first sensor task has run.
    return (count == 0) ? 0.0f : last;
}

float
AccumulatorV2::getAverage()
{
    if (count == 0) {
        variance = 0.0f;
        return 0.0f;
    }

    const double mean = sum / (double)count;

    // E[x^2] - E[x]^2 can go slightly negative through cancellation when the
    // spread is tiny next to the mean — which is exactly this data: readings
    // cluster near 40 with a spread under 1.
    double v = (sumSq / (double)count) - (mean * mean);
    if (!(v > 0.0)) {
        v = 0.0;
    }

    variance = (float)v;
    return (float)mean;
}

unsigned
AccumulatorV2::getSamples() const
{
    return count;
}
