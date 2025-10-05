
#ifndef SOME_SERVICE_H
#define SOME_SERVICE_H

#include <string>
#include <iostream>     // std::cout
#include <algorithm>    // std::sort
#include <vector>       // std::vector
#include <functional>
#include <nlohmann/json.hpp>

#include <zcamSnapshot.h>

using namespace std;
using namespace nlohmann;

class someService {
    json config;
    string server;
    string host;
    string serviceName;
    ZCAMSnapshot* snapshotService;
    void post_status(string status);
    void post_response(json, string status, json response = json());
public:
    explicit someService(json config, string serviceName);
    function<void(json)> onMessage;
    void run();
};

#endif //ALHAPANIM_SERVICE_H
