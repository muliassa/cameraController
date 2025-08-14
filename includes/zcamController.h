#ifndef ZCAM_CONTROLLER_H
#define ZCAM_CONTROLLER_H

#include <string>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;

class ZCAMController {
	
private:
	bool stop = false;
	string server;
    int start_hour = 6;   // 6 AM
    int end_hour = 22;    // 10 PM

    string camera_ip;
    string camera_id;
    string rtsp_url;
    string http_base_url;

	bool isOperatingHours();

public:
    ZCAMController(const json& config, const int cam_idx);

};

#endif