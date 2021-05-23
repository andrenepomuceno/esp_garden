#pragma once

class Accumulator
{
public:
  Accumulator();

  void add(float value);
  float getLast();
  float getAverage();
  void resetAverage();

private:
  float last;
  float sum;
  unsigned samples;
};
