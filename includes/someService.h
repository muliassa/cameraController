
#ifndef SERVICE_H
#define SERVICE_H

#include <string>
#include <iostream>     // std::cout
#include <algorithm>    // std::sort
#include <vector>       // std::vector
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

class someService {
    json config;
    string server;
    string host;
    string serviceName;
public:
    explicit someService(json config, string serviceName);
    void post_status(string status);
    void post_response(json, string status, json response = json());
    void run();
};

#endif //ALHAPANIM_SERVICE_H
