//
// Created by muli on 11/22/23.
//
#include <chrono>
#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <someLogger.h>

someLogger *someLogger::_instance;

someLogger *someLogger::getInstance(string filename, someLogLevel level) {
    if (_instance == nullptr)
        _instance = new someLogger(filename, level);
    return _instance;
}

someLogger *someLogger::getInstance() {
    return _instance;
}

someLogger::someLogger(string filename, someLogLevel level) {
    fp = fopen(filename.c_str(), "w");
    defaultLogLevel = level;
}

void someLogger::log(string message, Colors color, someLogLevel override) {
    someLogLevel level = override == someLogLevel::DEFAULT ? defaultLogLevel : override;
    // timeStamp
    uint64_t ts = timeSinceEpochMilli();
    // uint64_t diff = last > 0 ? ts - last : 0;
    // if (last > 0) fprintf(fp, "+");
    auto dateString = getCurrentDateString();
    last = ts;
    fprintf(fp, "%s: %s\n", dateString.c_str(), message.c_str());
    fflush(fp);
    if (level == someLogLevel::DEBUG) {
//        fprintf(stdout, "\033[1;%dm", color);
        fprintf(stdout, "%s\n", message.c_str());
    }
}

void someLogger::error(string message) {
    log("[ERROR] " + message);
}

void someLogger::close() {
    fclose(fp);
}

time_t someLogger::fileTimeToTimeT(const fs::file_time_type& ftime) {
    // Convert file_time to system_clock time_point
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );    
    // Convert to time_t
    return std::chrono::system_clock::to_time_t(sctp);
}

string someLogger::getTimeString(time_t timeStamp) {
    // Format the time as desired
    stringstream ss;
    ss << std::put_time(std::localtime(&timeStamp), "%H:%M:%S");    
    return ss.str();
}
 
string someLogger::getTimeString(const fs::file_time_type& ftime) {
    return getTimeString(fileTimeToTimeT(ftime));
}

string someLogger::getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    return getTimeString(current_time);
}

string someLogger::getCurrentDateString() {
    // Get current time
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    
    // Format the time as desired
    stringstream ss;
    ss << std::put_time(std::localtime(&current_time), "%Y-%m-%d %H:%M:%S");
    
    return ss.str();
}

time_t someLogger::now() {
    // Get current time
    auto clock = std::chrono::system_clock::now();
    return std::chrono::system_clock::to_time_t(clock);
}

uint64_t someLogger::timeSinceEpochMilli() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

vector<string> someLogger::split(const string& str, char delimiter) {
    vector<std::string> tokens;
    stringstream ss(str);
    string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

json someLogger::loadConfig(string path) {
    ifstream ifs(path);
    return json::parse(ifs);
}
