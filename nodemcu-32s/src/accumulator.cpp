#include "accumulator.h"

Accumulator::Accumulator()
  : last(0.0)
  , sum(0.0)
  , samples(0)
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
    sum = 0.0;
    samples = 0;
}