#ifndef LOGGER_H
#define LOGGER_H

#include <vector>
#include <string>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

using namespace std;
using namespace nlohmann;
namespace fs = filesystem;

enum class someLogLevel { ERROR, INFO, DEBUG, DEFAULT };
enum class Colors { BLACK=30, RED=31, GREEN=32, YELLOW=33, BLUE=34, MAGENTA=35};

class someLogger {
private:
    FILE *fp;
    someLogLevel defaultLogLevel;
    uint64_t last = 0;
    static someLogger *_instance;
    someLogger(string filename, someLogLevel level);
public:
    static someLogger *getInstance(string filename, someLogLevel = someLogLevel::INFO);
    static someLogger *getInstance();
    static string getTimeString(time_t timeStamp);
    static string getTimeString(const fs::file_time_type& ftim);
    static time_t fileTimeToTimeT(const fs::file_time_type& ftime);
    static string getCurrentTimeString();
    static string getCurrentDateString();
    static time_t now();
    static uint64_t timeSinceEpochMilli();
    static vector<string> split(const string& str, char delimiter);
    json loadConfig(string path);
    someLogLevel getDefault() { return defaultLogLevel; };
    void log(string message, Colors = Colors::BLACK, someLogLevel = someLogLevel::DEFAULT);
    void error(string message);
    void close();
};

#endif //LOGGER_H
