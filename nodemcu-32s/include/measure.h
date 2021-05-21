class Measure
{
public:
  Measure();

  void add(float value);
  float getLast();
  float getAverage();
  void resetAverage();

private:
  float last;
  float sum;
  unsigned samples;
};
