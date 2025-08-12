//
// Created by muli on 11/22/23.
//
#include <chrono>
#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <peocl_logger.h>

PeoclLogger *PeoclLogger::_instance;

PeoclLogger *PeoclLogger::getInstance(string filename, PeoclLogLevel level) {
    if (_instance == nullptr)
        _instance = new PeoclLogger(filename, level);
    return _instance;
}

PeoclLogger *PeoclLogger::getInstance() {
    return _instance;
}

PeoclLogger::PeoclLogger(string filename, PeoclLogLevel level) {
    fp = fopen(filename.c_str(), "w");
    defaultLogLevel = level;
}

void PeoclLogger::log(string message, Colors color, PeoclLogLevel override) {
    PeoclLogLevel level = override == PeoclLogLevel::DEFAULT ? defaultLogLevel : override;
    // timeStamp
    uint64_t ts = timeSinceEpochMilli();
    // uint64_t diff = last > 0 ? ts - last : 0;
    // if (last > 0) fprintf(fp, "+");
    auto dateString = getCurrentDateString();
    last = ts;
    fprintf(fp, "%s: %s\n", dateString.c_str(), message.c_str());
    fflush(fp);
    if (level == PeoclLogLevel::DEBUG) {
//        fprintf(stdout, "\033[1;%dm", color);
        fprintf(stdout, "%s\n", message.c_str());
    }
}

void PeoclLogger::error(string message) {
    log("[ERROR] " + message);
}

void PeoclLogger::close() {
    fclose(fp);
}

time_t PeoclLogger::fileTimeToTimeT(const fs::file_time_type& ftime) {
    // Convert file_time to system_clock time_point
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );    
    // Convert to time_t
    return std::chrono::system_clock::to_time_t(sctp);
}

string PeoclLogger::getTimeString(time_t timeStamp) {
    // Format the time as desired
    stringstream ss;
    ss << std::put_time(std::localtime(&timeStamp), "%H:%M:%S");    
    return ss.str();
}
 
string PeoclLogger::getTimeString(const fs::file_time_type& ftime) {
    return getTimeString(fileTimeToTimeT(ftime));
}

string PeoclLogger::getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    return getTimeString(current_time);
}

string PeoclLogger::getCurrentDateString() {
    // Get current time
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    
    // Format the time as desired
    stringstream ss;
    ss << std::put_time(std::localtime(&current_time), "%Y-%m-%d %H:%M:%S");
    
    return ss.str();
}

time_t PeoclLogger::now() {
    // Get current time
    auto clock = std::chrono::system_clock::now();
    return std::chrono::system_clock::to_time_t(clock);
}

uint64_t PeoclLogger::timeSinceEpochMilli() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

vector<string> PeoclLogger::split(const string& str, char delimiter) {
    vector<std::string> tokens;
    stringstream ss(str);
    string token;
    while (std::getline(ss, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}



