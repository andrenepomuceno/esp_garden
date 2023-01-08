#pragma once
#include <Arduino.h>

class Logger
{
  public:
    static Logger& instance();

    int println(const String& str);
    String& read();

    void backupSetup();
    void backup();

  private:
    Logger();

    int print(const String& str);
    int print(int i);

    static const unsigned BUFFER_SIZE = 4096;
    String buffer;
    bool writing = false;
    int currentLog;
    unsigned logOffset;
    unsigned bufferOffset;
};

#define logger Logger::instance()