#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <iomanip>
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
    double exposure_compensation;
    std::string aperture;
    int shutter_angle;
    std::string reasoning;
};

class ZCAMFFmpegController {
private:
    std::string camera_ip;
    std::string rtsp_url;
    CURL *curl;
    
    // FFmpeg components
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;  // Use const AVCodec* for newer FFmpeg versions
    SwsContext *sws_ctx = nullptr;
    int video_stream_index = -1;
    
    // Current camera settings
    int current_iso = 500;
    double current_ev = 0.0;
    std::string current_aperture = "5.6";
    int current_shutter_angle = 180;
    
    double target_brightness = 128.0;
    double brightness_tolerance = 15.0;

public:
    ZCAMFFmpegController(const std::string& ip) : camera_ip(ip) {
        rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        
        // Initialize CURL
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        
        // Initialize FFmpeg (for newer versions)
        #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();  // Only needed for older FFmpeg versions
        #endif
        avformat_network_init();
        
        std::cout << "✅ ZCAM FFmpeg Controller initialized" << std::endl;
        std::cout << "   Camera IP: " << camera_ip << std::endl;
        std::cout << "   RTSP URL: " << rtsp_url << std::endl;
    }
    
    ~ZCAMFFmpegController() {
        cleanup();
        
        if (curl) {
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
        
        avformat_network_deinit();
    }
    
    bool initializeStream() {
        std::cout << "🔌 Connecting to ZCAM RTSP stream..." << std::endl;
        
        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cout << "❌ Failed to allocate format context" << std::endl;
            return false;
        }
        
        // Set RTSP CLIENT options - CRITICAL: TCP transport for ZCAM
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);     // ZCAM requires TCP!
        av_dict_set(&options, "stimeout", "10000000", 0);      // Connection timeout (10 sec)
        av_dict_set(&options, "max_delay", "5000000", 0);      // 5 seconds max delay
        av_dict_set(&options, "buffer_size", "1024000", 0);    // 1MB buffer
        av_dict_set(&options, "rtsp_flags", "prefer_tcp", 0);  // Force TCP
        av_dict_set(&options, "user_agent", "ZCAMController/1.0", 0);
        
        std::cout << "   Using TCP transport (required for ZCAM)" << std::endl;
        std::cout << "   URL: " << rtsp_url << std::endl;
        
        // Open input stream with ZCAM-compatible options
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        
        // Clean up options dictionary
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open RTSP stream: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ RTSP connection established" << std::endl;
        
        // Find stream information
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            std::cout << "❌ Failed to find stream info" << std::endl;
            return false;
        }
        
        // Find video stream
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream_index = i;
                break;
            }
        }
        
        if (video_stream_index == -1) {
            std::cout << "❌ No video stream found" << std::endl;
            return false;
        }
        
        // Get codec parameters
        AVCodecParameters *codec_params = format_ctx->streams[video_stream_index]->codecpar;
        
        // Find decoder
        codec = avcodec_find_decoder(codec_params->codec_id);
        if (!codec) {
            std::cout << "❌ Codec not supported" << std::endl;
            return false;
        }
        
        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cout << "❌ Failed to allocate codec context" << std::endl;
            return false;
        }
        
        // Copy codec parameters
        if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
            std::cout << "❌ Failed to copy codec parameters" << std::endl;
            return false;
        }
        
        // Open codec
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cout << "❌ Failed to open codec" << std::endl;
            return false;
        }
        
        std::cout << "✅ Video decoder initialized" << std::endl;
        std::cout << "   Resolution: " << codec_ctx->width << "x" << codec_ctx->height << std::endl;
        std::cout << "   Codec: " << codec->name << std::endl;
        std::cout << "   Pixel Format: " << av_get_pix_fmt_name(codec_ctx->pix_fmt) << std::endl;
        
        return true;
    }
    
    bool captureFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
        if (!format_ctx || !codec_ctx) {
            std::cout << "❌ Stream not initialized" << std::endl;
            return false;
        }
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgb_frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            std::cout << "❌ Failed to allocate frames" << std::endl;
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&rgb_frame);
            return false;
        }
        
        bool frame_captured = false;
        
        // Read packets until we get a video frame
        while (av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == video_stream_index) {
                // Send packet to decoder
                if (avcodec_send_packet(codec_ctx, packet) == 0) {
                    // Receive frame from decoder
                    if (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        // Convert to RGB
                        width = frame->width;
                        height = frame->height;
                        
                        // Initialize scaler if needed
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                width, height, (AVPixelFormat)frame->format,
                                width, height, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }
                        
                        if (sws_ctx) {
                            // Allocate RGB buffer
                            int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
                            rgb_data.resize(rgb_size);
                            
                            // Setup RGB frame
                            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize,
                                                rgb_data.data(), AV_PIX_FMT_RGB24, width, height, 1);
                            
                            // Convert frame to RGB
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                    rgb_frame->data, rgb_frame->linesize);
                            
                            frame_captured = true;
                        }
                        
                        break;
                    }
                }
            }
            av_packet_unref(packet);
        }
        
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        
        if (frame_captured) {
            std::cout << "📷 Captured frame: " << width << "x" << height << std::endl;
        }
        
        return frame_captured;
    }
    
    ExposureMetrics analyzeExposure(const std::vector<uint8_t>& rgb_data, int width, int height) {
        ExposureMetrics metrics;
        
        if (rgb_data.empty()) {
            return metrics;
        }
        
        // Convert RGB to grayscale and analyze
        std::vector<uint8_t> gray_data;
        gray_data.reserve(width * height);
        
        double sum_brightness = 0.0;
        double sum_squared = 0.0;
        int highlight_count = 0;
        int shadow_count = 0;
        
        // Initialize histogram
        metrics.histogram.resize(256, 0.0f);
        
        // Process each pixel
        for (int i = 0; i < width * height; i++) {
            int pixel_idx = i * 3; // RGB format
            if (pixel_idx + 2 < rgb_data.size()) {
                uint8_t r = rgb_data[pixel_idx];
                uint8_t g = rgb_data[pixel_idx + 1];
                uint8_t b = rgb_data[pixel_idx + 2];
                
                // Convert to grayscale (standard weights)
                uint8_t gray = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
                gray_data.push_back(gray);
                
                // Accumulate statistics
                sum_brightness += gray;
                sum_squared += gray * gray;
                
                // Count clipped pixels
                if (gray >= 250) highlight_count++;
                if (gray <= 5) shadow_count++;
                
                // Build histogram
                metrics.histogram[gray]++;
            }
        }
        
        int total_pixels = width * height;
        if (total_pixels > 0) {
            // Calculate metrics
            metrics.mean_brightness = sum_brightness / total_pixels;
            
            // Calculate standard deviation (contrast)
            double variance = (sum_squared / total_pixels) - (metrics.mean_brightness * metrics.mean_brightness);
            metrics.contrast = std::sqrt(variance);
            
            // Calculate clipped percentages
            metrics.clipped_highlights = (highlight_count * 100.0) / total_pixels;
            metrics.clipped_shadows = (shadow_count * 100.0) / total_pixels;
            
            // Find dynamic range
            auto min_it = std::find_if(gray_data.begin(), gray_data.end(), [](uint8_t val) { return val > 0; });
            auto max_it = std::max_element(gray_data.begin(), gray_data.end());
            if (min_it != gray_data.end() && max_it != gray_data.end()) {
                metrics.dynamic_range = *max_it - *min_it;
            }
            
            // Normalize histogram
            for (auto& val : metrics.histogram) {
                val /= total_pixels;
            }
            
            // Calculate exposure score
            metrics.exposure_score = calculateExposureScore(metrics);
        }
        
        return metrics;
    }
    
    double calculateExposureScore(const ExposureMetrics& metrics) {
        double score = 100.0;
        
        // Penalize brightness deviation from target
        double brightness_error = std::abs(metrics.mean_brightness - target_brightness);
        score -= std::min(brightness_error * 2.0, 50.0);
        
        // Penalize clipped pixels
        score -= metrics.clipped_highlights * 2.0;
        score -= metrics.clipped_shadows * 2.0;
        
        // Reward good contrast (but not too much)
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
        
        if (hour >= 6.0 && hour <= 22.0) {
            double solar_noon = 13.0;
            double hour_angle = std::abs(hour - solar_noon);
            double sun_elevation = 90.0 - (hour_angle * 12.0);
            return std::max(0.0, sun_elevation / 90.0);
        } else {
            return 0.1;
        }
    }
    
    // HTTP request helper
    std::string sendHTTPRequest(const std::string& url) {
        struct WriteMemoryStruct {
            char *memory;
            size_t size;
        };
        
        auto WriteMemoryCallback = [](void *contents, size_t size, size_t nmemb, struct WriteMemoryStruct *userp) -> size_t {
            size_t realsize = size * nmemb;
            userp->memory = (char*)realloc(userp->memory, userp->size + realsize + 1);
            if (userp->memory == NULL) return 0;
            
            memcpy(&(userp->memory[userp->size]), contents, realsize);
            userp->size += realsize;
            userp->memory[userp->size] = 0;
            
            return realsize;
        };
        
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
    
    ZCAMSettings suggestCameraSettings(const ExposureMetrics& metrics) {
        ZCAMSettings settings;
        settings.iso = current_iso;
        settings.exposure_compensation = current_ev;
        settings.aperture = current_aperture;
        settings.shutter_angle = current_shutter_angle;
        
        double brightness_error = metrics.mean_brightness - target_brightness;
        double sun_factor = getSunAngleFactor();
        
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
        
        std::cout << "🧹 FFmpeg resources cleaned up" << std::endl;
    }
    
    ZCAMSettings suggestCameraSettings(const ExposureMetrics& metrics) {
        ZCAMSettings settings;
        settings.iso = current_iso;
        settings.exposure_compensation = current_ev;
        settings.aperture = current_aperture;
        settings.shutter_angle = current_shutter_angle;
        
        double brightness_error = metrics.mean_brightness - target_brightness;
        double sun_factor = getSunAngleFactor();
        
        // ISO adjustment using ZCAM dual native ISOs (500/2500)
        if (metrics.mean_brightness < target_brightness - brightness_tolerance) {
            // Too dark - increase ISO, prefer native ISOs
            if (current_iso == 500) {
                settings.iso = 1000;  // Step up from low native
            } else if (current_iso < 2500) {
                settings.iso = 2500;  // Jump to high native ISO
            }
        } else if (metrics.mean_brightness > target_brightness + brightness_tolerance) {
            // Too bright - decrease ISO, prefer native ISOs  
            if (current_iso == 2500) {
                settings.iso = 1000;  // Step down from high native
            } else if (current_iso > 500) {
                settings.iso = 500;   // Back to low native ISO
            }
        }
        
        // EV compensation for fine-tuning
        if (metrics.clipped_highlights > 5.0) {
            settings.exposure_compensation = std::max(current_ev - 0.7, -2.0);
        } else if (metrics.clipped_shadows > 10.0 && metrics.mean_brightness < 100.0) {
            settings.exposure_compensation = std::min(current_ev + 0.5, 2.0);
        }
        
        // Aperture based on lighting conditions
        double current_f = std::stod(current_aperture);
        if (sun_factor > 0.8) {
            // Bright daylight - smaller aperture for sharpness
            settings.aperture = "8.0";
        } else if (sun_factor < 0.3) {
            // Low light - wider aperture
            settings.aperture = "2.8";
        }
        
        // Shutter angle for surf motion
        if (sun_factor > 0.6) {
            settings.shutter_angle = 180;  // Standard motion blur
        } else {
            settings.shutter_angle = 270;  // More light in low conditions
        }
        
        // Generate reasoning
        std::vector<std::string> reasons;
        if (std::abs(brightness_error) > brightness_tolerance) {
            reasons.push_back(brightness_error < 0 ? "Too dark" : "Too bright");
        }
        if (metrics.clipped_highlights > 5.0) {
            reasons.push_back("Highlights clipped");
        }
        if (metrics.clipped_shadows > 10.0) {
            reasons.push_back("Shadows clipped");
        }
        if (sun_factor > 0.8) {
            reasons.push_back("Bright daylight surfing");
        } else if (sun_factor < 0.3) {
            reasons.push_back("Low light surfing");
        }
        
        settings.reasoning = reasons.empty() ? "Exposure optimal for surf recording" : reasons[0];
        for (size_t i = 1; i < reasons.size(); i++) {
            settings.reasoning += ", " + reasons[i];
        }
        
        return settings;
    }
    
    // Getters for current settings
    int getCurrentISO() const { return current_iso; }
    double getCurrentEV() const { return current_ev; }
    std::string getCurrentAperture() const { return current_aperture; }
    int getCurrentShutterAngle() const { return current_shutter_angle; }
};

int main(int argc, char* argv[]) {
    std::string camera_ip = "192.168.150.201";
    
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    try {
        std::cout << "=== ZCAM E2-F8 FFmpeg Controller ===" << std::endl;
        std::cout << "Using native FFmpeg C API (no OpenCV GDAL conflicts)" << std::endl;
        
        ZCAMFFmpegController controller(camera_ip);
        
        // Initialize RTSP stream
        if (!controller.initializeStream()) {
            std::cout << "❌ Failed to initialize RTSP stream" << std::endl;
            return -1;
        }
        
        std::cout << "\n🎬 Starting live exposure monitoring..." << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        int analysis_count = 0;
        
        while (true) {
            analysis_count++;
            
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            
            std::cout << "\n--- Analysis #" << analysis_count 
                     << " (" << std::put_time(&tm, "%H:%M:%S") << ") ---" << std::endl;
            
            // Capture frame from RTSP
            std::vector<uint8_t> rgb_data;
            int width, height;
            
            if (controller.captureFrame(rgb_data, width, height)) {
                // Analyze exposure
                ExposureMetrics metrics = controller.analyzeExposure(rgb_data, width, height);
                
                std::cout << "📊 Brightness: " << std::fixed << std::setprecision(1) 
                         << metrics.mean_brightness << "/255";
                
                if (metrics.mean_brightness < 100) {
                    std::cout << " (DARK 🌙)";
                } else if (metrics.mean_brightness > 180) {
                    std::cout << " (BRIGHT ☀️)";
                } else {
                    std::cout << " (GOOD ✅)";
                }
                std::cout << std::endl;
                
                std::cout << "📊 Contrast: " << metrics.contrast << std::endl;
                std::cout << "📊 Highlights clipped: " << metrics.clipped_highlights << "%" << std::endl;
                std::cout << "📊 Shadows clipped: " << metrics.clipped_shadows << "%" << std::endl;
                std::cout << "📊 Exposure score: " << metrics.exposure_score << "/100" << std::endl;
                
                // Get camera adjustment suggestions
                ZCAMSettings suggested = controller.suggestCameraSettings(metrics);
                std::cout << "💡 Analysis: " << suggested.reasoning << std::endl;
                
                if (suggested.iso != controller.getCurrentISO() || 
                    std::abs(suggested.exposure_compensation - controller.getCurrentEV()) > 0.1) {
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
                std::cout << "❌ Failed to capture frame, retrying..." << std::endl;
            }
            
            // Wait before next analysis
            std::this_thread::sleep_for(std::chrono::seconds(15));
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
}

/*
COMPILATION:
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libcurl4-openssl-dev libjsoncpp-dev

g++ -std=c++17 -O2 zcam_ffmpeg.cpp \
    -lavformat -lavcodec -lavutil -lswscale \
    -lcurl -ljsoncpp -pthread \
    -o zcam_ffmpeg_controller

USAGE:
./zcam_ffmpeg_controller 192.168.150.201

This version:
✅ Uses native FFmpeg C API (no OpenCV/GDAL conflicts)
✅ Properly handles ZCAM RTSP with TCP transport  
✅ Real-time exposure analysis from live stream
✅ ZCAM-specific camera setting suggestions
✅ Dual native ISO optimization (500/2500)
✅ Surf-specific exposure recommendations
*/