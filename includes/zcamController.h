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
	bool readCurrentSettings();
	bool initializeStream();
	bool captureFrame(std::vector<uint8_t>& rgb_data, int& width, int& height);

public:
    ZCAMController(const json& config, const int cam_idx);
    void stop();

};

#endif