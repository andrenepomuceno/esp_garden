#include "logger.h"
#include "SPIFFS.h"

#define MAX_LOG_FILES 4

Logger::Logger()
  : buffer("")
{
    Serial.begin(115200);

    currentLog = -1;
    bufferOffset = 0;
    logOffset = 0;
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

        bufferOffset -= (endlineIdx + 1);
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

void
Logger::dumpToFSSetup()
{
    logger.println("Log setup...");

    const String currentFilename("/current.txt");    
    File currentFile;

    if (!SPIFFS.exists(currentFilename)) {
        logger.println(String(currentFilename) +
                       " do not exists. Creating one...");

        currentFile = SPIFFS.open(currentFilename, FILE_WRITE, true);
        if (currentFile == false) {
            logger.println("Failed to create " + String(currentFilename));
            return;
        }

        currentFile.print('0');
        currentFile.close();

        currentLog = 0;
    } else {
        currentFile = SPIFFS.open(currentFilename, FILE_READ);
        currentLog = (currentFile.readString().toInt() + 1) % MAX_LOG_FILES;
        currentFile.close();

        logger.println("Updating " + String(currentFilename));
        currentFile = SPIFFS.open(currentFilename, FILE_WRITE);
        currentFile.print(String(currentLog));
        currentFile.close();
    }

    logger.println("Current log: log" + String(currentLog) + ".txt");
    logger.println("Log setup done!");
}

void
Logger::dumpToFS()
{
    println("Starting log backup...");

    String logFilename = "/log" + String(currentLog) + ".txt";
    if (logOffset == 0) {
        auto logFile = SPIFFS.open(logFilename, FILE_WRITE, true);
        if (logFile == false) {
            logger.println("Failed to open " + String(logFilename));
            return;
        }

        logFile.print(buffer);
        logFile.close();

        logOffset = buffer.length();
        bufferOffset = logOffset;
    } else {
        auto logFile = SPIFFS.open(logFilename, FILE_APPEND);
        if (logFile == false) {
            logger.println("failed to open " + String(logFilename));
            return;
        }

        const uint8_t* pos = (const uint8_t*)buffer.c_str() + bufferOffset;
        size_t len = buffer.length() - bufferOffset;
        logFile.write(pos, len);
        logFile.close();

        logOffset += len;
        bufferOffset += len;
    }

    println("Log backup done!");
}