#pragma once

class Accumulator
{
  public:
    Accumulator();

    void add(const float value);
    float getLast() const;
    float getAverage() const;
    void resetAverage();

  private:
    float last;
    float sum;
    unsigned samples;
};
