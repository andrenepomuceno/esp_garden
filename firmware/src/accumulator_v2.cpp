#include "accumulator_v2.h"

AccumulatorV2::AccumulatorV2(unsigned maxLen)
  : lastAvg(0.0)
  , maxLen(maxLen)
{
}

void
AccumulatorV2::add(const float value)
{
    sampleList.push_back(value);
    if (sampleList.size() > maxLen) {
        sampleList.pop_front();
    }
}

float
AccumulatorV2::getLast() const
{
    return sampleList.back();
}

float
AccumulatorV2::getAverage() const
{   
    if (sampleList.size() == 0) return 0;

    float sum = 0;
    for (auto v : sampleList) {
        sum += v;
    }
    return sum / sampleList.size();
}

unsigned
AccumulatorV2::getSamples()
{
    return sampleList.size();
}