#include <string>
#include <iostream>

#include <zcamController.h>
#include <someService.h>
#include <someLogger.h>

using namespace std;
using json = nlohmann::json;

json config;
string root;
ZCAMController *left;
ZCAMController *right;

/*
bool monitorCamera(ZCAMController& controller) {

        // Connect to camera
        if (!controller.connect()) {
            std::cout << "❌ Failed to connect to camera" << std::endl;
            return -1;
        }

        auto autoAdjust = controller.getAutoAdjustEnabled();

        // Get initial camera settings
        controller.getCurrentCameraSettings();

        auto cameraState = controller.getCameraState();
        
        std::cout << "\n🎬 Starting exposure monitoring with auto-control..." << std::endl;
        std::cout << "📊 Target brightness: " << cameraState.target_brightness << "/255" << std::endl;
        std::cout << "⏱️  Analysis interval: 15 seconds" << std::endl;
        std::cout << "🤖 Auto-adjust: " << (autoAdjust ? "ENABLED" : "DISABLED") << std::endl;
        std::cout << "🎚️ Confidence threshold: " << (controller.getConfidenceThreshold() * 100) << "%" << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        // Try to capture ONE frame
        std::vector<uint8_t> rgb_data;
        int width, height;
        
        if (controller.captureOneFrame(rgb_data, width, height)) {

            std::cout << "\n🎉 SUCCESS!" << std::endl;
            std::cout << "📊 Frame captured: " << width << "x" << height << std::endl;
            std::cout << "📊 RGB data size: " << rgb_data.size() << " bytes" << std::endl;

            // Analyze exposure
                ExposureMetrics metrics = controller.analyzeExposure(rgb_data, width, height);
                
                cout << "📊 Brightness: " << fixed << setprecision(1) 
                         << metrics.mean_brightness << "/255";
                
                if (metrics.mean_brightness < 100) {
                    std::cout << " (DARK 🌙)";
                } else if (metrics.mean_brightness > 180) {
                    std::cout << " (BRIGHT ☀️)";
                } else {
                    std::cout << " (GOOD ✅)";
                }
                cout << std::endl;
                
                std::cout << "📊 Contrast: " << metrics.contrast << std::endl;
                std::cout << "📊 Highlights clipped: " << metrics.clipped_highlights << "%" << std::endl;
                std::cout << "📊 Shadows clipped: " << metrics.clipped_shadows << "%" << std::endl;
                std::cout << "📊 Exposure score: " << metrics.exposure_score << "/100" << std::endl;
                
                // Get camera adjustment suggestions

                ZCAMSettings suggested = controller.recommendSettings(metrics);

                cout << "💡 Analysis: " << suggested.reasoning << std::endl;
                cout << "   ISO: " << controller.getCurrentISO() << " → " << suggested.iso;
                cout << "💡 Suggested ISO: " << suggested.iso << std::endl;
                
                if (suggested.iso != controller.getCurrentISO() || 
                    abs(suggested.exposure_compensation - controller.getCurrentEV()) > 0.1) {
                    std::cout << "🔧 Suggested ZCAM adjustments:" << std::endl;
                    std::cout << "   ISO: " << controller.getCurrentISO() << " → " << suggested.iso;
                    if (suggested.iso == 500 || suggested.iso == 2500) {
                        std::cout << " (native)";
                    }
                    std::cout << std::endl;
                    std::cout << "   EV: " << controller.getCurrentEV() << " → " << suggested.exposure_compensation << std::endl;
                    std::cout << "   Aperture: f/" << controller.getCurrentAperture() << " → f/" << suggested.aperture << std::endl;
                }
            
        } else {
            std::cout << "\n❌ FAILED to capture frame" << std::endl;
            std::cout << "🔧 Check camera streaming and network connection" << std::endl;
            return false;
        }
        
        std::cout << "\n✅ Test completed successfully!" << endl;

        return true;

}
*/

// Simple test of just the frame capture
int main(int argc, char* argv[]) {

	string site = argc > 1 ? argv[1] : "tlv1"; 

	config = someLogger::loadConfig(string("config/") + site + string(".json"));
	root = config["files"].get<string>();
    someLogger::getInstance(root + "logs/zcam.log")->log("start zcam controller");

    json cameras = config["cameras"];

	left = new ZCamController(config, 0);
	right = new ZCamController(config, 1);

    thread leftThread([left]() {
        left->run();
    });

    thread rightThread([right]() {
        right->run();
    });

    auto service = new someService(config);
    someLogger::getInstance()->log("start service");
    service->run();
        
    if (leftThread.joinable()) {
        leftThread.join();
    }
        
    if (rightThread.joinable()) {
        rightThread.join();
    }

}

