#ifndef ZCAM_CONTROLLER_H
#define ZCAM_CONTROLLER_H

#include <string>
#include <iostream>
#include <nlohmann/json.hpp>

#include <someNetwork.h>

using namespace std;
using json = nlohmann::json;

// Simplified metrics - focus only on exposure
struct ExposureMetrics {
    double brightness = 0.0;          // 0-255 (target: ~128-150 for surf)
    double contrast = 0.0;            // Standard deviation
    double highlights_clipped = 0.0;  // % pixels >250
    double shadows_clipped = 0.0;     // % pixels <5
    double exposure_score = 0.0;      // Overall quality 0-100
    int total_pixels = 0;
};

// Camera state
struct CameraSettings {
    int iso = 500;
    string iris = "10";          // f-stop
    double target_brightness = 140.0; // Slightly brighter for surf
    double brightness_tolerance = 20.0;
    
    // Iris constraints for surf
    string min_iris = "8";       // f/8 - good contrast, not too wide
    string max_iris = "11";      // f/11 - reasonable light control
};

struct CameraState {
    // Current settings (would be read from camera API)
    int current_iso = 500;
    nlohmann::json iso_options;

    double current_iris = 10.0;
    nlohmann::json iris_options;

    double current_ev = 0.0;
    string current_aperture = "5.6";
    nlohmann::json ev_options;

    int current_shutter_angle = 180;
    nlohmann::json shutter_options;

    // Scene analysis
    double sun_factor = 0.5;
    string scene_type = "unknown";

    int temperature = 0;
    
    // Targets
    double target_brightness = 128.0;
    double brightness_tolerance = 15.0;
};

class ZCAMController {

private:
	
	atomic<bool> keep_running{true};

	string root;
    string camera_ip;
    string camera_id;
    string rtsp_url;
    string http_base_url;

	string snapshot;
	int refresh = 5;
	bool auto_adjust = false;
	
	bool stop = false;
	string server;
	string host;
    int start_hour = 6;   // 6 AM
    int end_hour = 22;    // 10 PM


    CameraSettings settings;
    CameraState camera_state;
	ExposureMetrics metrics;

    int adjustment_count = 0;

	bool isOperatingHours();
	bool readCurrentSettings();

    someNetwork::Response httpRequest(const string& endpoint, const string& method = "GET", const string& data = "");

    ExposureMetrics analyzeExposure(const vector<uint8_t>& rgb_data, int width, int height);
    bool adjustExposure(const ExposureMetrics& metrics);
    bool applySetting(const string& param, const string& value);
	bool initializeStream();
    bool detectVideoStream();
	bool captureFrame(vector<uint8_t>& rgb_data, int& width, int& height);
    bool monitorCam();
    void cleanup();

public:
    ZCAMController(const json& config, const int cam_idx);
    ~ZCAMController();
    void run();
    void shutdown();

};

#endif