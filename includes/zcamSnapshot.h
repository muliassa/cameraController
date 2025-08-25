#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <string>
#include <chrono>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

class ZCAMSnapshot {
	json config;
	string root;
	string cam_id;
public:
    explicit ZCAMSnapshot(json config);
	string take();
};

#endif