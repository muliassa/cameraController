#include <zcamController.h>

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <chrono>
#include <curl/curl.h>
#include <json/json.h>

#include <someLogger.h>
#include <someNetwork.h>
#include <someFFMpeg.h>

using namespace std;


struct ExposureMetrics {

    double mean_brightness;
    std::vector<float> histogram;
    double dynamic_range;
    double contrast;
    double clipped_highlights;
    double clipped_shadows;
    double exposure_score;
   
    // Advanced metrics
    double shadows_percent = 0.0;   // 0-85 range
    double midtones_percent = 0.0;  // 85-170 range  
    double highlights_percent = 0.0; // 170-255 range
    double saturation_level = 0.0;  // How close to clipping
    int total_pixels = 0;
};

struct ZCAMSettings {
    int iso;
    double exposure_compensation;
    string aperture;
    int shutter_angle;
    string reasoning;
    
    // Quality indicators
    bool is_native_iso = false;
    double confidence = 0.0;  // 0-1 how confident we are in this recommendation
};

    CURL *curl;

    CameraState camera_state;
    ExposureMetrics exposure_metrics;

    // FFmpeg components
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    const AVCodec *codec = nullptr;  // Use const AVCodec* for newer FFmpeg versions
    SwsContext *sws_ctx = nullptr;
    int video_stream_index = -1;
        
    // ZCAM E2 parameter ranges
    vector<int> iso_values = {100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800};
    vector<int> native_iso_values = {500, 2500}; // Dual native ISO for E2
    pair<double, double> ev_range = {-3.0, 3.0};
    vector<string> aperture_values = {"1.4", "1.6", "1.8", "2.0", "2.2", "2.5", "2.8", "3.2", "3.5", "4.0", "4.5", "5.0", "5.6", "6.3", "7.1", "8.0", "9.0", "10", "11", "13", "14", "16"};
    
    // Control settings
    bool auto_adjust_enabled = true;
    double confidence_threshold = 0.6;  // Only apply changes if confidence > 60%
    int changes_applied = 0;

    ZCAMController::ZCAMController(const json& config, const int cam_idx) {

        rtsp_url = "rtsp://" + ip + "/live_stream";
        http_base_url = "http://" + ip + "/ctrl";
        camera_ip = config["ipaddr"][cam_idx].get<string>();
        camera_id = config["camera"][cam_idx].get<string>();
        server = config["server"].get<string>();
        
        // Initialize FFmpeg
        #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();
        #endif
        avformat_network_init();
        
        cout << "🎥 ZCAM Simple Frame Capture" << endl;
        cout << "📡 RTSP URL: " << rtsp_url << endl;

    }
    
    ZCAMController::~ZCAMController() {
        cleanup();
        avformat_network_deinit();
    }
    
    double ZCAMController::calculateExposureScore(const ExposureMetrics& metrics) {
        double score = 100.0;
        
        // Penalize brightness deviation from target
        double brightness_error = std::abs(metrics.mean_brightness - camera_state.target_brightness);
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
    
    int ZCAMController::findClosestISO(int target_iso) {
        auto it = std::lower_bound(iso_values.begin(), iso_values.end(), target_iso);
        if (it == iso_values.end()) return iso_values.back();
        if (it == iso_values.begin()) return iso_values.front();
        
        int upper = *it;
        int lower = *(--it);
        
        return (target_iso - lower < upper - target_iso) ? lower : upper;
    }
    
    string ZCAMController::findClosestAperture(double target_f) {
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
    
    double ZCAMController::calculateSunAngleFactor() {
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

   ZCAMSettings ZCAMController::recommendSettings(const ExposureMetrics& metrics) {

        ZCAMSettings settings;
        settings.iso = camera_state.current_iso;
        settings.exposure_compensation = camera_state.current_ev;
        settings.aperture = camera_state.current_aperture;
        settings.shutter_angle = camera_state.current_shutter_angle;
        settings.confidence = 0.5;  // Default confidence
        
        double brightness_error = metrics.mean_brightness - camera_state.target_brightness;
        camera_state.sun_factor = calculateSunAngleFactor();
        
        std::vector<std::string> reasons;
        
        // ISO recommendations (prefer ZCAM native ISOs: 500, 2500)
        if (brightness_error < -camera_state.brightness_tolerance) {
            // Too dark - increase ISO
            if (camera_state.current_iso <= 500) {
                settings.iso = 2500;  // Jump to high native ISO for significant darkness
                reasons.push_back("Dark scene - jump to native ISO 2500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso < 2500) {
                settings.iso = 2500;  // Go to native high ISO
                reasons.push_back("Increase to native ISO 2500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso == 2500 && brightness_error < -30) {
                settings.iso = 5000;  // Only go higher if very dark
                reasons.push_back("Very dark - increase beyond native ISO");
                settings.confidence += 0.2;
            }
        } else if (brightness_error > camera_state.brightness_tolerance) {
            // Too bright - decrease ISO
            if (camera_state.current_iso > 2500) {
                settings.iso = 2500;  // Back to high native
                reasons.push_back("Reduce to native ISO 2500");
                settings.confidence += 0.2;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso == 2500) {
                settings.iso = 500;   // Back to low native
                reasons.push_back("Bright scene - reduce to native ISO 500");
                settings.confidence += 0.3;
                settings.is_native_iso = true;
            } else if (camera_state.current_iso > 500) {
                settings.iso = 500;   // Back to low native
                reasons.push_back("Return to native ISO 500");
                settings.confidence += 0.2;
                settings.is_native_iso = true;
            }
        } else {
            // Brightness OK - optimize for native ISO if not already
            if (camera_state.current_iso != 500 && camera_state.current_iso != 2500) {
                // Choose the native ISO that's closer to current
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
        
        // EV compensation for fine-tuning
        if (metrics.clipped_highlights > 3.0) {
            settings.exposure_compensation = std::max(camera_state.current_ev - 0.7, -2.0);
            reasons.push_back("Reduce EV (highlight protection)");
            settings.confidence += 0.2;
        } else if (metrics.clipped_shadows > 8.0 && metrics.mean_brightness < 100.0) {
            settings.exposure_compensation = std::min(camera_state.current_ev + 0.5, 2.0);
            reasons.push_back("Increase EV (shadow recovery)");
            settings.confidence += 0.2;
        } else if (metrics.saturation_level > 15.0) {
            settings.exposure_compensation = std::max(camera_state.current_ev - 0.3, -2.0);
            reasons.push_back("Slight EV reduction (saturation protection)");
            settings.confidence += 0.1;
        }
        
        // Aperture recommendations based on lighting and scene
        if (camera_state.sun_factor > 0.8) {
            // Bright daylight
            settings.aperture = "8.0";
            if (camera_state.current_aperture != "8.0") {
                reasons.push_back("Daylight aperture for sharpness");
                settings.confidence += 0.1;
            }
        } else if (camera_state.sun_factor < 0.3) {
            // Low light
            settings.aperture = "2.8";
            if (camera_state.current_aperture != "2.8") {
                reasons.push_back("Wide aperture for low light");
                settings.confidence += 0.2;
            }
        } else if (metrics.contrast > 60.0) {
            // High contrast scene
            settings.aperture = "5.6";
            reasons.push_back("Balanced aperture for contrast");
        }
        
        // Shutter angle for surf motion
        if (camera_state.sun_factor > 0.6 && metrics.contrast > 40.0) {
            settings.shutter_angle = 180;  // Standard cinematic motion
        } else if (metrics.mean_brightness < 80.0) {
            settings.shutter_angle = 270;  // More light for low conditions
            if (camera_state.current_shutter_angle != 270) {
                reasons.push_back("Wider shutter for low light");
                settings.confidence += 0.1;
            }
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
    
    string getAdjustmentReasoning(double brightness_error, const ExposureMetrics& metrics, double sun_factor) {
        std::vector<std::string> reasons;
        
        if (std::abs(brightness_error) > camera_state.brightness_tolerance) {
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
    
    double ZCAMController::getSunAngleFactor() {
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
    
    bool ZCAMController::connect() {
        std::cout << "🔌 Connecting to ZCAM..." << std::endl;
        
        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cout << "❌ Failed to allocate format context" << std::endl;
            return false;
        }
        
        // Set RTSP options - keep it simple but effective
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);  // 10 second timeout
        av_dict_set(&options, "max_delay", "3000000", 0);   // 3 second max delay
        
        // Open the stream
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open stream: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ Connected to RTSP stream" << std::endl;
        
        // Skip stream info detection - it's causing segfault
        std::cout << "⚠️ Skipping stream info analysis (causes segfault with this camera)" << std::endl;
        std::cout << "🔍 Using manual stream detection..." << std::endl;
        
        // Check basic stream count first
        std::cout << "📊 Found " << format_ctx->nb_streams << " streams" << std::endl;
        
        if (format_ctx->nb_streams == 0) {
            std::cout << "❌ No streams found in RTSP feed" << std::endl;
            return false;
        }
        
        // Try to find video stream without calling avformat_find_stream_info
        if (!findVideoStreamManually()) {
            return false;
        }
        
        if (video_stream_index == -1) {
            std::cout << "❌ No video stream found" << std::endl;
            return false;
        }
        
        std::cout << "✅ Stream detection and decoder setup complete" << std::endl;
        return true;
    }
    
    bool ZCAMController::setupDecoderForStream() {
        std::cout << "🔧 Setting up H.264 decoder for stream #" << video_stream_index << "..." << std::endl;
        
        // Create H.264 decoder directly
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cout << "❌ H.264 codec not available" << std::endl;
            return false;
        }
        
        std::cout << "✅ Found H.264 codec: " << codec->name << std::endl;
        
        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cout << "❌ Failed to allocate codec context" << std::endl;
            return false;
        }
        
        // Set minimal required parameters
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_H264;
        
        // Open the codec - it will auto-detect parameters from the stream
        int ret = avcodec_open2(codec_ctx, codec, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open H.264 codec: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ H.264 decoder ready (parameters will be detected from stream)" << std::endl;
        return true;
    }
    
    bool ZCAMController::findVideoStreamManually() {
        std::cout << "🔍 Manual stream detection..." << std::endl;
        
        // First, check stream parameters directly if available
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            AVStream *stream = format_ctx->streams[i];
            if (stream && stream->codecpar) {
                std::cout << "   Stream #" << i << ": codec_type=" << stream->codecpar->codec_type;
                std::cout << " codec_id=" << stream->codecpar->codec_id;
                std::cout << " size=" << stream->codecpar->width << "x" << stream->codecpar->height;
                std::cout << " (VIDEO=" << AVMEDIA_TYPE_VIDEO << ")" << std::endl;
                
                if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video_stream_index = i;
                    std::cout << "✅ Found video stream at index " << i << " (direct check)" << std::endl;
                    return true;
                }
            } else {
                std::cout << "   Stream #" << i << ": NULL codecpar - needs packet analysis" << std::endl;
            }
        }
        
        // If direct check failed, try packet-based detection and codec discovery
        std::cout << "🔍 Trying packet-based detection with codec analysis..." << std::endl;
        
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) {
            std::cout << "❌ Failed to allocate packet" << std::endl;
            return false;
        }
        
        std::vector<int> stream_sizes(format_ctx->nb_streams, 0);
        std::vector<int> stream_counts(format_ctx->nb_streams, 0);
        
        // Read packets and try to identify codecs
        for (int i = 0; i < 30; i++) { 
            int ret = av_read_frame(format_ctx, pkt);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                std::cout << "   Read error on packet " << i << ": " << errbuf << std::endl;
                break;
            }
            
            if (pkt->stream_index < static_cast<int>(stream_sizes.size())) {
                stream_sizes[pkt->stream_index] += pkt->size;
                stream_counts[pkt->stream_index]++;
                
                // Try to detect H.264 pattern in large packets (likely video)
                if (pkt->size > 1000) {
                    // Look for H.264 NAL unit start codes
                    bool has_nal_header = false;
                    if (pkt->size >= 4) {
                        uint8_t *data = pkt->data;
                        // Check for 0x00000001 (4-byte start code) or 0x000001 (3-byte start code)
                        if ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
                            (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01)) {
                            has_nal_header = true;
                        }
                    }
                    
                    if (has_nal_header && video_stream_index == -1) {
                        video_stream_index = pkt->stream_index;
                        std::cout << "   🎬 Detected H.264 video in stream #" << pkt->stream_index 
                                 << " (NAL units found)" << std::endl;
                        
                        // Try to populate basic codec info for this stream
                        AVStream *stream = format_ctx->streams[pkt->stream_index];
                        if (stream && stream->codecpar) {
                            stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                            stream->codecpar->codec_id = AV_CODEC_ID_H264;
                            std::cout << "   📝 Set codec info: H.264 video" << std::endl;
                        }
                    }
                }
            }
            
            av_packet_unref(pkt);
        }
        
        // Print analysis results
        for (size_t i = 0; i < stream_sizes.size(); i++) {
            std::cout << "   Stream #" << i << ": " << stream_counts[i] 
                     << " packets, " << stream_sizes[i] << " bytes total";
            if (static_cast<int>(i) == video_stream_index) {
                std::cout << " (IDENTIFIED AS VIDEO)";
            }
            std::cout << std::endl;
        }
        
        av_packet_free(&pkt);
        
        if (video_stream_index >= 0) {
            std::cout << "✅ Video stream identified: #" << video_stream_index << std::endl;
            
            // Set up decoder immediately after identifying the stream
            if (!setupDecoderForStream()) {
                std::cout << "❌ Failed to setup decoder for identified stream" << std::endl;
                video_stream_index = -1;
                return false;
            }
            
            return true;
        }
        
        // Last resort - assume stream 0 is video if it has substantial data
        for (size_t i = 0; i < stream_sizes.size(); i++) {
            if (stream_sizes[i] > 50000) { // At least 50KB suggests video
                video_stream_index = static_cast<int>(i);
                std::cout << "⚠️ Assuming stream #" << i << " is video based on data size" << std::endl;
                
                // Force set codec parameters
                AVStream *stream = format_ctx->streams[i];
                if (stream && stream->codecpar) {
                    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    stream->codecpar->codec_id = AV_CODEC_ID_H264; // Most common for IP cameras
                    std::cout << "   📝 Forced codec info: H.264 video" << std::endl;
                }
                return true;
            }
        }
        
        std::cout << "❌ Could not identify video stream" << std::endl;
        return false;
    }
    
    bool ZCAMController::setupDecoder() {
        std::cout << "🔧 Setting up decoder..." << std::endl;
        
        if (video_stream_index < 0 || video_stream_index >= (int)format_ctx->nb_streams) {
            std::cout << "❌ Invalid video stream index: " << video_stream_index << std::endl;
            return false;
        }
        
        AVStream *video_stream = format_ctx->streams[video_stream_index];
        if (!video_stream) {
            std::cout << "❌ Invalid video stream" << std::endl;
            return false;
        }
        
        // Try to use existing codecpar if available
        AVCodecParameters *codec_params = video_stream->codecpar;
        if (codec_params) {
            std::cout << "📊 Stream codecpar info:" << std::endl;
            std::cout << "   Codec ID: " << codec_params->codec_id << std::endl;
            std::cout << "   Codec type: " << codec_params->codec_type << std::endl;
            std::cout << "   Width: " << codec_params->width << std::endl;
            std::cout << "   Height: " << codec_params->height << std::endl;
        }
        
        // For H.264, try to create decoder directly
        std::cout << "🎯 Attempting H.264 decoder setup..." << std::endl;
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            std::cout << "❌ H.264 codec not found" << std::endl;
            return false;
        }
        
        std::cout << "✅ Found H.264 codec: " << codec->name << std::endl;
        
        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            std::cout << "❌ Failed to allocate codec context" << std::endl;
            return false;
        }
        
        // Set basic parameters manually for H.264
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_H264;
        
        // If we have codec parameters, use them
        if (codec_params && codec_params->width > 0 && codec_params->height > 0) {
            std::cout << "📋 Using existing codec parameters" << std::endl;
            int ret = avcodec_parameters_to_context(codec_ctx, codec_params);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                std::cout << "⚠️ Failed to copy codec parameters: " << errbuf << std::endl;
                // Continue anyway - decoder might auto-detect
            }
        } else {
            std::cout << "⚠️ No codec parameters available, decoder will auto-detect" << std::endl;
        }
        
        // Try to open codec
        int ret = avcodec_open2(codec_ctx, codec, nullptr);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cout << "❌ Failed to open codec: " << errbuf << std::endl;
            return false;
        }
        
        std::cout << "✅ Decoder opened successfully" << std::endl;
        std::cout << "   Codec: " << codec->name << std::endl;
        
        // Dimensions might not be available until first frame
        if (codec_ctx->width > 0 && codec_ctx->height > 0) {
            std::cout << "   Resolution: " << codec_ctx->width << "x" << codec_ctx->height << std::endl;
        } else {
            std::cout << "   Resolution: Will be determined from first frame" << std::endl;
        }
        
        if (codec_ctx->pix_fmt != AV_PIX_FMT_NONE) {
            std::cout << "   Pixel format: " << av_get_pix_fmt_name(codec_ctx->pix_fmt) << std::endl;
        } else {
            std::cout << "   Pixel format: Will be determined from first frame" << std::endl;
        }
        
        return true;
    }
    
    bool ZCAMController::captureOneFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
        if (!format_ctx || !codec_ctx) {
            std::cout << "❌ Not connected" << std::endl;
            return false;
        }
        
        std::cout << "📷 Capturing frame..." << std::endl;
        
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        AVFrame *rgb_frame = av_frame_alloc();
        
        if (!packet || !frame || !rgb_frame) {
            std::cout << "❌ Failed to allocate memory" << std::endl;
            if (packet) av_packet_free(&packet);
            if (frame) av_frame_free(&frame);
            if (rgb_frame) av_frame_free(&rgb_frame);
            return false;
        }
        
        bool success = false;
        int packets_read = 0;
        
        // Read packets until we get a decoded frame
        while (packets_read < 200) { // Safety limit
            int ret = av_read_frame(format_ctx, packet);
            packets_read++;
            
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                std::cout << "❌ Read error after " << packets_read << " packets: " << errbuf << std::endl;
                break;
            }
            
            // Process video packets only
            if (packet->stream_index == video_stream_index) {
                // Send packet to decoder
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret == 0) {
                    // Try to receive a frame
                    ret = avcodec_receive_frame(codec_ctx, frame);
                    if (ret == 0) {

                        someFFMpeg::saveAVFrameAsJPEG(frame, string("stream.jpg"), 100);

                        // We got a frame! Convert it to RGB
                        width = frame->width;
                        height = frame->height;
                        
                        std::cout << "🎬 Frame decoded: " << width << "x" << height 
                                 << " (after " << packets_read << " packets)" << std::endl;
                        
                        // Setup color conversion
                        sws_ctx = sws_getContext(
                            width, height, (AVPixelFormat)frame->format,
                            width, height, AV_PIX_FMT_RGB24,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                        );
                        
                        if (sws_ctx) {
                            // Allocate RGB buffer
                            int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
                            rgb_data.resize(rgb_size);
                            
                            // Setup RGB frame
                            av_image_fill_arrays(rgb_frame->data, rgb_frame->linesize,
                                                rgb_data.data(), AV_PIX_FMT_RGB24, width, height, 1);
                            
                            // Convert to RGB
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
                                    rgb_frame->data, rgb_frame->linesize);
                            
                            std::cout << "✅ Frame converted to RGB (" << rgb_size << " bytes)" << std::endl;
                            success = true;
                        }
                        break;
                    } else if (ret != AVERROR(EAGAIN)) {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
                        std::cout << "⚠️ Decode error: " << errbuf << std::endl;
                    }
                }
            }
            
            av_packet_unref(packet);
        }
        
        // Cleanup
        av_packet_free(&packet);
        av_frame_free(&frame);
        av_frame_free(&rgb_frame);
        
        if (!success) {
            std::cout << "❌ Failed to capture frame after " << packets_read << " packets" << std::endl;
        }
        
        return success;
    }
    
    // Getters for current settings
    CameraState getCameraState() { return camera_state; }
    bool getAutoAdjustEnabled() { return auto_adjust_enabled; }
    double getConfidenceThreshold() { return confidence_threshold; }
    int getCurrentISO() const { return camera_state.current_iso; }
    double getCurrentEV() const { return camera_state.current_ev; }
    std::string getCurrentAperture() const { return camera_state.current_aperture; }
    int getCurrentShutterAngle() const { return camera_state.current_shutter_angle; }

    nlohmann::json getOptions() {
        nlohmann::json options;
        options["iso_options"] = camera_state.iso_options;
        options["iris_options"] = camera_state.iris_options;
        options["shutter_options"] = camera_state.shutter_options;
        options["target_brightness"] = camera_state.target_brightness;
        options["brightness_range"] = "112-144";
        options["contrast_range"] = "25-60";
        return options;
    }

    nlohmann::json toJson() { 
        nlohmann::json params;
        params["iso"] = camera_state.current_iso;
        params["iris"] = camera_state.current_iris;
        params["shutter"] = camera_state.current_shutter_angle;
        params["mean_brightness"] = exposure_metrics.mean_brightness;
        params["contrast"] = exposure_metrics.contrast;
        params["exposure_score"] = exposure_metrics.exposure_score;
        return params;
    }

    
    void ZCAMController::cleanup() {
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
        std::cout << "🧹 Cleaned up" << std::endl;
    }

    someNetwork::Response getRequest(const string& endpoint, const string& method = "GET", const string& data = "") {

        std::cout << "🌐 HTTP Request: " << endpoint << std::endl;

        someNetwork network;

        auto response = network.http_get(camera_ip, endpoint);

        cout << "HTTP Response: " << response.str << " " << response.status << endl;
        
        return response;
    }

    bool ZCAMController::getCurrentCameraSettings() {

        cout << "🔍 Reading current ZCAM E8 Z2 settings..." << endl;
        
        // Get current ISO - using your working JS format
        auto resp = getRequest("/ctrl/get?k=iso");
        if (resp.status == 200) {
            if (resp.json.count("value") > 0) 
                camera_state.current_iso = stoi(resp.json["value"].get<string>());
            camera_state.iso_options = resp.json["opts"]; 
        }

        resp = getRequest("/ctrl/get?k=iris");
        if (resp.status == 200 && resp.json.count("value") > 0) {
            camera_state.current_aperture = resp.json["value"].get<string>();
            camera_state.current_iris = stod(camera_state.current_aperture); 
            camera_state.iris_options = resp.json["opts"]; 
        }

        resp = getRequest("/ctrl/get?k=shutter_angle");
        if (resp.status == 200 && resp.json.count("value") > 0) {
            auto shutter = resp.json["value"].get<string>();
            camera_state.shutter_options = resp.json["opts"];
            camera_state.current_shutter_angle = shutter == "Auto" ? 0 : stoi(shutter);
        }

        // resp = getRequest("/ctrl/get?k=ev");
        // if (resp.status == 200) {
        //     double ev_steps = 0.0;
        //     if (resp.json.count("value") > 0) {
        //         ev_steps = resp.json["value"].get<double>(); 
        //         camera_state.current_ev = ev_steps / 10.0;  // Convert to actual EV                
        //     }
        //     cout << "   📊 Current EV: " << std::showpos << camera_state.current_ev << std::noshowpos << " (steps: " << ev_steps << ")" << std::endl;
        // } else {
        //     cout << "   ⚠️ Could not read EV (HTTP " << resp.status << ")" << endl;
        // }
        
        // Show available ISO options
        // if (root.isMember("opts") && root["opts"].isArray()) {
        //     std::cout << "   🎚️ Available ISOs: ";
        //     for (const auto& iso_opt : root["opts"]) {
        //         std::cout << iso_opt.asString() << " ";
        //     }
        //     std::cout << std::endl;
        // } else {
        //     std::cout << "   ❌ Unexpected ISO response format" << std::endl;
        //     std::cout << "   Response: " << resp.data << std::endl;
        // }

        return resp.status == 200;

        // Get white balance for context
        // auto wb_resp = getRequest("/ctrl/get?k=wb");
        // if (wb_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(wb_resp.data, root) && root.isMember("value")) {
        //         std::cout << "   📊 White Balance: " << root["value"].asString() << std::endl;
        //     }
        // }
        
        // Get manual white balance if available
        // auto mwb_resp = getRequest("/ctrl/get?k=mwb");
        // if (mwb_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(mwb_resp.data, root) && root.isMember("value")) {
        //         std::cout << "   📊 Manual WB: " << root["value"].asString() << "K" << std::endl;
        //     }
        // }
        
        // Get camera temperature
        // auto temp_resp = getRequest("/ctrl/temperature");
        // if (temp_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(temp_resp.data, root)) {
        //         std::cout << "   🌡️ Camera Temp: " << temp_resp.data << std::endl;
        //     }
        // }
        
        // Check recording status  
        // auto rec_resp = getRequest("/ctrl/get?k=rec");
        // if (rec_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(rec_resp.data, root) && root.isMember("value")) {
        //         std::string rec_status = root["value"].asString();
        //         std::cout << "   📹 Recording: " << (rec_status == "on" ? "🔴 RECORDING" : "⏸️ STANDBY") << std::endl;
        //     }
        // }
        
        return resp.status == 200;
    }
    
    bool ZCAMController::applySetting(const std::string& param, const std::string& value) {
        string endpoint = "/ctrl/set?" + param + "=" + value;
        // HTTPResponse response = sendHTTPRequest(endpoint);
        
        // if (response.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(response.data, root) && 
        //         root.isMember("code") && root["code"].asInt() == 0) {
        //         return true;
        //     }
        // }
        return false;
    }
    
    // HTTPResponse ZCAMController::sendHTTPRequest(const std::string& endpoint) {
    //     HTTPResponse response;
    //     if (!curl) return response;
        
    //     std::string url = http_base_url + endpoint;
        
    //     curl_easy_reset(curl);
    //     curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    //     curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    //     curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    //     curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    //     curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        
    //     CURLcode res = curl_easy_perform(curl);
    //     curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.response_code);
    //     response.success = (res == CURLE_OK && response.response_code == 200);
        
    //     return response;
    // }
    
    
    ExposureMetrics analyzeExposure(const vector<uint8_t>& rgb_data, int width, int height) {
        ExposureMetrics metrics;
        
        if (rgb_data.empty()) return metrics;
        
        metrics.total_pixels = width * height;
        
        double sum_brightness = 0.0;
        double sum_squared = 0.0;
        int highlight_count = 0;
        int shadow_count = 0;
        
        // Analyze pixels
        for (int i = 0; i < metrics.total_pixels; i++) {
            size_t pixel_idx = static_cast<size_t>(i) * 3;
            if (pixel_idx + 2 < rgb_data.size()) {
                uint8_t r = rgb_data[pixel_idx];
                uint8_t g = rgb_data[pixel_idx + 1];
                uint8_t b = rgb_data[pixel_idx + 2];
                
                uint8_t gray = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b);
                
                sum_brightness += gray;
                sum_squared += gray * gray;
                
                if (gray >= 250) highlight_count++;
                if (gray <= 5) shadow_count++;
            }
        }
        
        if (metrics.total_pixels > 0) {
            metrics.brightness = sum_brightness / metrics.total_pixels;
            
            double variance = (sum_squared / metrics.total_pixels) - (metrics.brightness * metrics.brightness);
            metrics.contrast = std::sqrt(std::max(0.0, variance));
            
            metrics.highlights_clipped = (highlight_count * 100.0) / metrics.total_pixels;
            metrics.shadows_clipped = (shadow_count * 100.0) / metrics.total_pixels;
            
            // Simplified exposure score
            double score = 100.0;
            
            // Brightness penalty
            double brightness_error = std::abs(metrics.brightness - settings.target_brightness);
            score -= std::min(brightness_error * 1.5, 50.0);
            
            // Clipping penalties
            score -= metrics.highlights_clipped * 3.0;
            score -= metrics.shadows_clipped * 2.0;
            
            // Contrast bonus/penalty
            if (metrics.contrast < 15.0) {
                score -= (15.0 - metrics.contrast) * 1.0;
            }
            
            metrics.exposure_score = std::max(0.0, std::min(100.0, score));
        }
        
        return metrics;
    }
    
    bool ZCAMController::adjustExposure(const ExposureMetrics& metrics) {
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
    
    bool ZCAMController::captureFrame(std::vector<uint8_t>& rgb_data, int& width, int& height) {
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
        
        while (packets_read < 100 && keep_running) {
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
    
    bool ZCAMController::readCurrentSettings() {
        // Get ISO
        // HTTPResponse iso_resp = sendHTTPRequest("/ctrl/get?k=iso");
        // if (iso_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(iso_resp.data, root) && root.isMember("value")) {
        //         settings.iso = std::stoi(root["value"].asString());
        //     }
        // }
        
        // Get Iris
        // HTTPResponse iris_resp = sendHTTPRequest("/ctrl/get?k=iris");
        // if (iris_resp.success) {
        //     Json::Value root;
        //     Json::Reader reader;
        //     if (reader.parse(iris_resp.data, root) && root.isMember("value")) {
        //         settings.iris = root["value"].asString();
        //     }
        // }
        
        // return iso_resp.success && iris_resp.success;

        return true;
    }
    
    bool ZCAMController::detectVideoStream() {
        AVPacket *pkt = av_packet_alloc();
        if (!pkt) return false;
        
        for (int i = 0; i < 30; i++) {
            int ret = av_read_frame(format_ctx, pkt);
            if (ret < 0) break;
            
            if (pkt->size > 1000) {
                uint8_t *data = pkt->data;
                if (pkt->size >= 4 && 
                    ((data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) ||
                     (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01))) {
                    video_stream_index = pkt->stream_index;
                    break;
                }
            }
            av_packet_unref(pkt);
        }
        
        av_packet_free(&pkt);
        
        if (video_stream_index < 0) return false;
        
        // Setup H.264 decoder
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return false;
        
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) return false;
        
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_H264;
        
        return avcodec_open2(codec_ctx, codec, nullptr) >= 0;
    }
    
    bool ZCAMController::initializeStream() {
        std::cout << "🔌 Connecting to RTSP..." << std::endl;
        
        format_ctx = avformat_alloc_context();
        if (!format_ctx) return false;
        
        AVDictionary *options = nullptr;
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "10000000", 0);
        av_dict_set(&options, "max_delay", "3000000", 0);
        
        int ret = avformat_open_input(&format_ctx, rtsp_url.c_str(), nullptr, &options);
        av_dict_free(&options);
        
        if (ret < 0) return false;
        
        // Skip stream info analysis, use manual detection
        if (!detectVideoStream()) return false;
        
        cout << "✅ RTSP stream ready" << std::endl;
        return true;
    }

    void ZCAMController::shutdown() {
        stop = true;
    }
    
    bool ZCAMController::isOperatingHours() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        
        return (tm.tm_hour >= start_hour && tm.tm_hour < end_hour);
    }

    void ZCAMController::singleRun() {

        if (!isOperatingHours()) {
            std::cout << "😴 Outside operating hours, sleeping..." << std::endl;
            return;
        }

        if (!initializeStream()) {
            std::cout << "❌ Failed to initialize stream" << std::endl;
            return;
        }
        
        if (!readCurrentSettings()) {
            std::cout << "❌ Failed to read camera settings" << std::endl;
            return;
        }
        
        cout << "✅ Current settings: ISO " << settings.iso << ", f/" << settings.iris << std::endl;
            
        std::vector<uint8_t> rgb_data;
        int width, height;
            
        if (captureFrame(rgb_data, width, height)) {
            ExposureMetrics metrics = analyzeExposure(rgb_data, width, height);    
                std::cout << "   Brightness: " << std::fixed << std::setprecision(1) 
                         << metrics.brightness << "/255, Contrast: " << metrics.contrast 
                         << ", Score: " << metrics.exposure_score << "/100" << std::endl;
                
                adjustExposure(metrics);
        } else {
            cout << "   ⚠️ Frame capture failed" << std::endl;
        }      


        nlohmann::json params;
        params["camera"] = camera_id;
        params["iso"] = camera_state.current_iso;
        params["iris"] = camera_state.current_iris;
        params["brightness"] = exposure_metrics.brightness;
        params["contrast"] = exposure_metrics.contrast;
        params["exposure"] = exposure_metrics.exposure_score;      

        someNetwork net;
        net.https_request(server, "/api/caminfo", http::verb::post, params);

    }

    void ZCAMController::runLoop() {

        while (!stop) {
            singleRun()
            std::this_thread::sleep_for(std::chrono::seconds(300)); // 5 MIN            
        }

    }



};
