#pragma once

#include <stdint.h>
#include <string.h>

namespace probe_sampling {

static const unsigned kCapacity = 64;

struct Window
{
    uint16_t elapsedMs[kCapacity];
    uint16_t adc[kCapacity];
    uint16_t count;
    uint16_t taken;
};

inline void
reset(Window& window)
{
    window.count = 0;
    window.taken = 0;
}

inline void
add(Window& window, uint32_t elapsedMs, uint16_t adc)
{
    if (window.count == kCapacity) {
        memmove(window.elapsedMs,
                window.elapsedMs + 1,
                (kCapacity - 1) * sizeof(window.elapsedMs[0]));
        memmove(
          window.adc, window.adc + 1, (kCapacity - 1) * sizeof(window.adc[0]));
    } else {
        ++window.count;
    }
    window.elapsedMs[window.count - 1] =
      (uint16_t)(elapsedMs > 0xFFFFu ? 0xFFFFu : elapsedMs);
    window.adc[window.count - 1] = adc;
    if (window.taken < 0xFFFFu) {
        ++window.taken;
    }
}

inline unsigned
dropped(const Window& window)
{
    return (unsigned)window.taken - window.count;
}

inline uint32_t
nextSampleAt(uint32_t elapsedMs, uint32_t settleMs, uint32_t intervalMs)
{
    if (intervalMs == 0 || elapsedMs >= settleMs) {
        return settleMs;
    }
    const uint32_t next = (elapsedMs / intervalMs + 1) * intervalMs;
    return next < settleMs ? next : settleMs;
}

struct Cycle
{
    uint32_t settleMs;
    uint32_t intervalMs;
    uint32_t nextAtMs;
    uint16_t taken;
    bool active;
};

enum Action
{
    kWait,
    kSample,
    kSampleAndFinish
};

inline void
begin(Cycle& cycle, uint32_t settleMs, uint32_t intervalMs)
{
    cycle.settleMs = settleMs;
    cycle.intervalMs = intervalMs;
    cycle.nextAtMs = nextSampleAt(0, settleMs, intervalMs);
    cycle.taken = 0;
    cycle.active = true;
}

inline Action
step(Cycle& cycle, uint32_t elapsedMs)
{
    if (!cycle.active || elapsedMs < cycle.nextAtMs) {
        return kWait;
    }
    if (cycle.taken < 0xFFFFu) {
        ++cycle.taken;
    }
    if (elapsedMs >= cycle.settleMs) {
        cycle.active = false;
        return kSampleAndFinish;
    }
    cycle.nextAtMs =
      nextSampleAt(elapsedMs, cycle.settleMs, cycle.intervalMs);
    return kSample;
}

inline uint32_t
waitMs(const Cycle& cycle, uint32_t elapsedMs, uint32_t floorMs)
{
    const bool finalAfterWindow =
      cycle.taken > 0 && cycle.nextAtMs >= cycle.settleMs;
    const uint32_t minimum = finalAfterWindow ? 1 : floorMs;
    if (cycle.nextAtMs <= elapsedMs) {
        return minimum;
    }
    const uint32_t wait = cycle.nextAtMs - elapsedMs;
    return wait > minimum ? wait : minimum;
}

inline unsigned
samplesPerWindow(uint32_t settleMs, uint32_t intervalMs, uint32_t floorMs)
{
    Cycle cycle;
    begin(cycle, settleMs, intervalMs);
    uint32_t now = 0;
    for (;;) {
        const Action action = step(cycle, now);
        if (action == kSampleAndFinish) {
            return cycle.taken;
        }
        now += waitMs(cycle, now, floorMs);
    }
}

inline double
tailMedian(const Window& window, unsigned tail, unsigned* used)
{
    unsigned n = tail < window.count ? tail : window.count;
    if (used != nullptr) {
        *used = n;
    }
    if (n == 0) {
        return 0.0;
    }

    uint16_t sorted[kCapacity];
    memcpy(sorted, window.adc + (window.count - n), n * sizeof(sorted[0]));
    for (unsigned i = 1; i < n; ++i) {
        const uint16_t value = sorted[i];
        unsigned j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            --j;
        }
        sorted[j] = value;
    }

    if (n % 2 == 1) {
        return sorted[n / 2];
    }
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
}

} // namespace probe_sampling
