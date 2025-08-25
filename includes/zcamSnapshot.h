#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <string>
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>

#include <zcam.h>

using namespace std;
using json = nlohmann::json;

class ZCAMSnapshot {
	
	json config;
	string root;
	string cam_id;
	string cam_name;
	ZCAM * zcam;

public:
    explicit ZCAMSnapshot(json config);
	string take();
};

#endif