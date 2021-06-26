#pragma once
#include <Arduino.h>

class Logger
{
  public:
    static Logger& instance();

    int println(const String& str);
    String& read();

  private:
    Logger();

    int print(const String& str);
    int print(int i);

    static const unsigned BUFFER_SIZE = 4096;
    String buffer;
    bool writing = false;
};

#define logger Logger::instance()