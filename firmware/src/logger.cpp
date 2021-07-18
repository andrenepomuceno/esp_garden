#include "logger.h"

Logger::Logger()
  : buffer("")
{
    Serial.begin(115200);
}

Logger&
Logger::instance()
{
    static Logger _logger;
    return _logger;
}

int
Logger::print(const String& str)
{
    auto len = str.length();

    if (len <= 0) {
        return 0;
    }

    Serial.print(str);

    buffer += str;

    while (buffer.length() > BUFFER_SIZE) {
        auto endlineIdx = buffer.indexOf('\n');
        buffer.remove(0, endlineIdx + 1);
    }

    return buffer.length();
}

int
Logger::print(int i)
{
    return print(String(i));
}

int
Logger::println(const String& str)
{
    char timestamp[32];
    auto now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%F %T", localtime(&now));
    return print("[" + String(timestamp) + "] " + str + "\n");
}

String&
Logger::read()
{
    return buffer;
}
