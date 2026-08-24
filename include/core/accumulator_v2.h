#pragma once

#include <stddef.h>

// A rolling window of the last N samples, with its mean and variance.
//
// WHY THIS IS A RING BUFFER AND NOT A std::list
//
// It used to be a `std::list<float>` with a push_back and a pop_front on every
// sample, at 1 Hz, forever — an allocate/free pair per second per channel for
// the life of the device. That contradicted the "no dynamic allocation in
// steady state" rule this firmware inherited, and on a board with 320 KB of
// DRAM and no PSRAM it is a fragmentation source that never stops.
//
// The second cost was worse and less obvious: `getAverage()` walked the list
// TWICE — once for the mean, once for the variance — so reading a channel was
// O(n) in pointer-chased heap nodes. Putting that call inside a spinlock
// panicked the board with an interrupt watchdog timeout. Now it is O(1), which
// removes the whole class of mistake rather than one instance of it.
//
// The window is allocated ONCE, when the length is set. Nothing allocates after
// that, and `add()` is a store plus two running sums.
class AccumulatorV2
{
  public:
    explicit AccumulatorV2(unsigned windowLength = 120);
    ~AccumulatorV2();

    // Deleted, not implemented. The std::list version was safely copyable and
    // this one owns a raw buffer, so a copy would delete[] the same pointer
    // twice — a double free with no compiler warning, found only when it
    // crashes. Nothing copies one today, which is exactly why the next person
    // to write `AccumulatorV2 snapshot = g_luminosity;` should get a compile
    // error instead.
    AccumulatorV2(const AccumulatorV2&) = delete;
    AccumulatorV2& operator=(const AccumulatorV2&) = delete;

    // Resize the window after construction. Needed because an array of
    // accumulators cannot pass a constructor argument, so array members would
    // otherwise be stuck on the default while every scalar accumulator tracks
    // the MQTT period.
    //
    // This is the ONLY method that allocates. Call it at setup, not from a
    // task.
    void setMaxLen(unsigned len);

    void add(float value);

    float getLast() const;

    // O(1): the running sums are maintained by add(). `variance` is refreshed
    // here as well, which is why it is only meaningful once this has been
    // called — the same contract the list version had.
    float getAverage();

    unsigned getSamples() const;

    float variance;

  private:
    // Recomputes the sums from the stored samples. Incremental add/subtract
    // drifts over millions of updates, so this runs once per full window: the
    // cost is one O(n) pass per n samples, i.e. amortised O(1), instead of the
    // O(n) per READ the list version paid.
    void resync();

    float* samples;   // windowLength entries, allocated once
    unsigned capacity;
    unsigned count;   // how many are valid, <= capacity
    unsigned head;    // next slot to write
    unsigned sinceResync;

    double sum;
    double sumSq;
    float last;
};
