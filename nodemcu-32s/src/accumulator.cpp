#include "accumulator.h"

Accumulator::Accumulator()
  : last(0.0)
  , sum(0.0)
  , samples(0)
{}

void
Accumulator::add(float value)
{
  last = value;
  sum += value;
  ++samples;
}

float
Accumulator::getLast()
{
  return last;
}

float
Accumulator::getAverage()
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