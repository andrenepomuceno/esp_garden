#include "core/logger.h"
#include "SPIFFS.h"

#define MAX_LOG_FILES 4

Logger::Logger()
  : buffer(""), logLevel(LOG_INFO)
{
    Serial.begin(115200);

    mutex = xSemaphoreCreateMutex();
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

void
Logger::setLogLevel(LogLevel level)
{
    logLevel = level;
}

int
Logger::print(const String& str)
{
    auto len = str.length();

    if (len <= 0) {
        return 0;
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    Serial.print(str);

    buffer += str;

    while (buffer.length() > BUFFER_SIZE) {
        auto endlineIdx = buffer.indexOf('\n');
        auto removed = endlineIdx + 1;
        buffer.remove(0, removed);

        if (bufferOffset > removed) {
            bufferOffset -= removed;
        } else {
            bufferOffset = 0;
        }
    }

    xSemaphoreGive(mutex);

    return buffer.length();
}

int
Logger::writeLine(LogLevel level, const String& str)
{
    if (logLevel < level || level > LOG_TRACE || level == LOG_DISABLE) {
        return 0;
    }

    static const char* levelStr[] = {
        "", "F", "E", "W", "I", "D", "T",
    };

    char timestamp[32];
    auto now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%F %T", localtime(&now));

    String formatted = "[" + String(timestamp) + "] [" +
                       String(levelStr[(int)level]) + "] " + str + "\n";
    return print(formatted);
}

void Logger::fatal(const String& str)   { writeLine(LOG_FATAL, str); }
void Logger::error(const String& str)   { writeLine(LOG_ERROR, str); }
void Logger::warning(const String& str) { writeLine(LOG_WARNING, str); }
void Logger::info(const String& str)    { writeLine(LOG_INFO, str); }
void Logger::debug(const String& str)   { writeLine(LOG_DEBUG, str); }
void Logger::trace(const String& str)   { writeLine(LOG_TRACE, str); }

int
Logger::println(const String& str)
{
    return writeLine(LOG_INFO, str);
}

String&
Logger::read()
{
    return buffer;
}

void
Logger::backupSetup()
{
    logger.info("Log setup...");

    const String currentFilename("/current.txt");
    File currentFile;

    if (!SPIFFS.exists(currentFilename)) {
        logger.info(String(currentFilename) +
                    " do not exists. Creating one...");

        currentFile = SPIFFS.open(currentFilename, FILE_WRITE, true);
        if (currentFile == false) {
            logger.error("Failed to create " + String(currentFilename));
            return;
        }

        currentFile.print('0');
        currentFile.close();

        currentLog = 0;
    } else {
        currentFile = SPIFFS.open(currentFilename, FILE_READ);
        currentLog = (currentFile.readString().toInt() + 1) % MAX_LOG_FILES;
        currentFile.close();

        logger.info("Updating " + String(currentFilename));
        currentFile = SPIFFS.open(currentFilename, FILE_WRITE);
        currentFile.print(String(currentLog));
        currentFile.close();
    }

    logger.info("Current log: log" + String(currentLog) + ".txt");
    logger.info("Log setup done!");
}

void
Logger::backup()
{
    String logFilename = "/log" + String(currentLog) + ".txt";

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    if (logOffset == 0) {
        auto logFile = SPIFFS.open(logFilename, FILE_WRITE, true);
        if (logFile == false) {
            xSemaphoreGive(mutex);
            logger.error("Failed to open " + String(logFilename));
            return;
        }

        logFile.print(buffer);
        logFile.close();

        logOffset = buffer.length();
        bufferOffset = logOffset;
    } else {
        auto logFile = SPIFFS.open(logFilename, FILE_APPEND);
        if (logFile == false) {
            xSemaphoreGive(mutex);
            logger.error("failed to open " + String(logFilename));
            return;
        }

        String data = buffer.substring(bufferOffset);
        size_t len = data.length();
        logFile.print(data);
        logFile.close();

        logOffset += len;
        bufferOffset += len;
    }

    xSemaphoreGive(mutex);
}