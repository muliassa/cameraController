#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
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

// Global flag for clean shutdown
std::atomic<bool> keep_running{true};

void signal_handler(int signal) {
    std::cout << "\n🛑 Received signal " << signal << ", shutting down gracefully..." << std::endl;
    keep_running = false;
}

struct ExposureMetrics {
    double mean_brightness = 0.0;
    std::vector<float> histogram;
    double dynamic_range = 0.0;
    double contrast = 0.0;
    double clipped_highlights = 0.0;
    double clipped_shadows = 0.0;
    double exposure_score = 0.0;
    
    // Advanced metrics
    double shadows_percent = 0.0;   // 0-85 range
    double midtones_percent = 0.0;  // 85-170 range  
    double highlights_percent = 0.0; // 170-255 range
    double saturation_level = 0.0;  // How close to clipping
    int total_pixels = 0;
    
    // NEW: Focus quality metrics
    double focus_sharpness = 0.0;    // Laplacian variance (higher = sharper)
    double edge_density = 0.0;       // Edge detection strength
    double high_freq_content = 0.0;  // High frequency content
    double focus_score = 0.0;        // Overall focus quality 0-100
};

struct ZCAMSettings {
    int iso;
    double exposure_compensation;
    std::string aperture;
    int shutter_angle;
    std::string reasoning;
    
    // Quality indicators
    bool is_native_iso = false;
    double confidence = 0.0;  // 0-1 how confident we are in this recommendation
};

struct CameraState {
    // Current settings (would be read from camera API)
    int current_iso = 500;
    double current_ev = 0.0;
    std::string current_aperture = "5.6";
    int current_shutter_angle = 180;
    
    // Scene analysis
    double sun_factor = 0.5;
    std::string scene_type = "unknown";
    
    // Targets
    double target_brightness = 128.0;
    double brightness_tolerance = 15.0;
};

class ZCAMExposureMonitor {
private:
    std::string camera_ip;
    std::string rtsp_url;
    std::string http_base_url;
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;
    SwsContext *sws_ctx = nullptr;
    int video_stream_index = -1;
    
    CURL *curl;
    CameraState camera_state;
    std::ofstream log_file;
    int analysis_count = 0;
    
    // Control settings
    bool auto_adjust_enabled = true;
    double confidence_threshold = 0.6;  // Only apply changes if confidence > 60%
    int changes_applied = 0;

public:
    ZCAMExposureMonitor(const std::string& camera_ip) : camera_ip(camera_ip) {
        rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        http_base_url = "http://" + camera_ip + "/ctrl";
        
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
        std::strftime(filename, sizeof(filename), "zcam_exposure_%Y%m%d_%H%M%S.log", &tm);
        log_file.open(filename);
        
        if (log_file.is_open()) {
            log_file << "ZCAM Exposure Monitor Log - Started: " 
                    << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << std::endl;
            log_file << "Camera IP: " << camera_ip << std::endl;
            log_file << "RTSP URL: " << rtsp_url << std::endl;
            log_file << "Target Brightness: " << camera_state.target_brightness << "/255" << std::endl;
            log_file << "----------------------------------------" << std::endl;
        }
        
        std::cout << "🎥 ZCAM Exposure Monitor with Auto-Control" << std::endl;
        std::cout << "📡 RTSP URL: " << rtsp_url << std::endl;
        std::cout << "🌐 HTTP API: " << http_base_url << std::endl;
        std::cout << "📝 Log file: " << filename << std::endl;
        std::cout << "🤖 Auto-adjust: " << (auto_adjust_enabled ? "ENABLED" : "DISABLED") << std::endl;
    }
    
    ~ZCAMExposureMonitor() {
        cleanup();
        avformat_network_deinit();
        
        if (curl) {
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
        
        if (log_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            log_file << "----------------------------------------" << std::endl;
            log_file << "Monitor stopped: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << std::endl;
            log_file << "Total analyses: " << analysis_count << std::endl;
            log_file << "Settings changed: " << changes_applied << std::endl;
            log_file.close();
        }
    }
    
    bool connect() {
        std::cout << "🔌 Connecting to ZCAM..." << std::endl;
        
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cout << "❌ Failed to allocate format context" << std::endl;
            return false;
        }
        
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);
        av_dict_set(&options, "max_delay", "3000000", 0);
        
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open stream: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ Connected to RTSP stream" << std::endl;
        
        // Skip stream info to avoid segfault, use manual detection
        if (!detectVideoStream()) {
            return false;
        }
        
        std::cout << "✅ Ready for exposure monitoring" << std::endl;
        return true;
    }
    
    bool detectVideoStream() {
        std::cout << "🔍 Detecting H.264 video stream..." << std::endl;
        
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) return false;
        
        std::vector<int> stream_sizes(format_ctx->nb_streams, 0);
        
        for (int i = 0; i < 30; i++) {
            int ret = av_read_frame(format_ctx, pkt);
            if (ret < 0) break;
            
            if (pkt->stream_index < static_cast<int>(stream_sizes.size())) {
                stream_sizes[pkt->stream_index] += pkt->size;
                
                // Detect H.264 NAL units in large packets
                if (pkt->size > 1000) {
                    uint8_t *data = pkt->data;
                    if (pkt->size >= 4 && 
                        ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
                         (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01))) {
                        video_stream_index = pkt->stream_index;
                        std::cout << "🎬 Found H.264 video stream #" << video_stream_index << std::endl;
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
        }
        
        av_packet_free(&pkt);
        
        if (video_stream_index < 0) {
            std::cout << "❌ No H.264 video stream found" << std::endl;
            return false;
        }
        
        return setupDecoder();
    }
    
    bool setupDecoder() {
        std::cout << "🔧 Setting up H.264 decoder..." << std::endl;
        
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cout << "❌ H.264 codec not available" << std::endl;
            return false;
        }
        
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cout << "❌ Failed to allocate codec context" << std::endl;
            return false;
        }
        
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_H264;
        
        int ret = avcodec_open2(codec_ctx, codec, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open codec: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ H.264 decoder ready" << std::endl;
        return true;
    }
    
    bool captureFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
        if (!format_ctx || !codec_ctx) return false;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgb_frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (rgb_frame) av_frame_free(&rgb_frame);
            return false;
        }
        
        bool success = false;
        int packets_read = 0;
        
        // Read until we get a frame (skip the H.264 initialization errors)
        while (packets_read < 200 && keep_running) {
            int ret = av_read_frame(format_ctx, packet);
            packets_read++;
            
            if (ret < 0) break;
            
            if (packet->stream_index == video_stream_index) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret == 0) {
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == 0) {
                        width = frame->width;
                        height = frame->height;
                        
                        // Setup color conversion
                        if (!sws_ctx) {
                            sws_ctx = sws_getContext(
                                width, height, (AVPixelFormat)frame->format,
                                width, height, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                            );
                        }
                        
                        if (sws_ctx) {
                            int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
                            rgb_data.resize(rgb_size);
                            
                            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize,
                                                rgb_data.data(), AV_PIX_FMT_RGB24, width, height, 1);
                            
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                    rgb_frame->data, rgb_frame->linesize);
                            
                            success = true;
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
        
        return success;
    }
    
    ExposureMetrics analyzeExposureAndFocus(const std::vector<uint8_t>& rgb_data, int width, int height, 
                                           int crop_x = 0, int crop_y = 0, int crop_w = 0, int crop_h = 0) {
        ExposureMetrics metrics;
        
        if (rgb_data.empty() || width <= 0 || height <= 0) {
            return metrics;
        }
        
        // Use crop area if specified, otherwise full frame
        int analyze_x = (crop_w > 0) ? crop_x : 0;
        int analyze_y = (crop_h > 0) ? crop_y : 0;
        int analyze_w = (crop_w > 0) ? crop_w : width;
        int analyze_h = (crop_h > 0) ? crop_h : height;
        
        // Ensure crop is within bounds
        analyze_x = std::max(0, std::min(analyze_x, width - 1));
        analyze_y = std::max(0, std::min(analyze_y, height - 1));
        analyze_w = std::min(analyze_w, width - analyze_x);
        analyze_h = std::min(analyze_h, height - analyze_y);
        
        metrics.total_pixels = analyze_w * analyze_h;
        metrics.histogram.resize(256, 0.0f);
        
        std::vector<uint8_t> gray_data;
        gray_data.reserve(metrics.total_pixels);
        
        double sum_brightness = 0.0;
        double sum_squared = 0.0;
        int highlight_count = 0;
        int shadow_count = 0;
        int shadows_range = 0;
        int midtones_range = 0;
        int highlights_range = 0;
        
        // Convert to grayscale for focus analysis
        std::vector<std::vector<uint8_t>> gray_matrix(analyze_h, std::vector<uint8_t>(analyze_w));
        
        for (int y = 0; y < analyze_h; y++) {
            for (int x = 0; x < analyze_w; x++) {
                int src_x = analyze_x + x;
                int src_y = analyze_y + y;
                size_t pixel_idx = static_cast<size_t>((src_y * width + src_x) * 3);
                
                if (pixel_idx + 2 < rgb_data.size()) {
                    uint8_t r = rgb_data[pixel_idx];
                    uint8_t g = rgb_data[pixel_idx + 1];
                    uint8_t b = rgb_data[pixel_idx + 2];
                    
                    uint8_t gray = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
                    gray_data.push_back(gray);
                    gray_matrix[y][x] = gray;
                    
                    // Exposure analysis
                    sum_brightness += gray;
                    sum_squared += gray * gray;
                    
                    if (gray >= 250) highlight_count++;
                    if (gray <= 5) shadow_count++;
                    
                    if (gray < 85) shadows_range++;
                    else if (gray < 170) midtones_range++;
                    else highlights_range++;
                    
                    metrics.histogram[gray]++;
                }
            }
        }
        
        // Calculate exposure metrics (same as before)
        if (metrics.total_pixels > 0) {
            metrics.mean_brightness = sum_brightness / metrics.total_pixels;
            double variance = (sum_squared / metrics.total_pixels) - (metrics.mean_brightness * metrics.mean_brightness);
            metrics.contrast = std::sqrt(std::max(0.0, variance));
            metrics.clipped_highlights = (highlight_count * 100.0) / metrics.total_pixels;
            metrics.clipped_shadows = (shadow_count * 100.0) / metrics.total_pixels;
            metrics.shadows_percent = (shadows_range * 100.0) / metrics.total_pixels;
            metrics.midtones_percent = (midtones_range * 100.0) / metrics.total_pixels;
            metrics.highlights_percent = (highlights_range * 100.0) / metrics.total_pixels;
            
            auto min_it = std::find_if(gray_data.begin(), gray_data.end(), [](uint8_t val) { return val > 5; });
            auto max_it = std::max_element(gray_data.begin(), gray_data.end());
            if (min_it != gray_data.end() && max_it != gray_data.end()) {
                metrics.dynamic_range = *max_it - *min_it;
            }
            
            int near_highlight = 0, near_shadow = 0;
            for (uint8_t gray : gray_data) {
                if (gray >= 240) near_highlight++;
                if (gray <= 15) near_shadow++;
            }
            metrics.saturation_level = std::max(
                (near_highlight * 100.0) / metrics.total_pixels,
                (near_shadow * 100.0) / metrics.total_pixels
            );
            
            for (auto& val : metrics.histogram) {
                val /= metrics.total_pixels;
            }
            
            metrics.exposure_score = calculateExposureScore(metrics);
        }
        
        // NEW: Calculate focus metrics
        metrics = calculateFocusMetrics(metrics, gray_matrix, analyze_w, analyze_h);
        
        return metrics;
    }
    
    ExposureMetrics calculateFocusMetrics(ExposureMetrics metrics, const std::vector<std::vector<uint8_t>>& gray_matrix, int width, int height) {
        if (width < 3 || height < 3) {
            return metrics;
        }
        
        double laplacian_sum = 0.0;
        double edge_sum = 0.0;
        double high_freq_sum = 0.0;
        int valid_pixels = 0;
        
        // Laplacian operator for sharpness detection
        for (int y = 1; y < height - 1; y++) {
            for (int x = 1; x < width - 1; x++) {
                // Laplacian kernel: [0 -1 0; -1 4 -1; 0 -1 0]
                double laplacian = -gray_matrix[y-1][x] - gray_matrix[y+1][x] - 
                                  gray_matrix[y][x-1] - gray_matrix[y][x+1] + 
                                  4 * gray_matrix[y][x];
                
                laplacian_sum += laplacian * laplacian;  // Variance of Laplacian
                
                // Sobel edge detection
                double gx = -gray_matrix[y-1][x-1] + gray_matrix[y-1][x+1] +
                           -2*gray_matrix[y][x-1] + 2*gray_matrix[y][x+1] +
                           -gray_matrix[y+1][x-1] + gray_matrix[y+1][x+1];
                           
                double gy = -gray_matrix[y-1][x-1] - 2*gray_matrix[y-1][x] - gray_matrix[y-1][x+1] +
                           gray_matrix[y+1][x-1] + 2*gray_matrix[y+1][x] + gray_matrix[y+1][x+1];
                           
                double edge_magnitude = std::sqrt(gx*gx + gy*gy);
                edge_sum += edge_magnitude;
                
                // High frequency content (difference from local average)
                double local_avg = (gray_matrix[y-1][x-1] + gray_matrix[y-1][x] + gray_matrix[y-1][x+1] +
                                   gray_matrix[y][x-1] + gray_matrix[y][x] + gray_matrix[y][x+1] +
                                   gray_matrix[y+1][x-1] + gray_matrix[y+1][x] + gray_matrix[y+1][x+1]) / 9.0;
                                   
                double high_freq = std::abs(gray_matrix[y][x] - local_avg);
                high_freq_sum += high_freq;
                
                valid_pixels++;
            }
        }
        
        if (valid_pixels > 0) {
            metrics.focus_sharpness = laplacian_sum / valid_pixels;      // Laplacian variance
            metrics.edge_density = edge_sum / valid_pixels;              // Average edge strength
            metrics.high_freq_content = high_freq_sum / valid_pixels;    // High frequency content
            
            // Normalize and combine into focus score (0-100)
            // These thresholds may need tuning based on your camera/lens
            double sharpness_norm = std::min(metrics.focus_sharpness / 500.0, 1.0);  // Normalize to 0-1
            double edge_norm = std::min(metrics.edge_density / 50.0, 1.0);
            double freq_norm = std::min(metrics.high_freq_content / 20.0, 1.0);
            
            metrics.focus_score = (sharpness_norm * 0.5 + edge_norm * 0.3 + freq_norm * 0.2) * 100.0;
        }
        
        return metrics;
    }
    
    double calculateExposureScore(const ExposureMetrics& metrics) {
        double score = 100.0;
        
        // Brightness deviation penalty
        double brightness_error = std::abs(metrics.mean_brightness - camera_state.target_brightness);
        score -= std::min(brightness_error * 1.5, 40.0);
        
        // Clipping penalties (harsh)
        score -= metrics.clipped_highlights * 3.0;
        score -= metrics.clipped_shadows * 2.5;
        
        // Contrast evaluation
        if (metrics.contrast < 25.0) {
            score -= (25.0 - metrics.contrast) * 0.8;  // Low contrast penalty
        } else if (metrics.contrast > 75.0) {
            score -= (metrics.contrast - 75.0) * 0.4;  // High contrast penalty
        }
        
        // Dynamic range evaluation
        if (metrics.dynamic_range < 180.0) {
            score -= (180.0 - metrics.dynamic_range) * 0.3;
        }
        
        // Tonal distribution bonus/penalty
        // Ideal: ~20% shadows, ~60% midtones, ~20% highlights
        double shadows_ideal = 20.0;
        double midtones_ideal = 60.0;
        double highlights_ideal = 20.0;
        
        score -= std::abs(metrics.shadows_percent - shadows_ideal) * 0.2;
        score -= std::abs(metrics.midtones_percent - midtones_ideal) * 0.1;
        score -= std::abs(metrics.highlights_percent - highlights_ideal) * 0.2;
        
        // Saturation penalty
        if (metrics.saturation_level > 10.0) {
            score -= (metrics.saturation_level - 10.0) * 1.5;
        }
        
        return std::max(0.0, std::min(100.0, score));
    }
    
    void displayResults(const ExposureMetrics& metrics, const ZCAMSettings& recommendations, int width, int height) {
        std::cout << "📊 Frame: " << width << "x" << height << std::endl;
        
        // Brightness analysis
        std::cout << "💡 Brightness: " << std::fixed << std::setprecision(1) 
                 << metrics.mean_brightness << "/255";
        if (metrics.mean_brightness < 80) {
            std::cout << " (VERY DARK 🌑)";
        } else if (metrics.mean_brightness < 110) {
            std::cout << " (DARK 🌙)";
        } else if (metrics.mean_brightness > 180) {
            std::cout << " (BRIGHT ☀️)";
        } else if (metrics.mean_brightness > 210) {
            std::cout << " (VERY BRIGHT 🔆)";
        } else {
            std::cout << " (GOOD ✅)";
        }
        std::cout << std::endl;
        
        // Contrast and quality metrics
        std::cout << "📈 Contrast: " << metrics.contrast;
        if (metrics.contrast < 20) {
            std::cout << " (LOW 📉)";
        } else if (metrics.contrast > 60) {
            std::cout << " (HIGH 📊)";
        } else {
            std::cout << " (GOOD ✅)";
        }
        std::cout << " | Dynamic Range: " << std::setprecision(0) << metrics.dynamic_range << "/255" << std::endl;
        
        // NEW: Focus quality metrics
        std::cout << "🔍 Focus Quality: " << std::setprecision(1) << metrics.focus_score << "/100";
        if (metrics.focus_score >= 75) {
            std::cout << " (SHARP ✅)";
        } else if (metrics.focus_score >= 50) {
            std::cout << " (ACCEPTABLE 📷)";
        } else if (metrics.focus_score >= 25) {
            std::cout << " (SOFT ⚠️)";
        } else {
            std::cout << " (BLURRY ❌)";
        }
        std::cout << " | Sharpness: " << std::setprecision(0) << metrics.focus_sharpnessca 
                 << " | Edges: " << metrics.edge_density << std::endl;
        
        // Clipping analysis
        std::cout << "⚠️ Clipping - Highlights: " << std::setprecision(2) << metrics.clipped_highlights << "%";
        if (metrics.clipped_highlights > 5.0) std::cout << " (HIGH)";
        std::cout << " | Shadows: " << metrics.clipped_shadows << "%";
        if (metrics.clipped_shadows > 10.0) std::cout << " (HIGH)";
        std::cout << std::endl;
        
        // Tonal distribution
        std::cout << "🎨 Tonal Distribution - Shadows: " << std::setprecision(1) << metrics.shadows_percent 
                 << "% | Midtones: " << metrics.midtones_percent 
                 << "% | Highlights: " << metrics.highlights_percent << "%" << std::endl;
        
        // Overall scores
        std::cout << "🎯 Exposure Score: " << metrics.exposure_score << "/100";
        if (metrics.exposure_score >= 80) {
            std::cout << " (EXCELLENT 🌟)";
        } else if (metrics.exposure_score >= 60) {
            std::cout << " (GOOD ✅)";
        } else if (metrics.exposure_score >= 40) {
            std::cout << " (FAIR ⚠️)";
        } else {
            std::cout << " (POOR ❌)";
        }
        std::cout << " | Scene: " << camera_state.scene_type << std::endl;
        
        // Current settings
        std::cout << "⚙️ Current Settings - ISO: " << camera_state.current_iso;
        if (camera_state.current_iso == 500 || camera_state.current_iso == 2500) {
            std::cout << " (native)";
        }
        std::cout << " | EV: " << std::showpos << camera_state.current_ev << std::noshowpos
                 << " | f/" << camera_state.current_aperture;
        if (camera_state.current_shutter_angle > 0) {
            std::cout << " | SA: " << camera_state.current_shutter_angle << "°";
        }
        std::cout << std::endl;
        
        // Recommendations
        bool needs_change = (recommendations.iso != camera_state.current_iso ||
                           std::abs(recommendations.exposure_compensation - camera_state.current_ev) > 0.1 ||
                           recommendations.aperture != camera_state.current_aperture ||
                           recommendations.shutter_angle != camera_state.current_shutter_angle);
        
        if (needs_change) {
            std::cout << "💡 Recommendations (confidence: " << std::setprecision(0) << (recommendations.confidence * 100) << "%) - ";
            
            if (recommendations.iso != camera_state.current_iso) {
                std::cout << "ISO: " << camera_state.current_iso << "→" << recommendations.iso;
                if (recommendations.is_native_iso) std::cout << " (native)";
                std::cout << " ";
            }
            
            if (std::abs(recommendations.exposure_compensation - camera_state.current_ev) > 0.1) {
                std::cout << "EV: " << std::showpos << camera_state.current_ev 
                         << "→" << recommendations.exposure_compensation << std::noshowpos << " ";
            }
            
            if (recommendations.aperture != camera_state.current_aperture) {
                std::cout << "f/" << camera_state.current_aperture << "→f/" << recommendations.aperture << " ";
            }
            
            if (recommendations.shutter_angle != camera_state.current_shutter_angle) {
                std::cout << "SA: " << camera_state.current_shutter_angle << "°→" << recommendations.shutter_angle << "° ";
            }
            
            std::cout << std::endl;
            std::cout << "🧠 Reasoning: " << recommendations.reasoning << std::endl;
        } else {
            std::cout << "✅ Current settings optimal - no changes recommended" << std::endl;
        }
    }
    
    double calculateSunAngleFactor() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        double hour = tm.tm_hour + tm.tm_min / 60.0;
        
        if (hour >= 6.0 && hour <= 22.0) {
            double solar_noon = 13.0;  // Adjust for your timezone
            double hour_angle = std::abs(hour - solar_noon);
            double sun_elevation = 90.0 - (hour_angle * 12.0);  // Rough approximation
            return std::max(0.1, sun_elevation / 90.0);
        } else {
            return 0.1;  // Night time
        }
    }
    
    std::string analyzeScene(const ExposureMetrics& metrics) {
        if (metrics.mean_brightness < 50) {
            if (metrics.shadows_percent > 70) return "Low light / Night";
            return "Underexposed scene";
        } else if (metrics.mean_brightness > 200) {
            if (metrics.highlights_percent > 50) return "Bright daylight";
            return "Overexposed scene";
        } else if (metrics.contrast < 20) {
            return "Flat lighting / Overcast";
        } else if (metrics.contrast > 60) {
            return "High contrast / Dramatic lighting";
        } else if (metrics.midtones_percent > 70) {
            return "Balanced lighting";
        } else {
            return "Mixed lighting conditions";
        }
    }
    
    ZCAMSettings recommendSettings(const ExposureMetrics& metrics) {
        ZCAMSettings settings;
        settings.iso = camera_state.current_iso;
        settings.exposure_compensation = camera_state.current_ev;
        settings.aperture = camera_state.current_aperture;
        settings.shutter_angle = camera_state.current_shutter_angle;
        settings.confidence = 0.5;  // Default confidence
        
        double brightness_error = metrics.mean_brightness - camera_state.target_brightness;
        camera_state.sun_factor = calculateSunAngleFactor();
        
        std::vector<std::string> reasons;
        
        // ISO recommendations - use actual ZCAM E8 Z2 ISO options
        if (brightness_error < -camera_state.brightness_tolerance) {
            // Too dark - increase ISO
            if (camera_state.current_iso <= 500) {
                settings.iso = 2500;  // Jump to high native ISO
                reasons.push_back("Dark scene - jump to native ISO 2500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso < 2500) {
                settings.iso = 2500;  // Go to native high ISO
                reasons.push_back("Increase to native ISO 2500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso == 2500 && brightness_error < -30) {
                settings.iso = 5000;  // Next available step up
                reasons.push_back("Very dark - increase beyond native ISO");
                settings.confidence += 0.2;
            }
        } else if (brightness_error > camera_state.brightness_tolerance) {
            // Too bright - decrease ISO (but 400 is the minimum manual setting)
            if (camera_state.current_iso > 500) {
                settings.iso = 500;   // Go to low native ISO first
                reasons.push_back("Bright scene - reduce to native ISO 500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso == 500) {
                settings.iso = 400;   // Minimum manual ISO
                reasons.push_back("Very bright - reduce to minimum ISO 400");
                settings.confidence += 0.2;
            } else if (camera_state.current_iso == 400) {
                // Already at minimum ISO - must use other controls
                reasons.push_back("At minimum ISO - use EV/aperture for brightness control");
                settings.confidence += 0.1;
            }
        } else {
            // Brightness OK - optimize for native ISO if possible
            if (camera_state.current_iso != 500 && camera_state.current_iso != 2500) {
                if (camera_state.current_iso < 1250) {
                    settings.iso = 500;
                    reasons.push_back("Optimize to native ISO 500");
                } else {
                    settings.iso = 2500;
                    reasons.push_back("Optimize to native ISO 2500");
                }
                settings.confidence += 0.1;
                settings.is_native_iso = true;
            }
        }
        
        // EV compensation - Use ZCAM's full range (-9.6 to +9.6 EV)
        double brightness_error = metrics.mean_brightness - camera_state.target_brightness;
        
        if (brightness_error > 60) {
            // Extremely bright (like your 196/255 case) - aggressive EV reduction
            // Reduce by additional -1.5 EV = -15 steps from current
            int current_steps = static_cast<int>(camera_state.current_ev * 10);
            int target_ev_steps = std::max(current_steps - 15, -96);
            settings.exposure_compensation = target_ev_steps / 10.0;
            reasons.push_back("Extremely bright - reduce EV by -1.5 more");
            settings.confidence += 0.5;
        } else if (brightness_error > 40) {
            // Very bright - moderate EV reduction 
            // Reduce by additional -1.0 EV = -10 steps from current
            int current_steps = static_cast<int>(camera_state.current_ev * 10);
            int target_ev_steps = std::max(current_steps - 10, -96);
            settings.exposure_compensation = target_ev_steps / 10.0;
            reasons.push_back("Very bright - reduce EV by -1.0 more");
            settings.confidence += 0.4;
        } else if (brightness_error > 20) {
            // Moderately bright - gentle EV reduction
            // Reduce by additional -0.7 EV = -7 steps from current
            int current_steps = static_cast<int>(camera_state.current_ev * 10);
            int target_ev_steps = std::max(current_steps - 7, -96);
            settings.exposure_compensation = target_ev_steps / 10.0;
            reasons.push_back("Bright scene - reduce EV by -0.7 more");
            settings.confidence += 0.3;
        } else if (brightness_error < -30) {
            // Too dark - increase EV
            // Increase by +1.0 EV = +10 steps from current
            int current_steps = static_cast<int>(camera_state.current_ev * 10);
            int target_ev_steps = std::min(current_steps + 10, 96);
            settings.exposure_compensation = target_ev_steps / 10.0;
            reasons.push_back("Too dark - increase EV by +1.0");
            settings.confidence += 0.3;
        } else if (metrics.clipped_highlights > 3.0) {
            // Highlight protection
            int current_steps = static_cast<int>(camera_state.current_ev * 10);
            int target_ev_steps = std::max(current_steps - 5, -96);
            settings.exposure_compensation = target_ev_steps / 10.0;
            reasons.push_back("Highlight protection - reduce EV by -0.5");
            settings.confidence += 0.2;
        }
        
        // Aperture recommendations - CRITICAL for bright scene control
        if (metrics.mean_brightness > 190) {
            // Extremely bright scenes (like your 197/255)
            settings.aperture = "22";  // Smallest aperture for maximum light reduction
            if (camera_state.current_aperture != "22") {
                reasons.push_back("Extremely bright - use f/22 to reduce light");
                settings.confidence += 0.4;
            }
        } else if (metrics.mean_brightness > 170) {
            // Very bright scenes  
            settings.aperture = "18";  // Very small aperture
            if (camera_state.current_aperture != "18") {
                reasons.push_back("Very bright - use f/18 for light control");
                settings.confidence += 0.3;
            }
        } else if (metrics.mean_brightness > 150) {
            // Bright daylight
            settings.aperture = "16";  // Small aperture for daylight
            if (camera_state.current_aperture != "16") {
                reasons.push_back("Bright daylight - use f/16");
                settings.confidence += 0.2;
            }
        } else if (camera_state.sun_factor > 0.7 || metrics.mean_brightness > 140) {
            // Standard bright conditions
            settings.aperture = "11";
            if (camera_state.current_aperture != "11") {
                reasons.push_back("Bright conditions - use f/11");
                settings.confidence += 0.2;
            }
        } else if (metrics.mean_brightness < 80) {
            // Low light - wide aperture
            settings.aperture = "2.8";
            if (camera_state.current_aperture != "2.8") {
                reasons.push_back("Low light - use f/2.8 for more light");
                settings.confidence += 0.3;
            }
        } else if (metrics.mean_brightness < 110) {
            // Moderate low light
            settings.aperture = "4";
            if (camera_state.current_aperture != "4") {
                reasons.push_back("Moderate light - use f/4");
                settings.confidence += 0.2;
            }
        } else {
            // Balanced conditions
            settings.aperture = "8";
            if (camera_state.current_aperture != "8") {
                reasons.push_back("Balanced lighting - use f/8");
                settings.confidence += 0.1;
            }
        }
        
        // Shutter angle - Additional light control
        if (metrics.mean_brightness > 180) {
            settings.shutter_angle = 90;   // Very fast shutter for very bright scenes
            if (camera_state.current_shutter_angle != 90) {
                reasons.push_back("Very bright - use 90° shutter to reduce light");
                settings.confidence += 0.2;
            }
        } else if (camera_state.sun_factor > 0.6 && metrics.mean_brightness > 140) {
            settings.shutter_angle = 120;  // Fast shutter for bright conditions
            if (camera_state.current_shutter_angle != 120) {
                reasons.push_back("Bright conditions - use 120° shutter");
                settings.confidence += 0.1;
            }
        } else if (metrics.mean_brightness < 80.0) {
            settings.shutter_angle = 270;  // Slower shutter for more light
            if (camera_state.current_shutter_angle != 270) {
                reasons.push_back("Low light - use 270° shutter for more light");
                settings.confidence += 0.1;
            }
        } else {
            settings.shutter_angle = 180;  // Standard cinematic
        }
        
        // Generate reasoning
        if (reasons.empty()) {
            settings.reasoning = "Current settings optimal for conditions";
            settings.confidence = std::max(0.8, settings.confidence);
        } else {
            settings.reasoning = reasons[0];
            for (size_t i = 1; i < reasons.size() && i < 3; i++) {
                settings.reasoning += "; " + reasons[i];
            }
        }
        
        // Adjust confidence based on scene complexity
        if (metrics.contrast < 15.0 || metrics.contrast > 80.0) {
            settings.confidence *= 0.8;  // Lower confidence in extreme contrast
        }
        if (metrics.exposure_score > 75.0) {
            settings.confidence += 0.1;  // Higher confidence when current exposure is good
        }
        
        settings.confidence = std::min(1.0, settings.confidence);
        
        return settings;
    }
    
    void logAnalysis(const ExposureMetrics& metrics, const ZCAMSettings& recommendations, int frame_width, int frame_height) {
        if (!log_file.is_open()) return;
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        log_file << std::fixed << std::setprecision(2);
        log_file << "[" << std::put_time(&tm, "%H:%M:%S") << "] ";
        log_file << "Analysis #" << analysis_count << " | ";
        log_file << frame_width << "x" << frame_height << " | ";
        log_file << "Brightness:" << metrics.mean_brightness << " | ";
        log_file << "Contrast:" << metrics.contrast << " | ";
        log_file << "Score:" << metrics.exposure_score << " | ";
        log_file << "H-Clip:" << metrics.clipped_highlights << "% | ";
        log_file << "S-Clip:" << metrics.clipped_shadows << "% | ";
        log_file << "Tonal(S/M/H):" << metrics.shadows_percent << "/" 
                 << metrics.midtones_percent << "/" << metrics.highlights_percent << " | ";
        
        // Current settings
        log_file << "Current(ISO:" << camera_state.current_iso 
                 << ",EV:" << camera_state.current_ev 
                 << ",f/" << camera_state.current_aperture 
                 << ",SA:" << camera_state.current_shutter_angle << ") | ";
        
        // Recommendations
        log_file << "Rec(ISO:" << recommendations.iso 
                 << ",EV:" << recommendations.exposure_compensation 
                 << ",f/" << recommendations.aperture 
                 << ",SA:" << recommendations.shutter_angle << ") | ";
        log_file << "Conf:" << (recommendations.confidence * 100) << "% | ";
        log_file << "Scene:" << camera_state.scene_type << " | ";
        log_file << "Reason:" << recommendations.reasoning << std::endl;
        
        log_file.flush();
    }
    
    // ═══════════════════════════════════════════════════════════════════
    // ZCAM HTTP API CONTROL METHODS
    // ═══════════════════════════════════════════════════════════════════
    
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
    
    HTTPResponse sendHTTPRequest(const std::string& endpoint, const std::string& method = "GET", const std::string& data = "") {
        std::cout << "🌐 HTTP Request: " << endpoint << std::endl;
        
        HTTPResponse response;
        
        if (!curl) {
            std::cout << "❌ CURL not initialized" << std::endl;
            return response;
        }
        
        std::string url = http_base_url + endpoint;
        std::cout << "🔗 Full URL: " << url << std::endl;
        
        // Reset curl handle
        curl_easy_reset(curl);
        
        // Set basic options
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        
        // Disable SSL verification for local network
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        
        struct curl_slist *headers = nullptr;
        
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.length());
            
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }
        
        // Perform the request
        CURLcode res = curl_easy_perform(curl);
        
        // Clean up headers
        if (headers) {
            curl_slist_free_all(headers);
        }
        
        // Get response info
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.response_code);
        
        std::cout << "📡 CURL Result: " << res << " | HTTP Code: " << response.response_code << std::endl;
        std::cout << "📄 Response: " << response.data.substr(0, 200) << std::endl;
        
        response.success = (res == CURLE_OK && response.response_code == 200);
        
        if (!response.success) {
            std::cout << "❌ Request failed - CURL: " << curl_easy_strerror(res) << std::endl;
        }
        
        return response;
    }
    
    bool getCurrentCameraSettings() {
        std::cout << "🔍 Reading current ZCAM E8 Z2 settings..." << std::endl;
        
        // Get current ISO - using the actual response format
        HTTPResponse iso_resp = sendHTTPRequest("/ctrl/get?k=iso");
        if (iso_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(iso_resp.data, root)) {
                if (root.isMember("code") && root["code"].asInt() == 0 && root.isMember("value")) {
                    std::string iso_str = root["value"].asString();
                    camera_state.current_iso = std::stoi(iso_str);
                    std::cout << "   📊 Current ISO: " << camera_state.current_iso << std::endl;
                    
                    // Show available ISO options
                    if (root.isMember("opts") && root["opts"].isArray()) {
                        std::cout << "   🎚️ Available ISOs: ";
                        for (const auto& iso_opt : root["opts"]) {
                            std::cout << iso_opt.asString() << " ";
                        }
                        std::cout << std::endl;
                    }
                } else {
                    std::cout << "   ❌ Unexpected ISO response format" << std::endl;
                    std::cout << "   Response: " << iso_resp.data << std::endl;
                }
            } else {
                std::cout << "   ❌ Failed to parse ISO JSON response" << std::endl;
                std::cout << "   Raw response: " << iso_resp.data << std::endl;
            }
        } else {
            std::cout << "   ⚠️ Could not read ISO (HTTP " << iso_resp.response_code << ")" << std::endl;
        }
        
        // Get current EV - FIXED to read integer values correctly
        HTTPResponse ev_resp = sendHTTPRequest("/ctrl/get?k=ev");
        if (ev_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(ev_resp.data, root)) {
                if (root.isMember("code") && root["code"].asInt() == 0 && root.isMember("value")) {
                    int ev_steps = root["value"].asInt();  // Get integer steps
                    camera_state.current_ev = ev_steps / 10.0;  // Convert to actual EV
                    std::cout << "   📊 Current EV: " << std::showpos << camera_state.current_ev 
                             << std::noshowpos << " (steps: " << ev_steps << ")" << std::endl;
                    
                    // Show EV range info
                    if (root.isMember("min") && root.isMember("max")) {
                        int min_steps = root["min"].asInt();
                        int max_steps = root["max"].asInt();
                        std::cout << "   📊 EV Range: " << (min_steps/10.0) << " to " << (max_steps/10.0) 
                                 << " (" << min_steps << " to " << max_steps << " steps)" << std::endl;
                    }
                } else {
                    std::cout << "   ❌ Unexpected EV response format" << std::endl;
                    std::cout << "   Response: " << ev_resp.data << std::endl;
                }
            } else {
                std::cout << "   ❌ Failed to parse EV JSON response" << std::endl;
                std::cout << "   Raw response: " << ev_resp.data << std::endl;
            }
        } else {
            std::cout << "   ⚠️ Could not read EV (HTTP " << ev_resp.response_code << ")" << std::endl;
        }
        
        // Get white balance
        HTTPResponse wb_resp = sendHTTPRequest("/ctrl/get?k=wb");
        if (wb_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(wb_resp.data, root) && root.isMember("code") && 
                root["code"].asInt() == 0 && root.isMember("value")) {
                std::cout << "   📊 White Balance: " << root["value"].asString() << std::endl;
            }
        }
        
        // Get current shutter angle/speed - TEST IF AVAILABLE
        HTTPResponse shutter_resp = sendHTTPRequest("/ctrl/get?k=shutter_angle");
        if (shutter_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(shutter_resp.data, root)) {
                if (root.isMember("code") && root["code"].asInt() == 0 && root.isMember("value")) {
                    camera_state.current_shutter_angle = root["value"].asInt();
                    std::cout << "   📊 Current Shutter Angle: " << camera_state.current_shutter_angle << "°" << std::endl;
                    
                    if (root.isMember("opts") && root["opts"].isArray()) {
                        std::cout << "   🎚️ Available Shutter Angles: ";
                        for (const auto& shutter_opt : root["opts"]) {
                            std::cout << shutter_opt.asString() << "° ";
                        }
                        std::cout << std::endl;
                    }
                } else {
                    std::cout << "   ❌ Unexpected shutter response format" << std::endl;
                }
            }
        } else {
            // Try alternative shutter speed parameter
            HTTPResponse shutter_speed_resp = sendHTTPRequest("/ctrl/get?k=shutter_speed");
            if (shutter_speed_resp.success) {
                Json::Value root;
                Json::Reader reader;
                if (reader.parse(shutter_speed_resp.data, root) && root.isMember("code") && 
                    root["code"].asInt() == 0 && root.isMember("value")) {
                    std::string shutter_str = root["value"].asString();
                    std::cout << "   📊 Current Shutter Speed: " << shutter_str << std::endl;
                }
            } else {
                std::cout << "   ⚠️ Shutter control not available via API" << std::endl;
            }
        }
        HTTPResponse aperture_resp = sendHTTPRequest("/ctrl/get?k=iris");
        if (aperture_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(aperture_resp.data, root)) {
                if (root.isMember("code") && root["code"].asInt() == 0 && root.isMember("value")) {
                    camera_state.current_aperture = root["value"].asString();
                    std::cout << "   📊 Current Aperture: f/" << camera_state.current_aperture << std::endl;
                    
                    // Show available aperture options
                    if (root.isMember("opts") && root["opts"].isArray()) {
                        std::cout << "   🎚️ Available Apertures: f/";
                        for (const auto& apt_opt : root["opts"]) {
                            std::cout << apt_opt.asString() << " ";
                        }
                        std::cout << std::endl;
                    }
                } else {
                    std::cout << "   ❌ Unexpected aperture response format" << std::endl;
                    std::cout << "   Response: " << aperture_resp.data << std::endl;
                }
            }
        } else {
            std::cout << "   ⚠️ Could not read aperture (HTTP " << aperture_resp.response_code << ")" << std::endl;
        }
        
        // Check recording status  
        HTTPResponse rec_resp = sendHTTPRequest("/ctrl/get?k=rec");
        if (rec_resp.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(rec_resp.data, root) && root.isMember("code") && 
                root["code"].asInt() == 0 && root.isMember("value")) {
                std::string rec_status = root["value"].asString();
                std::cout << "   📹 Recording: " << (rec_status == "on" ? "🔴 RECORDING" : "⏸️ STANDBY") << std::endl;
            }
        }
        
        // Determine profile based on your JS logic  
        std::string profile = "custom";
        if (camera_state.current_iso == 400) profile = "day";
        else if (camera_state.current_iso == 51200) profile = "night";
        std::cout << "   🎬 Profile: " << profile << std::endl;
        
        return iso_resp.success;
    }
    
    bool applyCameraSetting(const std::string& parameter, const std::string& value, const std::string& description) {
        std::cout << "🔧 Setting " << description << " to " << value << "..." << std::endl;
        
        // Use your working JS format: /ctrl/set?key=value
        std::string endpoint = "/ctrl/set?" + parameter + "=" + value;
        HTTPResponse response = sendHTTPRequest(endpoint);
        
        if (response.success) {
            // Parse response to check if setting was actually applied
            Json::Value root;
            Json::Reader reader;
            bool setting_applied = false;
            
            if (reader.parse(response.data, root)) {
                // ZCAM usually returns {"code": 0} for success
                if (root.isMember("code") && root["code"].asInt() == 0) {
                    setting_applied = true;
                } else if (root.isMember("result") && root["result"].asString() == "ok") {
                    setting_applied = true;
                } else if (response.data.find("ok") != std::string::npos) {
                    setting_applied = true;
                }
            } else {
                // If JSON parsing fails but HTTP was OK, assume success
                setting_applied = true;
            }
            
            if (setting_applied) {
                std::cout << "   ✅ " << description << " updated successfully" << std::endl;
                
                // Log the change
                if (log_file.is_open()) {
                    auto now = std::chrono::system_clock::now();
                    auto time_t = std::chrono::system_clock::to_time_t(now);
                    auto tm = *std::localtime(&time_t);
                    log_file << "[" << std::put_time(&tm, "%H:%M:%S") << "] SETTING_CHANGE: " 
                             << parameter << "=" << value << " (" << description << ") - SUCCESS" << std::endl;
                }
                
                return true;
            } else {
                std::cout << "   ❌ Camera rejected " << description << " change" << std::endl;
                std::cout << "   Response: " << response.data << std::endl;
                return false;
            }
        } else {
            std::cout << "   ❌ Failed to set " << description << " (HTTP " << response.response_code << ")" << std::endl;
            std::cout << "   Response: " << response.data.substr(0, 100) << std::endl;
            return false;
        }
    }
    
    bool applyRecommendations(const ZCAMSettings& recommendations) {
        if (!auto_adjust_enabled) {
            std::cout << "🚫 Auto-adjust disabled, skipping camera changes" << std::endl;
            return false;
        }
        
        if (recommendations.confidence < confidence_threshold) {
            std::cout << "⚠️ Low confidence (" << (recommendations.confidence * 100) << "%), skipping changes" << std::endl;
            return false;
        }
        
        std::cout << "\n🤖 Applying recommended settings (confidence: " << (recommendations.confidence * 100) << "%)..." << std::endl;
        
        bool any_changes = false;
        int successful_changes = 0;
        
        // Apply ISO changes
        if (recommendations.iso != camera_state.current_iso) {
            if (applyCameraSetting("iso", std::to_string(recommendations.iso), "ISO")) {
                camera_state.current_iso = recommendations.iso;
                successful_changes++;
                any_changes = true;
            }
        }
        
        // Apply EV changes using correct parameter name and integer values
        if (std::abs(recommendations.exposure_compensation - camera_state.current_ev) > 0.05) {
            // Convert EV to integer steps for ZCAM API (multiply by 10)
            int ev_steps = static_cast<int>(recommendations.exposure_compensation * 10);
            if (applyCameraSetting("ev", std::to_string(ev_steps), "EV Compensation")) {
                camera_state.current_ev = recommendations.exposure_compensation;
                successful_changes++;
                any_changes = true;
            }
        }
        
        // Apply aperture changes using 'iris' parameter
        if (recommendations.aperture != camera_state.current_aperture) {
            if (applyCameraSetting("iris", recommendations.aperture, "Aperture")) {
                camera_state.current_aperture = recommendations.aperture;
                successful_changes++;
                any_changes = true;
            }
        }
        
        // Apply shutter angle changes  
        if (recommendations.shutter_angle != camera_state.current_shutter_angle) {
            if (applyCameraSetting("shutter_angle", std::to_string(recommendations.shutter_angle), "Shutter Angle")) {
                camera_state.current_shutter_angle = recommendations.shutter_angle;
                successful_changes++;
                any_changes = true;
            }
        }
        
        if (any_changes) {
            changes_applied++;
            std::cout << "✅ Applied " << successful_changes << " setting changes" << std::endl;
            
            // Wait a moment for camera to adjust
            std::cout << "⏳ Waiting 3 seconds for camera adjustment..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        } else {
            std::cout << "✅ No changes needed - current settings are optimal" << std::endl;
        }
        
        return any_changes;
    }
    
    bool testCameraConnection() {
        std::cout << "🧪 Testing ZCAM E8 Z2 HTTP API connection..." << std::endl;
        
        // Try to get camera info using the actual response format
        HTTPResponse response = sendHTTPRequest("/ctrl/get?k=iso");
        if (response.success) {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(response.data, root)) {
                if (root.isMember("code") && root["code"].asInt() == 0 && root.isMember("value")) {
                    std::cout << "✅ Camera API connection successful" << std::endl;
                    std::cout << "   Current ISO: " << root["value"].asString() << std::endl;
                    std::cout << "   Description: " << root["desc"].asString() << std::endl;
                    return true;
                } else {
                    std::cout << "❌ Unexpected response format" << std::endl;
                    std::cout << "   Response: " << response.data << std::endl;
                }
            } else {
                std::cout << "❌ Failed to parse JSON response" << std::endl;
                std::cout << "   Raw response: " << response.data << std::endl;
            }
        } else {
            std::cout << "❌ Camera API connection failed (HTTP " << response.response_code << ")" << std::endl;
        }
        
        std::cout << "💡 Make sure camera HTTP API is enabled and accessible" << std::endl;
        std::cout << "💡 Try accessing http://" << camera_ip << "/ctrl/get?k=iso in browser" << std::endl;
        return false;
    }
    
    void runMonitoring() {
        if (!connect()) {
            std::cout << "❌ Failed to connect to camera" << std::endl;
            return;
        }
        
        std::cout << "\n🎬 Starting exposure monitoring..." << std::endl;
        std::cout << "📊 Target brightness: " << camera_state.target_brightness << "/255" << std::endl;
        std::cout << "⏱️  Analysis interval: 15 seconds" << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        while (keep_running) {
            analysis_count++;
            
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            
            std::cout << "\n━━━ Analysis #" << analysis_count 
                     << " (" << std::put_time(&tm, "%H:%M:%S") << ") ━━━" << std::endl;
            
            // Capture and analyze frame
            std::vector<uint8_t> rgb_data;
            int width, height;
            
            if (captureFrame(rgb_data, width, height)) {
                ExposureMetrics metrics = analyzeExposure(rgb_data, width, height);
                camera_state.scene_type = analyzeScene(metrics);
                ZCAMSettings recommendations = recommendSettings(metrics);
                
                // Display results
                displayResults(metrics, recommendations, width, height);
                
    void runMonitoring() {
        // Test camera connections
        if (!testCameraConnection()) {
            std::cout << "⚠️ Camera HTTP API not accessible - continuing with monitoring only" << std::endl;
            auto_adjust_enabled = false;
        }
        
        if (!connect()) {
            std::cout << "❌ Failed to connect to camera" << std::endl;
            return;
        }
        
        // Get initial camera settings
        if (auto_adjust_enabled) {
            getCurrentCameraSettings();
        }
        
        std::cout << "\n🎬 Starting exposure monitoring with auto-control..." << std::endl;
        std::cout << "📊 Target brightness: " << camera_state.target_brightness << "/255" << std::endl;
        std::cout << "⏱️  Analysis interval: 15 seconds" << std::endl;
        std::cout << "🤖 Auto-adjust: " << (auto_adjust_enabled ? "ENABLED" : "DISABLED") << std::endl;
        std::cout << "🎚️ Confidence threshold: " << (confidence_threshold * 100) << "%" << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        while (keep_running) {
            analysis_count++;
            
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto tm = *std::localtime(&time_t);
            
            std::cout << "\n━━━ Analysis #" << analysis_count 
                     << " (" << std::put_time(&tm, "%H:%M:%S") << ") ━━━" << std::endl;
            
            // Capture and analyze frame
            std::vector<uint8_t> rgb_data;
            int width, height;
            
            if (captureFrame(rgb_data, width, height)) {
                ExposureMetrics metrics = analyzeExposure(rgb_data, width, height);
                camera_state.scene_type = analyzeScene(metrics);
                ZCAMSettings recommendations = recommendSettings(metrics);
                
                // Display results
                displayResults(metrics, recommendations, width, height);
                
                // Apply recommendations if auto-adjust is enabled
                if (auto_adjust_enabled) {
                    bool applied = applyRecommendations(recommendations);
                    if (applied) {
                        // Wait a bit longer after applying changes
                        std::cout << "⏳ Allowing extra time for settings to take effect..." << std::endl;
                        for (int i = 0; i < 10 && keep_running; i++) {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                    }
                }
                
                // Log to file
                logAnalysis(metrics, recommendations, width, height);
                
            } else {
                std::cout << "❌ Failed to capture frame" << std::endl;
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                auto tm = *std::localtime(&time_t);
                if (log_file.is_open()) {
                    log_file << "[" << std::put_time(&tm, "%H:%M:%S") << "] CAPTURE_FAILED" << std::endl;
                }
            }
            
            // Wait before next analysis (but check keep_running frequently)
            std::cout << "\n⏳ Waiting 15 seconds for next analysis..." << std::endl;
            for (int i = 0; i < 15 && keep_running; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        
        std::cout << "\n✅ Monitoring stopped gracefully" << std::endl;
        std::cout << "📊 Total analyses: " << analysis_count << std::endl;
        std::cout << "🔧 Settings changed: " << changes_applied << " times" << std::endl;
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

// Main function
int main(int argc, char* argv[]) {
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::string camera_ip = "192.168.150.201";
    
    if (argc > 1) {
        camera_ip = argv[1];
    }
    
    try {
        std::cout << "=== ZCAM E2-F8 Auto-Exposure Controller ===" << std::endl;
        std::cout << "Camera IP: " << camera_ip << std::endl;
        std::cout << "Features: RTSP monitoring + HTTP API control" << std::endl;
        std::cout << "Press Ctrl+C to stop\n" << std::endl;
        
        ZCAMExposureMonitor monitor(camera_ip);
        monitor.runMonitoring();
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n💥 Error: " << e.what() << std::endl;
        return -1;
    } catch (...) {
        std::cerr << "\n💥 Unknown error occurred" << std::endl;
        return -1;
    }
}