#pragma once
#include <list>

class AccumulatorV2
{
  public:
    AccumulatorV2(unsigned firLen = 120);

    // Resize the window after construction. Needed because an array of
    // accumulators cannot pass a constructor argument, so array members would
    // otherwise be stuck on the default while every scalar accumulator tracks
    // the MQTT period.
    void setMaxLen(unsigned len);

    void add(const float value);
    float getLast() const;
    float getAverage();
    unsigned getSamples();

    float variance;

  private:
    std::list<float> sampleList;
    float lastAvg;
    unsigned maxLen;
};
