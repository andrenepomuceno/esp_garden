#include "measure.h"

Measure::Measure()
  : last(0.0)
  , sum(0.0)
  , samples(0)
{}

void
Measure::add(float value)
{
  last = value;
  sum += value;
  ++samples;
}

float
Measure::getLast()
{
  return last;
}

float
Measure::getAverage()
{
  if (samples == 0) {
    return last;
  }
  return (sum / samples);
}

void
Measure::resetAverage()
{
  sum = 0.0;
  samples = 0;
}