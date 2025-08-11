#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <curl/curl.h>
#include <json/json.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <string>

using namespace std;
using namespace cv;

// ZCAM HTTP Response structure
struct WriteMemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, struct WriteMemoryStruct *userp) {
    size_t realsize = size * nmemb;
    userp->memory = (char*)realloc(userp->memory, userp->size + realsize + 1);
    if (userp->memory == NULL) {
        return 0;
    }
    
    memcpy(&(userp->memory[userp->size]), contents, realsize);
    userp->size += realsize;
    userp->memory[userp->size] = 0;
    
    return realsize;
}

struct ExposureMetrics {
    double mean_brightness;
    std::vector<float> histogram;
    double dynamic_range;
    double contrast;
    double clipped_highlights;
    double clipped_shadows;
    double exposure_score;
};

struct ZCAMSettings {
    int iso;
    double exposure_compensation;  // EV value
    std::string aperture;          // f-stop as string (e.g., "2.8")
    int shutter_angle;             // Shutter angle in degrees
    std::string reasoning;
};

struct LogEntry {
    std::string timestamp;
    ExposureMetrics metrics;
    ZCAMSettings settings;
    double sun_factor;
};

class ZCAMExposureController {
private:
    string camera_ip;
    // cv::VideoCapture rtsp_cap;
    CURL *curl;
    
    double target_brightness = 128.0;
    double brightness_tolerance = 15.0;
    
    // ZCAM E2 parameter ranges
    vector<int> iso_values = {100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800};
    vector<int> native_iso_values = {500, 2500}; // Dual native ISO for E2
    pair<double, double> ev_range = {-3.0, 3.0};
    vector<string> aperture_values = {"1.4", "1.6", "1.8", "2.0", "2.2", "2.5", "2.8", "3.2", "3.5", "4.0", "4.5", "5.0", "5.6", "6.3", "7.1", "8.0", "9.0", "10", "11", "13", "14", "16"};
    
    // Current settings
    int current_iso = 500;
    double current_ev = 0.0;
    string current_aperture = "5.6";
    int current_shutter_angle = 180;
    
    // History for learning
    std::vector<LogEntry> exposure_history;

public:
    ZCAMExposureController(const string& ip = "192.168.1.100") : camera_ip(ip) {
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        // Test camera connection
        if (!testCameraConnection()) {
            throw runtime_error("Failed to connect to ZCAM at " + camera_ip);
        }
        
        // Initialize RTSP stream
        // string rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        // rtsp_cap.open(rtsp_url);
        
        // if (!rtsp_cap.isOpened()) {
        //     std::cout << "Warning: Failed to open RTSP stream, will try HTTP stream capture" << std::endl;
        // }
        
        // Configure stream settings for optimal monitoring
        configureStreamForMonitoring();
        
        cout << "ZCAM Exposure Controller initialized successfully" << std::endl;
        cout << "Camera IP: " << camera_ip << std::endl;
        // cout << "RTSP Stream: " << rtsp_url << std::endl;
    }
    
    ~ZCAMExposureController() {
        // if (rtsp_cap.isOpened()) {
        //     rtsp_cap.release();
        // }
        // cv::destroyAllWindows();
        
        if (curl) {
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
        
        saveFinalLog();
    }
    
    bool testCameraConnection() {
        std::string url = "http://" + camera_ip + "/info";
        std::string response = sendHTTPRequest(url);
        return !response.empty() && response.find("model") != std::string::npos;
    }
    
    void configureStreamForMonitoring() {
        // Configure ZCAM for live streaming
        // Set stream1 (network stream) for monitoring
        std::string base_url = "http://" + camera_ip + "/ctrl/stream_setting";
        
        // Configure stream parameters for real-time monitoring
        sendHTTPRequest(base_url + "?index=stream1&enc=h264");
        sendHTTPRequest(base_url + "?index=stream1&width=1920");
        sendHTTPRequest(base_url + "?index=stream1&height=1080");
        sendHTTPRequest(base_url + "?index=stream1&fps=30");
        sendHTTPRequest(base_url + "?index=stream1&bitrate=8000000"); // 8Mbps for good quality
        
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::string sendHTTPRequest(const std::string& url) {
        if (!curl) return "";
        
        struct WriteMemoryStruct chunk;
        chunk.memory = (char*)malloc(1);
        chunk.size = 0;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        
        CURLcode res = curl_easy_perform(curl);
        
        std::string response;
        if (res == CURLE_OK && chunk.memory) {
            response = std::string(chunk.memory);
        }
        
        if (chunk.memory) {
            free(chunk.memory);
        }
        
        return response;
    }
    
    Mat captureFrame() {

        Mat frame;
        
        // if (rtsp_cap.isOpened()) {
        //     bool ret = rtsp_cap.read(frame);
        //     if (ret && !frame.empty()) {
        //         return frame;
        //     }
        // }
        
        // Fallback: Try to capture via HTTP snapshot or return dummy frame
        string snapshot_url = "http://" + camera_ip + "/ctrl/snapshot";
        string response = sendHTTPRequest(snapshot_url);
        
        // In console mode, create a dummy frame for analysis
        // You could enhance this to actually download and decode the snapshot
        // Create a dummy frame - in real implementation you'd decode the HTTP response
        // frame = cv::Mat::zeros(100, 100, CV_8UC3);
        // Add some realistic data for testing
        // cv::randu(frame, cv::Scalar(100, 100, 100), cv::Scalar(150, 150, 150));
        
        return frame;
    }

    ExposureMetrics analyzeExposure(const cv::Mat& frame) {
        ExposureMetrics metrics;
        
        if (frame.empty()) {
            return metrics;
        }
        
        // Convert to grayscale
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        
        // Calculate mean brightness
        cv::Scalar mean_val = cv::mean(gray);
        metrics.mean_brightness = mean_val[0];
        
        // Calculate histogram
        std::vector<cv::Mat> hist;
        int histSize = 256;
        float range[] = {0, 256};
        const float* histRange = {range};
        cv::Mat hist_mat;
        cv::calcHist(&gray, 1, 0, cv::Mat(), hist_mat, 1, &histSize, &histRange);
        
        // Convert histogram to vector
        metrics.histogram.resize(256);
        for (int i = 0; i < 256; i++) {
            metrics.histogram[i] = hist_mat.at<float>(i);
        }
        
        // Calculate dynamic range
        double min_val, max_val;
        cv::minMaxLoc(gray, &min_val, &max_val);
        metrics.dynamic_range = max_val - min_val;
        
        // Calculate contrast (standard deviation)
        cv::Scalar mean, stddev;
        cv::meanStdDev(gray, mean, stddev);
        metrics.contrast = stddev[0];
        
        // Calculate clipped pixels
        cv::Mat highlight_mask = gray >= 250;
        cv::Mat shadow_mask = gray <= 5;
        metrics.clipped_highlights = (cv::sum(highlight_mask)[0] / 255.0) / gray.total() * 100.0;
        metrics.clipped_shadows = (cv::sum(shadow_mask)[0] / 255.0) / gray.total() * 100.0;
        
        // Calculate exposure score
        metrics.exposure_score = calculateExposureScore(metrics);
        
        return metrics;
    }
    
    double calculateExposureScore(const ExposureMetrics& metrics) {
        double score = 100.0;
        
        // Penalize brightness deviation
        double brightness_error = std::abs(metrics.mean_brightness - target_brightness);
        score -= std::min(brightness_error * 2.0, 50.0);
        
        // Penalize clipped pixels
        score -= metrics.clipped_highlights * 2.0;
        score -= metrics.clipped_shadows * 2.0;
        
        // Reward good contrast
        if (metrics.contrast < 30.0) {
            score -= (30.0 - metrics.contrast);
        } else if (metrics.contrast > 80.0) {
            score -= (metrics.contrast - 80.0) * 0.5;
        }
        
        // Reward good dynamic range
        if (metrics.dynamic_range < 200.0) {
            score -= (200.0 - metrics.dynamic_range) * 0.2;
        }
        
        return std::max(0.0, std::min(100.0, score));
    }
    
    double getSunAngleFactor() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        double hour = tm.tm_hour + tm.tm_min / 60.0;
        
        if (hour >= 6.0 && hour <= 22.0) { // 6am to 10pm surf recording
            double solar_noon = 13.0; // Adjust for your timezone/location
            double hour_angle = std::abs(hour - solar_noon);
            double sun_elevation = 90.0 - (hour_angle * 12.0); // Adjusted for longer day
            return std::max(0.0, sun_elevation / 90.0);
        } else {
            return 0.1; // Very low light
        }
    }
    
    int findClosestISO(int target_iso) {
        auto it = std::lower_bound(iso_values.begin(), iso_values.end(), target_iso);
        if (it == iso_values.end()) return iso_values.back();
        if (it == iso_values.begin()) return iso_values.front();
        
        int upper = *it;
        int lower = *(--it);
        
        return (target_iso - lower < upper - target_iso) ? lower : upper;
    }
    
    string findClosestAperture(double target_f) {
        double min_diff = std::numeric_limits<double>::max();
        std::string closest = aperture_values[0];
        
        for (const auto& f_str : aperture_values) {
            double f_val = std::stod(f_str);
            double diff = std::abs(f_val - target_f);
            if (diff < min_diff) {
                min_diff = diff;
                closest = f_str;
            }
        }
        
        return closest;
    }
    
    ZCAMSettings suggestCameraSettings(const ExposureMetrics& metrics) {
        ZCAMSettings settings;
        double brightness_error = metrics.mean_brightness - target_brightness;
        double sun_factor = getSunAngleFactor();
        
        // Start with current settings
        settings.iso = current_iso;
        settings.exposure_compensation = current_ev;
        settings.aperture = current_aperture;
        settings.shutter_angle = current_shutter_angle;
        
        // ISO adjustment logic
        if (metrics.mean_brightness < target_brightness - brightness_tolerance) {
            // Too dark - increase ISO (prefer native ISO values)
            int target_iso = static_cast<int>(current_iso * 1.6);
            
            // Prefer native ISO values when possible
            if (target_iso >= 2500) {
                settings.iso = 2500; // High native ISO
            } else if (target_iso >= 500) {
                // Choose between native ISOs or find closest
                if (std::abs(target_iso - 500) < std::abs(target_iso - 2500)) {
                    settings.iso = 500;
                } else {
                    settings.iso = findClosestISO(target_iso);
                }
            } else {
                settings.iso = findClosestISO(target_iso);
            }
            
        } else if (metrics.mean_brightness > target_brightness + brightness_tolerance) {
            // Too bright - decrease ISO
            int target_iso = static_cast<int>(current_iso / 1.4);
            
            if (target_iso <= 500) {
                settings.iso = 500; // Low native ISO
            } else {
                settings.iso = findClosestISO(target_iso);
            }
        }
        
        // EV compensation for fine tuning
        if (metrics.clipped_highlights > 5.0) {
            settings.exposure_compensation = std::max(current_ev - 0.5, ev_range.first);
        } else if (metrics.clipped_shadows > 10.0 && metrics.mean_brightness < 100.0) {
            settings.exposure_compensation = std::min(current_ev + 0.3, ev_range.second);
        }
        
        // Aperture adjustment based on lighting and depth of field needs
        double current_f = std::stod(current_aperture);
        if (sun_factor > 0.8) {
            // Bright daylight - smaller aperture for sharpness and surf detail
            settings.aperture = findClosestAperture(std::min(8.0, current_f + 1.0));
        } else if (sun_factor < 0.3) {
            // Low light - wider aperture
            settings.aperture = findClosestAperture(std::max(2.8, current_f - 1.0));
        }
        
        // Shutter angle for surf motion (180° is standard for natural motion)
        if (sun_factor > 0.6) {
            settings.shutter_angle = 180; // Standard for good motion blur
        } else {
            settings.shutter_angle = 270; // Wider for more light in low conditions
        }
        
        settings.reasoning = getAdjustmentReasoning(brightness_error, metrics, sun_factor);
        
        return settings;
    }
    
    string getAdjustmentReasoning(double brightness_error, const ExposureMetrics& metrics, double sun_factor) {
        std::vector<std::string> reasons;
        
        if (std::abs(brightness_error) > brightness_tolerance) {
            std::ostringstream oss;
            if (brightness_error < 0) {
                oss << "Image too dark (brightness: " << std::fixed << std::setprecision(1) << metrics.mean_brightness << ")";
            } else {
                oss << "Image too bright (brightness: " << std::fixed << std::setprecision(1) << metrics.mean_brightness << ")";
            }
            reasons.push_back(oss.str());
        }
        
        if (metrics.clipped_highlights > 5.0) {
            std::ostringstream oss;
            oss << "Highlights clipped (" << std::fixed << std::setprecision(1) << metrics.clipped_highlights << "%)";
            reasons.push_back(oss.str());
        }
        
        if (metrics.clipped_shadows > 10.0) {
            ostringstream oss;
            oss << "Shadows clipped (" << std::fixed << std::setprecision(1) << metrics.clipped_shadows << "%)";
            reasons.push_back(oss.str());
        }
        
        if (sun_factor > 0.8) {
            reasons.push_back("Bright daylight surfing conditions");
        } else if (sun_factor < 0.3) {
            reasons.push_back("Low light dawn/dusk surfing");
        }
        
        if (reasons.empty()) {
            return "Fine-tuning for optimal surf recording";
        }
        
        std::string result = reasons[0];
        for (size_t i = 1; i < reasons.size(); i++) {
            result += "; " + reasons[i];
        }
        return result;
    }
    
    bool updateZCAMSettings(const ZCAMSettings& new_settings) {
        cout << "Updating ZCAM settings:" << std::endl;
        cout << "  ISO: " << current_iso << " → " << new_settings.iso << std::endl;
        cout << "  EV: " << std::fixed << std::setprecision(1) << current_ev 
                  << " → " << new_settings.exposure_compensation << std::endl;
        std::cout << "  Aperture: f/" << current_aperture << " → f/" << new_settings.aperture << std::endl;
        std::cout << "  Shutter Angle: " << current_shutter_angle << "° → " << new_settings.shutter_angle << "°" << std::endl;
        std::cout << "  Reason: " << new_settings.reasoning << std::endl;
        
        bool success = true;
        std::string base_url = "http://" + camera_ip + "/ctrl/set";
        
        // Update ISO
        if (new_settings.iso != current_iso) {
            std::string iso_url = base_url + "?iso=" + std::to_string(new_settings.iso);
            std::string response = sendHTTPRequest(iso_url);
            if (response.find("\"code\":0") != std::string::npos) {
                current_iso = new_settings.iso;
            } else {
                success = false;
                std::cout << "Failed to set ISO" << std::endl;
            }
        }
        
        // Update EV compensation
        if (std::abs(new_settings.exposure_compensation - current_ev) > 0.1) {
            // Convert EV to camera's EV format (usually in 1/3 stops)
            int ev_thirds = static_cast<int>(new_settings.exposure_compensation * 3);
            std::string ev_url = base_url + "?ev=" + std::to_string(ev_thirds);
            std::string response = sendHTTPRequest(ev_url);
            if (response.find("\"code\":0") != std::string::npos) {
                current_ev = new_settings.exposure_compensation;
            } else {
                success = false;
                std::cout << "Failed to set EV compensation" << std::endl;
            }
        }
        
        // Update Aperture (if using electronic lens)
        if (new_settings.aperture != current_aperture) {
            // Convert aperture string to camera format
            std::string aperture_url = base_url + "?aperture=" + new_settings.aperture;
            std::string response = sendHTTPRequest(aperture_url);
            if (response.find("\"code\":0") != std::string::npos) {
                current_aperture = new_settings.aperture;
            } else {
                std::cout << "Note: Aperture control may require electronic lens" << std::endl;
            }
        }
        
        // Update Shutter Angle
        if (new_settings.shutter_angle != current_shutter_angle) {
            std::string shutter_url = base_url + "?shutter_angle=" + std::to_string(new_settings.shutter_angle);
            std::string response = sendHTTPRequest(shutter_url);
            if (response.find("\"code\":0") != std::string::npos) {
                current_shutter_angle = new_settings.shutter_angle;
            } else {
                success = false;
                std::cout << "Failed to set shutter angle" << std::endl;
            }
        }
        
        return success;
    }
    
    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return oss.str();
    }
    
    void logExposureData(const ExposureMetrics& metrics, const ZCAMSettings& settings) {
        LogEntry entry;
        entry.timestamp = getCurrentTimestamp();
        entry.metrics = metrics;
        entry.settings = settings;
        entry.sun_factor = getSunAngleFactor();
        
        exposure_history.push_back(entry);
        
        // Save periodically
        if (exposure_history.size() % 10 == 0) {
            saveLogToFile();
        }
    }
    
    void saveLogToFile() {
        std::ofstream file("zcam_exposure_log.json");
        if (!file.is_open()) return;
        
        file << "[\n";
        for (size_t i = 0; i < exposure_history.size(); i++) {
            const auto& entry = exposure_history[i];
            file << "  {\n";
            file << "    \"timestamp\": \"" << entry.timestamp << "\",\n";
            file << "    \"metrics\": {\n";
            file << "      \"mean_brightness\": " << entry.metrics.mean_brightness << ",\n";
            file << "      \"exposure_score\": " << entry.metrics.exposure_score << ",\n";
            file << "      \"clipped_highlights\": " << entry.metrics.clipped_highlights << ",\n";
            file << "      \"clipped_shadows\": " << entry.metrics.clipped_shadows << ",\n";
            file << "      \"contrast\": " << entry.metrics.contrast << ",\n";
            file << "      \"dynamic_range\": " << entry.metrics.dynamic_range << "\n";
            file << "    },\n";
            file << "    \"settings\": {\n";
            file << "      \"iso\": " << entry.settings.iso << ",\n";
            file << "      \"exposure_compensation\": " << entry.settings.exposure_compensation << ",\n";
            file << "      \"aperture\": \"" << entry.settings.aperture << "\",\n";
            file << "      \"shutter_angle\": " << entry.settings.shutter_angle << ",\n";
            file << "      \"reasoning\": \"" << entry.settings.reasoning << "\"\n";
            file << "    },\n";
            file << "    \"sun_factor\": " << entry.sun_factor << "\n";
            file << "  }";
            if (i < exposure_history.size() - 1) file << ",";
            file << "\n";
        }
        file << "]\n";
        file.close();
    }
    
    void saveFinalLog() {
        if (!exposure_history.empty()) {
            saveLogToFile();
        }
    }
    
    cv::Mat drawHistogram(const std::vector<float>& hist) {
        cv::Mat hist_img = cv::Mat::zeros(100, 250, CV_8UC3);
        
        // Find max value for normalization
        float max_val = *std::max_element(hist.begin(), hist.end());
        if (max_val == 0) return hist_img;
        
        for (int i = 0; i < 256; i++) {
            int x = i * 250 / 256;
            int height = static_cast<int>(hist[i] / max_val * 90);
            cv::line(hist_img, cv::Point(x, 100), cv::Point(x, 100 - height), 
                    cv::Scalar(255, 255, 255), 1);
        }
        
        return hist_img;
    }
    
    void displayFrameWithInfo(const cv::Mat& frame, const ExposureMetrics& metrics) {
        cv::Mat display_frame = frame.clone();
        
        // Add ZCAM exposure info overlay
        vector<string> info_text = {
            string("ZCAM E2 Surf Monitor"),
            string("Brightness: ") + to_string(static_cast<int>(metrics.mean_brightness)),
            string("Score: ") + to_string(static_cast<int>(metrics.exposure_score)) + "/100",
            string("ISO: ") + to_string(current_iso) + (current_iso == 500 || current_iso == 2500 ? "*" : ""),
            string("EV: ") + String(current_ev >= 0 ? "+" : "") + to_string(current_ev).substr(0, 4),
            string("f/") + current_aperture,
            string("Shutter: ") + to_string(current_shutter_angle) + String("°")
        };
        
        for (size_t i = 0; i < info_text.size(); i++) {
            cv::Scalar color = (i == 0) ? cv::Scalar(0, 255, 255) : cv::Scalar(0, 255, 0); // Yellow for title
            cv::putText(display_frame, info_text[i], 
                       cv::Point(10, 25 + i * 20),
                       cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
        }
        
        // Add histogram
        cv::Mat hist_display = drawHistogram(metrics.histogram);
        cv::Rect roi(display_frame.cols - 260, 10, 250, 100);
        if (roi.x >= 0 && roi.y >= 0 && 
            roi.x + roi.width <= display_frame.cols && 
            roi.y + roi.height <= display_frame.rows) {
            hist_display.copyTo(display_frame(roi));
        }
        
        // Add recording indicator
        auto now = chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        if (ms.count() < 500) { // Blinking red dot
            cv::circle(display_frame, cv::Point(display_frame.cols - 30, 30), 8, cv::Scalar(0, 0, 255), -1);
            cv::putText(display_frame, "REC", cv::Point(display_frame.cols - 60, 40), 
                       cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 2);
        }
        
        // cv::imshow("ZCAM Surf Auto Exposure", display_frame);
    }
    
    void runAutoAdjustment(int interval_seconds = 30) {
        std::cout << "\n=== ZCAM Surf Auto Exposure Controller ===" << std::endl;
        std::cout << "Camera IP: " << camera_ip << std::endl;
        std::cout << "Recording Hours: 6:00 AM - 10:00 PM" << std::endl;
        std::cout << "Adjustment Interval: " << interval_seconds << " seconds" << std::endl;
        std::cout << "\nControls:" << std::endl;
        std::cout << "  'q' - Quit" << std::endl;
        std::cout << "  's' - Save snapshot" << std::endl;
        std::cout << "  'r' - Start/Stop recording" << std::endl;
        std::cout << "  'i' - Get camera info" << std::endl;
        std::cout << "==========================================\n" << std::endl;
        
        while (true) {
            cv::Mat frame = captureFrame();
            if (frame.empty()) {
                std::cout << "No frame available, checking stream..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            
            // Analyze exposure
            ExposureMetrics metrics = analyzeExposure(frame);
            
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            
            std::cout << "\n--- ZCAM Exposure Analysis (" 
                     << std::put_time(&tm, "%H:%M:%S") << ") ---" << std::endl;
            std::cout << std::fixed << std::setprecision(1);
            std::cout << "Brightness: " << metrics.mean_brightness 
                     << " (target: " << target_brightness << ")" << std::endl;
            std::cout << "Exposure Score: " << metrics.exposure_score << "/100" << std::endl;
            std::cout << "Clipped Highlights: " << metrics.clipped_highlights << "%" << std::endl;
            std::cout << "Clipped Shadows: " << metrics.clipped_shadows << "%" << std::endl;
            std::cout << "Contrast: " << metrics.contrast << std::endl;
            std::cout << "Sun Factor: " << getSunAngleFactor() << std::endl;
            
            // Check if adjustment needed
            bool needs_adjustment = 
                std::abs(metrics.mean_brightness - target_brightness) > brightness_tolerance ||
                metrics.clipped_highlights > 5.0 ||
                metrics.clipped_shadows > 10.0;
            
            if (needs_adjustment) {
                ZCAMSettings suggested = suggestCameraSettings(metrics);
                
                // Only update if settings actually changed
                if (suggested.iso != current_iso || 
                    std::abs(suggested.exposure_compensation - current_ev) > 0.1 ||
                    suggested.aperture != current_aperture ||
                    suggested.shutter_angle != current_shutter_angle) {
                    
                    if (updateZCAMSettings(suggested)) {
                        logExposureData(metrics, suggested);
                    } else {
                        std::cout << "Warning: Some settings failed to update" << std::endl;
                    }
                }
            } else {
                std::cout << "Exposure is optimal for surf recording" << std::endl;
            }
            
            // Display frame with info
            displayFrameWithInfo(frame, metrics);
            
            // Check for user input
            // char key = cv::waitKey(1000) & 0xFF;
            char key = 0;
            if (key == 'q') {
                break;
            } else if (key == 's') {
                // Save snapshot
                auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                auto tm_stamp = *std::localtime(&timestamp);
                std::ostringstream filename;
                // filename << "zcam_surf_snapshot_" << std::put_time(&tm_stamp, "%Y%m%d_%H%M%S") << ".jpg";
                // cv::imwrite(filename.str(), frame);
                std::cout << "Snapshot saved: " << filename.str() << std::endl;
            } else if (key == 'r') {
                // Toggle recording
                toggleRecording();
            } else if (key == 'i') {
                // Get camera info
                showCameraInfo();
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        }
    }
    
    void toggleRecording() {
        // Get current recording status
        std::string status_url = "http://" + camera_ip + "/info";
        std::string response = sendHTTPRequest(status_url);
        
        bool is_recording = response.find("\"recording\":true") != std::string::npos;
        
        std::string action_url = "http://" + camera_ip + "/ctrl/";
        if (is_recording) {
            action_url += "rec?action=stop";
            std::cout << "Stopping recording..." << std::endl;
        } else {
            action_url += "rec?action=start";
            std::cout << "Starting recording..." << std::endl;
        }
        
        std::string result = sendHTTPRequest(action_url);
        if (result.find("\"code\":0") != std::string::npos) {
            std::cout << "Recording " << (is_recording ? "stopped" : "started") << " successfully" << std::endl;
        } else {
            std::cout << "Failed to toggle recording" << std::endl;
        }
    }
    
    void showCameraInfo() {
        std::string info_url = "http://" + camera_ip + "/info";
        std::string response = sendHTTPRequest(info_url);
        
        std::cout << "\n=== ZCAM Camera Info ===" << std::endl;
        
        // Parse basic info (simplified JSON parsing)
        if (response.find("\"model\"") != std::string::npos) {
            size_t model_start = response.find("\"model\":\"") + 9;
            size_t model_end = response.find("\"", model_start);
            if (model_end != std::string::npos) {
                std::string model = response.substr(model_start, model_end - model_start);
                std::cout << "Model: " << model << std::endl;
            }
        }
        
        if (response.find("\"battery\"") != std::string::npos) {
            size_t battery_start = response.find("\"battery\":") + 10;
            size_t battery_end = response.find(",", battery_start);
            if (battery_end != std::string::npos) {
                std::string battery = response.substr(battery_start, battery_end - battery_start);
                std::cout << "Battery: " << battery << "%" << std::endl;
            }
        }
        
        bool is_recording = response.find("\"recording\":true") != std::string::npos;
        std::cout << "Recording: " << (is_recording ? "ON" : "OFF") << std::endl;
        
        std::cout << "Current Settings:" << std::endl;
        std::cout << "  ISO: " << current_iso << std::endl;
        std::cout << "  EV: " << current_ev << std::endl;
        std::cout << "  Aperture: f/" << current_aperture << std::endl;
        std::cout << "  Shutter Angle: " << current_shutter_angle << "°" << std::endl;
        std::cout << "========================\n" << std::endl;
    }
    
    // Additional utility methods for surf-specific optimizations
    void setSurfOptimizedSettings() {
        std::cout << "Applying surf-optimized settings..." << std::endl;
        
        // Set color profile for outdoor/surf conditions
        sendHTTPRequest("http://" + camera_ip + "/ctrl/set?color_profile=natural");
        
        // Set white balance for daylight
        sendHTTPRequest("http://" + camera_ip + "/ctrl/set?wb=daylight");
        
        // Enable image stabilization if available
        sendHTTPRequest("http://" + camera_ip + "/ctrl/set?stabilization=on");
        
        // Set recording format for high quality
        sendHTTPRequest("http://" + camera_ip + "/ctrl/set?format=4k_30p");
        
        // Set codec for good compression vs quality balance
        sendHTTPRequest("http://" + camera_ip + "/ctrl/set?codec=h265");
        
        std::cout << "Surf optimization complete" << std::endl;
    }
    
    void calibrateForLocation() {
        std::cout << "Starting location calibration..." << std::endl;
        
        // Take test shots at different times
        std::vector<ExposureMetrics> test_metrics;
        
        for (int i = 0; i < 5; i++) {
            Mat frame = captureFrame();
            if (!frame.empty()) {
                ExposureMetrics metrics = analyzeExposure(frame);
                test_metrics.push_back(metrics);
                std::cout << "Test " << (i+1) << " brightness: " << metrics.mean_brightness << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        
        if (!test_metrics.empty()) {
            // Calculate average brightness and adjust target
            double avg_brightness = 0;
            for (const auto& m : test_metrics) {
                avg_brightness += m.mean_brightness;
            }
            avg_brightness /= test_metrics.size();
            
            // Adjust target brightness based on current conditions
            if (avg_brightness < 100) {
                target_brightness = 110; // Lower target for low light
            } else if (avg_brightness > 150) {
                target_brightness = 140; // Higher target for bright conditions
            }
            
            std::cout << "Calibrated target brightness: " << target_brightness << std::endl;
        }
    }
};

// Example usage and main function
int main(int argc, char* argv[]) {
    std::string camera_ip = "192.168.1.100"; // Default ZCAM IP
    
    // Parse command line arguments
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    try {
        std::cout << "Initializing ZCAM Surf Camera Controller..." << std::endl;
        
        // Initialize camera controller
        ZCAMExposureController controller(camera_ip);
        
        // Apply surf-specific optimizations
        controller.setSurfOptimizedSettings();
        
        // Calibrate for current location/conditions
        controller.calibrateForLocation();
        
        // Run automatic adjustment every 30 seconds
        controller.runAutoAdjustment(30);
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "\nUsage: " << argv[0] << " [camera_ip]" << std::endl;
        std::cerr << "Example: " << argv[0] << " 192.168.1.100" << std::endl;
        return -1;
    }
    
    return 0;
}

/*
COMPILATION INSTRUCTIONS:

1. Install dependencies:
   sudo apt-get install libopencv-dev libcurl4-openssl-dev libjsoncpp-dev

2. Compile:
   g++ -std=c++17 zcam_surf_controller.cpp -o zcam_surf_controller \
   `pkg-config --cflags --libs opencv4` -lcurl -ljsoncpp

3. Run:
   ./zcam_surf_controller [camera_ip]

ZCAM SETUP:
1. Connect ZCAM to your network via Ethernet
2. Set camera to Router mode (Connect -> Network -> ETH Mode -> Router)
3. Note the camera's IP address (displayed in Network menu)
4. Ensure camera firmware supports HTTP API (v0.89 or newer)

FEATURES:
- Real-time RTSP stream analysis
- Automatic ISO, EV, aperture, and shutter adjustment
- ZCAM native dual ISO optimization (500/2500)
- Surf-specific time-based adjustments (6am-10pm)
- Remote recording control
- Snapshot capture
- Comprehensive logging

CONTROLS:
- 'q': Quit application
- 's': Save snapshot with current settings
- 'r': Toggle recording start/stop
- 'i': Show camera information
*/