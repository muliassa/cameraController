#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <fstream>
#include <curl/curl.h>
#include <json/json.h>

// FFmpeg C API headers  
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

// Global control
std::atomic<bool> keep_running{true};

void signal_handler(int signal) {
    std::cout << "\n🛑 Shutting down auto-exposure controller..." << std::endl;
    keep_running = false;
}

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
    std::string iris = "10";          // f-stop
    double target_brightness = 140.0; // Slightly brighter for surf
    double brightness_tolerance = 20.0;
    
    // Iris constraints for surf
    std::string min_iris = "8";       // f/8 - good contrast, not too wide
    std::string max_iris = "16";      // f/16 - reasonable light control
};

// HTTP response handler
struct HTTPResponse {
    std::string data;
    long response_code;
    bool success;
    HTTPResponse() : response_code(0), success(false) {}
};

static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t totalSize = size * nmemb;
    HTTPResponse *response = static_cast<HTTPResponse*>(userp);
    if (response && contents) {
        response->data.append(static_cast<char*>(contents), totalSize);
    }
    return totalSize;
}

class ZCAMProductionController {
private:
    std::string camera_ip;
    std::string rtsp_url;
    std::string http_base_url;
    CURL *curl;
    
    // FFmpeg components
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;
    SwsContext *sws_ctx = nullptr;
    int video_stream_index = -1;
    
    CameraSettings settings;
    std::ofstream log_file;
    int adjustment_count = 0;

public:
    ZCAMProductionController(const std::string& ip) : camera_ip(ip) {
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        // Initialize FFmpeg
        #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();
        #endif
        avformat_network_init();
        
        // Open log file
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        char filename[100];
        std::strftime(filename, sizeof(filename), "zcam_production_%Y%m%d.log", &tm);
        log_file.open(filename, std::ios::app);
        
        if (log_file.is_open()) {
            log_file << "=== ZCAM Production Controller Started ===" << std::endl;
            log_file << "Time: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << std::endl;
            log_file << "Schedule: " << start_hour << ":00 - " << end_hour << ":00" << std::endl;
            log_file << "Target Brightness: " << settings.target_brightness << std::endl;
            log_file << "Iris Range: f/" << settings.min_iris << " - f/" << settings.max_iris << std::endl;
        }
        
        std::cout << "🎬 ZCAM Production Auto-Exposure Controller" << std::endl;
        std::cout << "📡 Camera: " << camera_ip << std::endl;
        std::cout << "⏰ Schedule: " << start_hour << ":00 - " << end_hour << ":00" << std::endl;
        std::cout << "🎯 Target: " << settings.target_brightness << "/255" << std::endl;
    }
    
    ~ZCAMProductionController() {
        cleanup();
        
        if (curl) {
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
        avformat_network_deinit();
        
        if (log_file.is_open()) {
            log_file << "Controller stopped. Total adjustments: " << adjustment_count << std::endl;
            log_file.close();
        }
    }
    
    bool adjustExposure(const ExposureMetrics& metrics) {
        double brightness_error = metrics.brightness - settings.target_brightness;
        bool needs_adjustment = std::abs(brightness_error) > settings.brightness_tolerance;
        
        if (!needs_adjustment && metrics.exposure_score >= 70.0) {
            return false; // No adjustment needed
        }
        
        std::cout << "🔧 Adjusting exposure..." << std::endl;
        std::cout << "   Current: B=" << metrics.brightness << ", C=" << metrics.contrast 
                 << ", Score=" << metrics.exposure_score << std::endl;
        
        bool changed = false;
        std::string reason;
        
        // AGGRESSIVE ISO STRATEGY - Use full range, minimize iris changes
        if (brightness_error < -settings.brightness_tolerance) {
            // Too dark - use full ISO range before touching iris
            int new_iso = settings.iso;
            
            if (settings.iso < 2500) {
                new_iso = 2500;  // Jump to high native
                reason = "Dark - jump to native ISO 2500";
            } else if (settings.iso < 6400) {
                new_iso = 6400;  // Good quality range
                reason = "Still dark - ISO to 6400";
            } else if (settings.iso < 12800) {
                new_iso = 12800; // Acceptable quality
                reason = "Very dark - ISO to 12800";
            } else if (settings.iso < 25600) {
                new_iso = 25600; // High but usable
                reason = "Extremely dark - ISO to 25600";
            } else if (settings.iris != settings.min_iris) {
                // Only open iris after exhausting ISO options
                if (applySetting("iris", settings.min_iris)) {
                    reason = "Max ISO reached - opened iris f/" + settings.iris + "→f/" + settings.min_iris;
                    settings.iris = settings.min_iris;
                    changed = true;
                }
            }
            
            // Apply ISO change
            if (new_iso != settings.iso) {
                if (applySetting("iso", std::to_string(new_iso))) {
                    settings.iso = new_iso;
                    changed = true;
                }
            }
            
        } else if (brightness_error > settings.brightness_tolerance) {
            // Too bright - PRIORITIZE keeping good iris, reduce ISO aggressively
            
            // Check if we can solve with ISO reduction first
            if (settings.iso > 400) {
                int new_iso = settings.iso;
                
                if (settings.iso > 6400) {
                    new_iso = settings.iso / 2;  // Big steps down from high ISO
                    reason = "Bright - large ISO reduction " + std::to_string(settings.iso) + "→" + std::to_string(new_iso);
                } else if (settings.iso > 2500) {
                    new_iso = 1000;  // Step down to medium
                    reason = "Moderately bright - ISO to 1000";
                } else if (settings.iso > 500) {
                    new_iso = 400;   // Minimum ISO
                    reason = "Bright - minimum ISO 400";
                }
                
                if (new_iso != settings.iso) {
                    if (applySetting("iso", std::to_string(new_iso))) {
                        settings.iso = new_iso;
                        changed = true;
                    }
                }
            } else if (settings.iris != settings.max_iris && std::stod(settings.iris) < std::stod(settings.max_iris)) {
                // Only close iris after reaching minimum ISO
                std::string new_iris;
                double current_iris = std::stod(settings.iris);
                
                if (current_iris < 11) {
                    new_iris = "11";
                } else if (current_iris < 14) {
                    new_iris = "14";
                } else {
                    new_iris = settings.max_iris;
                }
                
                if (applySetting("iris", new_iris)) {
                    reason = "Very bright - closed iris f/" + settings.iris + "→f/" + new_iris + " (min ISO reached)";
                    settings.iris = new_iris;
                    changed = true;
                }
            }
        }
        
        if (changed) {
            adjustment_count++;
            std::cout << "   ✅ " << reason << std::endl;
            
            if (log_file.is_open()) {
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                auto tm = *std::localtime(&time_t);
                
                log_file << "[" << std::put_time(&tm, "%H:%M:%S") << "] ADJUSTMENT #" << adjustment_count 
                         << " | B:" << std::fixed << std::setprecision(1) << metrics.brightness
                         << " C:" << metrics.contrast << " S:" << metrics.exposure_score
                         << " | ISO:" << settings.iso << " f/" << settings.iris 
                         << " | " << reason << std::endl;
            }
            
            // Wait for camera to apply changes
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else {
            std::cout << "   ⚠️ No suitable adjustment available" << std::endl;
        }
        
        return changed;
    }
    
    void runProductionMode() {
        std::cout << "🚀 Starting production mode..." << std::endl;
        
        if (!initializeStream()) {
            std::cout << "❌ Failed to initialize stream" << std::endl;
            return;
        }
        
        if (!readCurrentSettings()) {
            std::cout << "❌ Failed to read camera settings" << std::endl;
            return;
        }
        
        while (keep_running) {
            if (!isOperatingHours()) {
                std::cout << "😴 Outside operating hours, sleeping..." << std::endl;
                std::this_thread::sleep_for(std::chrono::minutes(30));
                continue;
            }
            
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            
            std::cout << "\n📸 [" << std::put_time(&tm, "%H:%M:%S") << "] Analyzing exposure..." << std::endl;
            
            std::vector<uint8_t> rgb_data;
            int width, height;
            
            if (captureFrame(rgb_data, width, height)) {
                ExposureMetrics metrics = analyzeExposure(rgb_data, width, height);
                
                std::cout << "   Brightness: " << std::fixed << std::setprecision(1) 
                         << metrics.brightness << "/255, Contrast: " << metrics.contrast 
                         << ", Score: " << metrics.exposure_score << "/100" << std::endl;
                
                adjustExposure(metrics);
            } else {
                std::cout << "   ⚠️ Frame capture failed" << std::endl;
            }
            
            // Wait 1 minute before next check
            for (int i = 0; i < 60 && keep_running; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        std::cout << "✅ Production mode stopped" << std::endl;
    }
    
    void cleanup() {
        if (sws_ctx) {
            sws_freeContext(sws_ctx);
            sws_ctx = nullptr;
        }
        
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
            codec_ctx = nullptr;
        }
        
        if (format_ctx) {
            avformat_close_input(&format_ctx);
            format_ctx = nullptr;
        }
        
        video_stream_index = -1;
    }
};

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::string camera_ip = "192.168.150.201";
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    try {
        ZCAMProductionController controller(camera_ip);
        controller.runProductionMode();
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}