#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum LogLevel {
    LOG_DISABLE = 0,
    LOG_FATAL = 1,
    LOG_ERROR = 2,
    LOG_WARNING = 3,
    LOG_INFO = 4,
    LOG_DEBUG = 5,
    LOG_TRACE = 6,
};

class Logger
{
  public:
    static Logger& instance();

    void setLogLevel(LogLevel level);

    int println(const String& str); // legacy: routes to info()

    void fatal(const String& str);
    void error(const String& str);
    void warning(const String& str);
    void info(const String& str);
    void debug(const String& str);
    void trace(const String& str);

    String& read();

    void backupSetup();
    void backup();

  private:
    Logger();

    int print(const String& str);
    int writeLine(LogLevel level, const String& str);

    static const unsigned BUFFER_SIZE = 8 * 1024;
    String buffer;
    LogLevel logLevel;
    SemaphoreHandle_t mutex;
    int currentLog;
    unsigned logOffset;
    unsigned bufferOffset;
};

#define logger Logger::instance()