#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <string>
#include <iostream>
#include <chrono>
#include <nlohmann/json.hpp>

#include <zcam.h>
#include <overlays.h>

using namespace std;
using json = nlohmann::json;

class ZCAMSnapshot {

	json config;
	string root;
	int cam_idx;
	string cam_name;
	ZCAM * zcam;
	std::unique_ptr<FrameOverlayProcessor> overlayProcessor;

public:
    explicit ZCAMSnapshot(json config);
	string take();
};

#endif