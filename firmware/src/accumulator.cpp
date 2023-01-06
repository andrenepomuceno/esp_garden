#include "accumulator.h"

Accumulator::Accumulator()
  : last(0.0)
  , sum(0.0)
  , samples(0)
  , lastAvg(0.0)
{}

void
Accumulator::add(const float value)
{
    last = value;
    sum += value;
    ++samples;
}

float
Accumulator::getLast() const
{
    return last;
}

float
Accumulator::getAverage() const
{
    if (samples == 0) {
        return last;
    }
    return (sum / samples);
}

void
Accumulator::resetAverage()
{
    lastAvg = sum/samples;
    sum = 0.0;
    samples = 0;
}

unsigned
Accumulator::getSamples()
{
    return samples;
}

float
Accumulator::getLastAvg() const
{
    return lastAvg;
}