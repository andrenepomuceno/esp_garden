#pragma once
#include <list>

class AccumulatorV2
{
  public:
    AccumulatorV2(unsigned firLen = 120);

    void add(const float value);
    float getLast() const;
    float getAverage() const;
    unsigned getSamples();

  private:
    std::list<float> sampleList;
    float lastAvg;
    unsigned maxLen;
};
