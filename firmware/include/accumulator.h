#pragma once

class Accumulator
{
  public:
    Accumulator();

    void add(const float value);
    float getLast() const;
    float getAverage() const;
    float getLastAvg() const;
    void resetAverage();
    unsigned getSamples();

  private:
    float last;
    float sum;
    unsigned samples;
    float lastAvg;
};
