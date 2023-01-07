#include "logger.h"
#include "SPIFFS.h"

#define MAX_LOG_FILES 4

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

void
Logger::dumpToFSSetup()
{
    const String currentFilename("/current.txt");
    currentLog = -1;
    File currentFile;

    if (!SPIFFS.exists(currentFilename)) {
        logger.println(String(currentFilename) +
                       " do not exists. Creating one...");

        currentFile = SPIFFS.open(currentFilename, FILE_WRITE, true);
        if (!currentFile) {
            logger.println("Failed to create " + String(currentFilename));
            return;
        }

        currentFile.print('1');
        currentFile.close();

        currentLog = 0;
    } else {
        currentFile = SPIFFS.open(currentFilename, FILE_READ);
        currentLog = (currentFile.readString().toInt()) % MAX_LOG_FILES;
        logger.println("currentLog = " + String(currentLog));
        currentFile.close();

        logger.println("Updating " + String(currentFilename));
        currentFile = SPIFFS.open(currentFilename, FILE_WRITE);
        int nextLog = (currentLog + 1) % MAX_LOG_FILES;
        currentFile.print(String(nextLog));
        currentFile.close();
    }
}

void
Logger::dumpToFS()
{
    String logFilename = "/log" + String(currentLog) + ".txt";
    auto logFile = SPIFFS.open(logFilename, FILE_WRITE, true);
    if (!logFile) {
        logger.println("failed to open " + String(logFilename));
        return;
    }
    logFile.print(buffer);
    logFile.close();
}