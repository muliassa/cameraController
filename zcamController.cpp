#include <zcamController.h>

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <cstdint>
#include <thread>
#include <chrono>
#include <json/json.h>

#include <someLogger.h>
#include <someNetwork.h>
#include <someFFMpeg.h>

using namespace std;

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

        cout << config["ipaddr"].dump(4) << endl;
        cout << config["camera"].dump(4) << endl;

        camera_ip = config["ipaddr"][cam_idx].get<string>();
        camera_id = config["camera"][cam_idx].get<string>();

        rtsp_url = "rtsp://" + camera_ip + "/live_stream";
        http_base_url = "http://" + camera_ip + "/ctrl";

        cout << rtsp_url << endl;

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
        
        if (video_stream_index == -1) {
            std::cout << "❌ No video stream found" << std::endl;
            return false;
        }
        
        std::cout << "✅ Stream detection and decoder setup complete" << std::endl;
        return true;
    }

    nlohmann::json ZCAMController::getOptions() {
        nlohmann::json options;
        options["iso_options"] = camera_state.iso_options;
        options["iris_options"] = camera_state.iris_options;
        options["target_brightness"] = camera_state.target_brightness;
        options["brightness_range"] = "112-144";
        options["contrast_range"] = "25-60";
        return options;
    }

    nlohmann::json ZCAMController::toJson() { 
        nlohmann::json params;
        params["iso"] = camera_state.current_iso;
        params["iris"] = camera_state.current_iris;
        params["brightness"] = exposure_metrics.brightness;
        params["contrast"] = exposure_metrics.contrast;
        params["exposure"] = exposure_metrics.exposure_score;
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

    someNetwork::Response ZCAMController::httpRequest(const string& endpoint, const string& method, const string& data) {

        std::cout << "🌐 HTTP Request: " << endpoint << std::endl;

        someNetwork net;

        auto response = net.http_get(camera_ip, endpoint);

        cout << "HTTP Response: " << response.str << " " << response.status << endl;
        
        return response;
    }

    bool ZCAMController::getCurrentCameraSettings() {

        cout << "🔍 Reading current ZCAM E8 Z2 settings..." << endl;
        
        // Get current ISO - using your working JS format
        auto resp = httpRequest("/ctrl/get?k=iso");
        if (resp.status == 200) {
            if (resp.json.count("value") > 0) 
                camera_state.current_iso = stoi(resp.json["value"].get<string>());
            camera_state.iso_options = resp.json["opts"]; 
        }

        resp = httpRequest("/ctrl/get?k=iris");
        if (resp.status == 200 && resp.json.count("value") > 0) {
            camera_state.current_aperture = resp.json["value"].get<string>();
            camera_state.current_iris = stod(camera_state.current_aperture); 
            camera_state.iris_options = resp.json["opts"]; 
        }

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
    
    ExposureMetrics ZCAMController::analyzeExposure(const vector<uint8_t>& rgb_data, int width, int height) {
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

                        someFFMpeg::saveAVFrameAsJPEG(frame, string("stream.jpg"), 100);

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

    void ZCAMController::run() {
        while (!stop) {
            singleRun();
            std::this_thread::sleep_for(std::chrono::seconds(60));             
        }
    }
