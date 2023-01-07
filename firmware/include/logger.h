#pragma once
#include <Arduino.h>

class Logger
{
  public:
    static Logger& instance();

    int println(const String& str);
    String& read();

    void dumpToFSSetup();
    void dumpToFS();

  private:
    Logger();

    int print(const String& str);
    int print(int i);

    static const unsigned BUFFER_SIZE = 4096;
    String buffer;
    bool writing = false;
    int currentLog;
};

#define logger Logger::instance()